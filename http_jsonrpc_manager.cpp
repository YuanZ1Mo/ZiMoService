#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evbase(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_hubMgr(nullptr)
    , m_channel(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();
}

bool HttpJsonRpcManager::Open(struct event_base* evbase, HubProxyManager* hubMgr)
{
    if (!evbase || !hubMgr)
        return false;

    m_evbase = evbase;
    m_hubMgr = hubMgr;

    // 1. 创建内部 JRPC 请求通道（必须在 HTTP 服务器启动前，确保 Worker 线程可用）
    if (!OpenJrpcChannel(hubMgr))
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed");
        return false;
    }

    // 2. 创建 HTTP JSON-RPC 服务器，注册异步回调（Worker 线程不阻塞）
    if (nullptr == m_httpServerJRPC)
    {
        m_httpServerJRPC = new ZmJsonRpcServer(ZM_HTTPSERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT);
        m_httpServerJRPC->Start();
        m_httpServerJRPC->SetJsonRpcCBAsync(std::bind(&HttpJsonRpcManager::OnJsonRpcAsync, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4));
    }

    return (nullptr != m_httpServerJRPC);
}

void HttpJsonRpcManager::Close()
{
    // ★ 先关通道（拒绝所有 pending promise），再停 HTTP 服务器（join 线程池）
    CloseJrpcChannel();

    if (m_httpServerJRPC)
    {
        m_httpServerJRPC->Stop();
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }

    m_hubMgr = nullptr;
    m_evbase = nullptr;
}

// ============================================================================
// 内部 JRPC 请求通道
// ============================================================================

bool HttpJsonRpcManager::OpenJrpcChannel(HubProxyManager* hubMgr)
{
    if (m_channel)
    {
        DEFAULT_LOG_WARN("ZmNetRequestChannel already opened, skipping");
        return true;
    }

    if (!m_evbase)
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: evbase not set");
        return false;
    }

    if (!hubMgr || !hubMgr->HubProxy())
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: Hub not available");
        return false;
    }

    m_channel = new ZmNetRequestChannel();
    if (!m_channel->Open(m_evbase,
            std::bind(&HttpJsonRpcManager::InjectJrpcRequest, this,
                std::placeholders::_1, std::placeholders::_2)))
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed: channel Open() returned false");
        delete m_channel;
        m_channel = nullptr;
        return false;
    }

    DEFAULT_LOG_INFO("JrpcChannel opened");
    return true;
}

void HttpJsonRpcManager::CloseJrpcChannel()
{
    if (!m_channel)
        return;

    m_channel->Close(m_evbase);
    delete m_channel;
    m_channel = nullptr;

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

    if (!m_channel)
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
    m_channel->SubmitAsync(reqjs,
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
    struct bufferevent* pair[2] = { nullptr, nullptr };
    if (bufferevent_pair_new(m_evbase, ZM_EVENT_BEV_OPTIONS, pair) < 0)
    {
        DEFAULT_LOG_ERROR("InjectJrpcRequest: bufferevent_pair_new failed");
        callback(std::string());
        return;
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
        bufferevent_free(pair[0]);
        bufferevent_free(pair[1]);
        callback(std::string());
        return;
    }

    if (!ZmTapContextEventHandler::OnPairAcceptBev(m_hubMgr->HubProxy(), pair[1]))
    {
        DEFAULT_LOG_ERROR("InjectJrpcRequest: OnPairAcceptBev failed");
        bufferevent_free(pair[0]);
        callback(std::string());
        return;
    }

    auto* rctx = new ResponseReadCtx{ std::move(callback), pair[0], 0, false, {} };
    bufferevent_setcb(pair[0], HttpJsonRpcManager::OnResponseRead, nullptr,
                       HttpJsonRpcManager::OnResponseEvent, rctx);
    bufferevent_setwatermark(pair[0], EV_READ, 4, 0);
    bufferevent_enable(pair[0], EV_READ);
}

// ============================================================================
// bufferevent_pair 响应回调
// ============================================================================

void HttpJsonRpcManager::OnResponseRead(struct bufferevent* bev, void* ctx)
{
    auto* rctx = static_cast<ResponseReadCtx*>(ctx);
    struct evbuffer* input = bufferevent_get_input(bev);

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

        rctx->callback(std::move(rctx->buffer));

        bufferevent_free(rctx->pair0);
        delete rctx;
    }
}

void HttpJsonRpcManager::OnResponseEvent(struct bufferevent* bev, short events, void* ctx)
{
    auto* rctx = static_cast<ResponseReadCtx*>(ctx);

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
        DEFAULT_LOG_WARN("JRPC response channel closed before full response, events={}", events);
        rctx->callback(std::string());
    }

    bufferevent_free(rctx->pair0);
    delete rctx;
}
