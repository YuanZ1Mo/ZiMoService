#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "hub_proxy_manager.h"

#include "zm_net_request_channel.h"
#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

#include <event2/bufferevent.h>
#include <event2/buffer.h>

// ============================================================================
// HttpJsonRpcManager
// ============================================================================

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evLoop(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_hubMgr(nullptr)
    , m_hub_channel(nullptr)
    , m_pairPool(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();

    // ★ Pair 池应在析构前通过 ShutdownPairPool() 显式销毁（在 Hub 事件循环停止前调用）。
    // 此处作为兜底：若调用者未提前调用 ShutdownPairPool()，则在此处销毁。
    // 但此时 event_base 可能已释放，bufferevent_free 存在崩溃风险，
    // 因此调用者须确保在 delete 前已通过 NetDock::CloseHub() → HubProxyManager::Close(beforeLoopStop)
    // 路径提前销毁 pair 池。
    if (m_pairPool != nullptr)
    {
        m_pairPool->Shutdown();
        delete m_pairPool;
        m_pairPool = nullptr;
    }
}

bool HttpJsonRpcManager::Open(HubProxyManager* hubMgr)
{
    if (hubMgr == nullptr)
        return false;

    m_hubMgr = hubMgr;

    // 1. 创建 bufferevent_pair 对象池（预创建，减少高并发下的系统调用开销）
    //    ★ 注意：pair pool 使用 Hub 的事件循环，因为 pair[1] 要注入 Hub 代理链
    if (m_pairPool == nullptr)
    {
        m_pairPool = new BuffereventPairPool();
        m_pairPool->Init(hubMgr->EvBase(), 128);
    }

    // 2. 创建内部 JRPC 请求通道（走 Hub 事件循环，因需要注入 Hub 代理链）
    if (!OpenJrpcChannel(hubMgr))
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed");
        return false;
    }

    // 3. 创建自有事件循环线程（供 HTTP JRPC 服务器使用，与 Hub 事件循环独立）
    if (m_evLoop == nullptr)
    {
        m_evLoop = new ZmEvBaseRunLoop("JrpcHttpServerLoop");
        if (!m_evLoop->Loop())
        {
            DEFAULT_LOG_ERROR("JRPC HTTP 事件循环启动失败");
            delete m_evLoop;
            m_evLoop = nullptr;
            return false;
        }
    }

    // 4. 创建 HTTP JSON-RPC 服务器（绑定到自有事件循环），注册异步回调
    if (m_httpServerJRPC == nullptr)
    {
        m_httpServerJRPC = new ZmJsonRpcServer(m_evLoop->GetEventBase(),
            ZM_HTTPSERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT);
        if (!m_httpServerJRPC->Init())
        {
            DEFAULT_LOG_ERROR("JRPC HTTP 服务器初始化失败");
            delete m_httpServerJRPC;
            m_httpServerJRPC = nullptr;
            m_evLoop->Stop();
            delete m_evLoop;
            m_evLoop = nullptr;
            return false;
        }
        m_httpServerJRPC->SetJsonRpcCBAsync(std::bind(&HttpJsonRpcManager::OnJsonRpcAsync, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4));
    }

    return (m_httpServerJRPC != nullptr);
}

void HttpJsonRpcManager::Close()
{
    // ★ 先关通道（拒绝所有 pending promise）
    CloseJrpcChannel();

    // 再停 HTTP 服务器（join 线程池，释放 evhttp）
    if (m_httpServerJRPC != nullptr)
    {
        m_httpServerJRPC->Close();
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }

    // 再停自有事件循环（evhttp 已释放，evbase 安全释放）
    if (m_evLoop != nullptr)
    {
        m_evLoop->Stop();
        delete m_evLoop;
        m_evLoop = nullptr;
    }

    // ★ Pair 池不在此处销毁 — 已注入 Hub 链的在飞请求仍需 pair 完成响应回写
    // Pair 池的销毁通过 ShutdownPairPool() 在 Hub 事件循环停止前执行

    m_hubMgr = nullptr;
}

void HttpJsonRpcManager::ShutdownPairPool()
{
    if (m_pairPool != nullptr)
    {
        // bufferevent_free 内部调用 event_del 需 event_base 存活，
        // 调用者须确保在 Hub 事件循环停止前调用本方法
        m_pairPool->Shutdown();
        delete m_pairPool;
        m_pairPool = nullptr;
    }
}

// ============================================================================
// 内部 JRPC 请求通道
// ============================================================================

bool HttpJsonRpcManager::OpenJrpcChannel(HubProxyManager* hubMgr)
{
    if (m_hub_channel != nullptr)
    {
        DEFAULT_LOG_WARN("ZmNetRequestChannel already opened, skipping");
        return true;
    }

    if (hubMgr == nullptr || hubMgr->HubProxy() == nullptr)
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: Hub not available");
        return false;
    }

    if (m_hubMgr->EvBase() == nullptr)
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: evbase not set");
        return false;
    }

    m_hub_channel = new ZmNetRequestChannel();
    if (!m_hub_channel->Open(m_hubMgr->EvBase(),
            std::bind(&HttpJsonRpcManager::InjectJrpcRequest, this,
                std::placeholders::_1, std::placeholders::_2)))
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: channel Open() returned false");
        delete m_hub_channel;
        m_hub_channel = nullptr;
        return false;
    }

    DEFAULT_LOG_INFO("JrpcChannel opened");
    return true;
}

void HttpJsonRpcManager::CloseJrpcChannel()
{
    if (m_hub_channel == nullptr)
        return;

    m_hub_channel->Close(m_hubMgr->EvBase());
    delete m_hub_channel;
    m_hub_channel = nullptr;

    DEFAULT_LOG_INFO("JrpcChannel closed");
}

// ============================================================================
// 异步 JRPC 请求处理（Worker 线程不阻塞）
// ============================================================================

/**
 * @brief JSON-RPC 异步回调入口（由 ZmJsonRpcServer 在 Worker 线程中调用，立即返回）
 *
 * 构建请求 JSON → 通过 SubmitAsync 提交到事件循环线程 → Worker 立即返回。
 * 事件循环线程处理完成后，响应回调直接在事件循环线程中触发 reply()。
 * 全程零额外线程：Worker 不阻塞，也无 detach 等待线程。
 */
void HttpJsonRpcManager::OnJsonRpcAsync(ZmHttpdTask* task, const std::string& method,
                                         const ZMJSON& params,
                                         std::function<void(const ZMJSON&, const ZMJSON&)> reply)
{
    // 1. 构建内部请求 JSON（携带 method / ip / port / useragent / params）
    ZMJSON reqobj;
    reqobj["method"] = method;
    reqobj["ip"] = task->Ip();
    reqobj["port"] = task->Port();
    reqobj["request_useragent"] = task->GetRequestHeader("User-Agent");
    reqobj["params"] = params;

    if (m_hub_channel == nullptr)
    {
        ZMJSON err;
        err["code"] = ZM_JRPC_ERR_SEND_HUB;
        err["message"] = "Internal channel not available";
        reply(ZMJSON(), err);
        return;
    }

    std::string reqjs = reqobj.dump();

    // 2. SubmitAsync：请求入队 + 注册直接回调，Worker 线程立即返回
    //    回调在事件循环线程中触发（Drain → InjectJrpcRequest → Hub → Response → OnResponseRead → callback）
    //    用 shared_ptr 包装 reply 避免 move-only lambda 导致 std::function 构造失败
    auto replyPtr = std::make_shared<std::function<void(const ZMJSON&, const ZMJSON&)>>(std::move(reply));
    m_hub_channel->SubmitAsync(reqjs,
        [replyPtr](std::string rspjs) {
            auto& reply = *replyPtr;

            if (rspjs.empty())
            {
                ZMJSON err;
                err["code"] = ZM_JRPC_ERR_EMPTY_RSP;
                err["message"] = "Response is empty";
                reply(ZMJSON(), err);
                return;
            }

            std::string jerrstr;
            ZMJSON repjson = zm_json_parse(rspjs, jerrstr);
            if (!repjson.is_object())
            {
                ZMJSON err;
                err["code"] = ZM_JRPC_ERR_FORMAT;
                err["message"] = "Response format error";
                reply(ZMJSON(), err);
                return;
            }

            reply(repjson["result"], repjson["error"]);
        });
}

// ============================================================================
// bufferevent_pair + Hub 注入
// ============================================================================

void HttpJsonRpcManager::InjectJrpcRequest(const std::string& request_json,
                                            std::function<void(std::string)> callback)
{
    // 从对象池获取 bufferevent_pair（池耗尽时自动扩容，仅内存不足时返回 nullptr）
    PairPoolSlot* slot = m_pairPool ? m_pairPool->Acquire() : nullptr;

    struct bufferevent* pair[2] = { nullptr, nullptr };
    if (slot != nullptr)
    {
        pair[0] = slot->pair[0];
        pair[1] = slot->pair[1];
    }
    else
    {
        if (bufferevent_pair_new(m_hubMgr->EvBase(), ZM_EVENT_BEV_OPTIONS, pair) < 0)
        {
            DEFAULT_LOG_ERROR("InjectJrpcRequest: bufferevent_pair_new failed");
            callback(std::string());
            return;
        }
    }

    char head[8];
    memcpy(head, "JRPC", 4);
    uint32_t qlen = htonl((uint32_t)request_json.size());
    memcpy(head + 4, &qlen, 4);

    struct evbuffer* output = bufferevent_get_output(pair[0]);
    if (evbuffer_add(output, head, 8) < 0 ||
        evbuffer_add(output, request_json.data(), request_json.size()) < 0)
    {
        DEFAULT_LOG_ERROR("InjectJrpcRequest: evbuffer_add failed");
        if (slot != nullptr)
        {
            // 归还槽位（写失败，尚未送出，两端都未用）
            slot->in_use = false;
            m_pairPool->ReturnToFreeStack(slot);
        }
        else
        {
            bufferevent_free(pair[0]);
            bufferevent_free(pair[1]);
        }
        callback(std::string());
        return;
    }

    // 注入 Hub：传递 slot 和归还回调，TAP 释放时通过回调归还 pair[1]
    if (!ZmTapContextEventHandler::OnPairAcceptBev(m_hubMgr->HubProxy(), pair[1],
            slot, &BuffereventPairPool::ReleaseHalf))
    {
        DEFAULT_LOG_ERROR("InjectJrpcRequest: OnPairAcceptBev failed");
        if (slot != nullptr)
        {
            // OnPairAcceptBev 失败时，pool 路径下 pair[1] 已被 ReleaseHalf 软归还
            // （仅 pair1_done=true，未实际 free），此处需同时释放 pair[0] 和 pair[1]
            // 再重新创建一对放入 slot，归还到空闲栈
            bufferevent_free(pair[0]);
            bufferevent_free(pair[1]);

            struct bufferevent* new_pair[2] = { nullptr, nullptr };
            if (bufferevent_pair_new(m_hubMgr->EvBase(), ZM_EVENT_BEV_OPTIONS, new_pair) == 0)
            {
                slot->pair[0] = new_pair[0];
                slot->pair[1] = new_pair[1];
            }
            else
            {
                // 创建失败时清空指针，防止下次 Acquire 拿到悬空指针
                slot->pair[0] = nullptr;
                slot->pair[1] = nullptr;
            }
            slot->pair0_done = false;
            slot->pair1_done = false;
            slot->in_use = false;
            m_pairPool->ReturnToFreeStack(slot);
        }
        else
        {
            bufferevent_free(pair[0]);
            // pair[1] 已被 OnPairAcceptBev 在失败路径中释放
        }
        callback(std::string());
        return;
    }

    auto* rctx = new ResponseReadCtx{ std::move(callback), slot, 0, false, false, {} };
    bufferevent_setcb(pair[0], HttpJsonRpcManager::OnResponseRead, nullptr,
                       HttpJsonRpcManager::OnResponseEvent, rctx);
    bufferevent_setwatermark(pair[0], EV_READ, 4, 0);
    bufferevent_enable(pair[0], EV_READ | EV_WRITE);
}

// ============================================================================
// bufferevent_pair 响应回调
// ============================================================================

void HttpJsonRpcManager::OnResponseRead(struct bufferevent* bev, void* ctx)
{
    auto* rctx = static_cast<ResponseReadCtx*>(ctx);
    struct evbuffer* input = bufferevent_get_input(bev);

    // ★ 防止双回调：WriteResponse 在写响应后立即 Drop TAP，
    //    可能触发 ReleaseHalf(pair1)→bufferevent_trigger_event(BEV_EVENT_EOF)，
    //    若 BEV_EVENT_EOF 在 OnResponseRead 之后到达，OnResponseEvent 会再次调用 callback。
    if (rctx->callback_fired)
        return;

    if (!rctx->header_read)
    {
        if (evbuffer_get_length(input) < 4)
            return;

        uint32_t len;
        evbuffer_remove(input, &len, 4);
        rctx->response_len = ntohl(len);
        rctx->header_read = true;
    }

    size_t available = evbuffer_get_length(input);
    if (rctx->header_read && available >= rctx->response_len)
    {
        rctx->buffer.resize(rctx->response_len);
        evbuffer_remove(input, &rctx->buffer[0], rctx->response_len);

        rctx->callback_fired = true;
        rctx->callback(std::move(rctx->buffer));

        // 归还 pair[0]：来自池则归还槽位，否则直接释放
        if (rctx->pool_slot)
            BuffereventPairPool::ReleaseHalf(rctx->pool_slot, false);
        else
            bufferevent_free(bev);
        delete rctx;
    }
}

void HttpJsonRpcManager::OnResponseEvent(struct bufferevent* bev, short events, void* ctx)
{
    auto* rctx = static_cast<ResponseReadCtx*>(ctx);

    // ★ 防止双回调：若 OnResponseRead 已触发（在回调执行后、ResetPair 清理前），
    //    BEV_EVENT_EOF 可能在此期间到达。ResetPair 会将 errorcb 置 nullptr，
    //    此后 BEV_EVENT_EOF 不再触发本回调。此 guard 覆盖窄窗口竞态。
    if (rctx->callback_fired)
        return;

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
        DEFAULT_LOG_WARN("JRPC response channel closed before full response, events={}", events);
        rctx->callback_fired = true;
        rctx->callback(std::string());
    }

    // 归还 pair[0]：来自池则归还槽位，否则直接释放
    if (rctx->pool_slot)
        BuffereventPairPool::ReleaseHalf(rctx->pool_slot, false);
    else
        bufferevent_free(bev);
    delete rctx;
}
