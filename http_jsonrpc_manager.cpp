#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

// ============================================================================
// BuffereventPairPool — bufferevent_pair 对象池
// ============================================================================

/** @brief 池中单个槽位，持有一对 bufferevent_pair */
struct PairPoolSlot
{
    struct bufferevent*  pair[2];   ///< pair[0] 响应端，pair[1] TAP 端
    bool                 in_use;    ///< 是否正在使用中
    bool                 pair0_done;///< pair[0] 是否已归还
    bool                 pair1_done;///< pair[1] 是否已归还
    BuffereventPairPool* owner;     ///< 回指所属池，供 ReleaseHalf 归还
};

/**
 * @brief bufferevent_pair 对象池，消除高并发下的 socketpair 系统调用和堆分配开销
 *
 * 预创建固定数量的 bufferevent_pair，空闲时 O(1) 获取，两端都归还后自动回收。
 * 池耗尽时 fallback 到动态创建（不阻塞）。
 */
class BuffereventPairPool
{
public:
    BuffereventPairPool() : m_evbase(nullptr) {}

    /**
     * @brief 预创建池
     * @param evbase   libevent 事件循环基
     * @param capacity 预创建 pair 数量
     */
    void Init(struct event_base* evbase, int capacity)
    {
        m_evbase = evbase;
        m_slots.resize(capacity);
        for (int i = 0; i < capacity; i++)
        {
            auto& slot = m_slots[i];
            slot.pair[0] = nullptr;
            slot.pair[1] = nullptr;
            slot.in_use = false;
            slot.pair0_done = false;
            slot.pair1_done = false;
            slot.owner = this;

            struct bufferevent* p[2] = { nullptr, nullptr };
            if (bufferevent_pair_new(m_evbase, ZM_EVENT_BEV_OPTIONS, p) == 0)
            {
                slot.pair[0] = p[0];
                slot.pair[1] = p[1];
                m_free_stack.push_back(&slot);
            }
        }
        DEFAULT_LOG_INFO("BuffereventPairPool initialized: capacity={}, created={}",
            capacity, (int)m_free_stack.size());
    }

    /** @brief 销毁池中所有 pair */
    void Shutdown()
    {
        for (auto& slot : m_slots)
        {
            if (slot.pair[0]) { bufferevent_free(slot.pair[0]); slot.pair[0] = nullptr; }
            if (slot.pair[1]) { bufferevent_free(slot.pair[1]); slot.pair[1] = nullptr; }
        }
        m_free_stack.clear();
        m_slots.clear();
        m_evbase = nullptr;
    }

    /**
     * @brief 获取一个可用槽位（O(1) 从空闲栈弹出）
     * @return 槽位指针，池耗尽时返回 nullptr
     */
    PairPoolSlot* Acquire()
    {
        if (m_free_stack.empty())
            return nullptr;

        auto* slot = m_free_stack.back();
        m_free_stack.pop_back();
        slot->in_use = true;
        slot->pair0_done = false;
        slot->pair1_done = false;
        return slot;
    }

    /**
     * @brief 归还 pair 的某一端
     * @param slot      槽位指针
     * @param is_pair1  true 表示 pair[1]（TAP 端），false 表示 pair[0]（响应端）
     *
     * 两端都归还后清空 evbuffer 并回收到空闲栈。
     * @note 由 FreeRequesterEnd 和 OnResponseRead/Event 调用
     */
    static void ReleaseHalf(void* slot, bool is_pair1)
    {
        if (!slot) return;
        auto* s = static_cast<PairPoolSlot*>(slot);

        if (is_pair1)
            s->pair1_done = true;
        else
            s->pair0_done = true;

        if (s->pair0_done && s->pair1_done)
        {
            // 两端都归还：清空缓冲区，通过 owner 回指归还到空闲栈
            ResetPair(s);
            s->in_use = false;
            if (s->owner)
                s->owner->ReturnToFreeStack(s);
        }
    }

    /** @brief 槽位回空闲栈（由 ReleaseHalf 内部调用） */
    void ReturnToFreeStack(PairPoolSlot* slot)
    {
        m_free_stack.push_back(slot);
    }

private:
    /** @brief 重置 pair 的 evbuffer 状态，清空残留数据 */
    static void ResetPair(PairPoolSlot* slot)
    {
        if (slot->pair[0])
        {
            struct evbuffer* input = bufferevent_get_input(slot->pair[0]);
            struct evbuffer* output = bufferevent_get_output(slot->pair[0]);
            if (input)  evbuffer_drain(input, evbuffer_get_length(input));
            if (output) evbuffer_drain(output, evbuffer_get_length(output));
            // 重置水位线和回调（OnPairAcceptBev 会重新设置 pair[1]，pair[0] 在 InjectJrpcRequest 中设置）
            bufferevent_setcb(slot->pair[0], nullptr, nullptr, nullptr, nullptr);
        }
        if (slot->pair[1])
        {
            struct evbuffer* input = bufferevent_get_input(slot->pair[1]);
            struct evbuffer* output = bufferevent_get_output(slot->pair[1]);
            if (input)  evbuffer_drain(input, evbuffer_get_length(input));
            if (output) evbuffer_drain(output, evbuffer_get_length(output));
            bufferevent_setcb(slot->pair[1], nullptr, nullptr, nullptr, nullptr);
        }
    }

    struct event_base*      m_evbase;
    std::vector<PairPoolSlot> m_slots;        ///< 槽位数组（不扩容，下标稳定）
    std::vector<PairPoolSlot*> m_free_stack;  ///< 空闲槽位栈
};


// ============================================================================
// HttpJsonRpcManager
// ============================================================================

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evbase(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_hubMgr(nullptr)
    , m_channel(nullptr)
    , m_pairPool(nullptr)
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

    // 1. 创建 bufferevent_pair 对象池（预创建，减少高并发下的系统调用开销）
    if (!m_pairPool)
    {
        m_pairPool = new BuffereventPairPool();
        m_pairPool->Init(evbase, 32);
    }

    // 2. 创建内部 JRPC 请求通道（必须在 HTTP 服务器启动前，确保 Worker 线程可用）
    if (!OpenJrpcChannel(hubMgr))
    {
        DEFAULT_LOG_ERROR("OpenJrpcChannel failed");
        return false;
    }

    // 3. 创建 HTTP JSON-RPC 服务器，注册异步回调（Worker 线程不阻塞）
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

    // 销毁 pair 池（在通道关闭后，确保没有在飞的请求）
    if (m_pairPool)
    {
        m_pairPool->Shutdown();
        delete m_pairPool;
        m_pairPool = nullptr;
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
    // 尝试从对象池获取 bufferevent_pair，池耗尽则动态创建
    PairPoolSlot* slot = m_pairPool ? m_pairPool->Acquire() : nullptr;

    struct bufferevent* pair[2] = { nullptr, nullptr };
    if (slot)
    {
        pair[0] = slot->pair[0];
        pair[1] = slot->pair[1];
    }
    else
    {
        if (bufferevent_pair_new(m_evbase, ZM_EVENT_BEV_OPTIONS, pair) < 0)
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
        if (slot)
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
        if (slot)
        {
            // OnPairAcceptBev 失败时 pair[1] 已被直接 free（发生在获取 TAP 之前）
            // 释放 pair[0] 并重新创建一对放入 slot，再归还到空闲栈
            bufferevent_free(pair[0]);
            struct bufferevent* new_pair[2] = { nullptr, nullptr };
            if (bufferevent_pair_new(m_evbase, ZM_EVENT_BEV_OPTIONS, new_pair) == 0)
            {
                slot->pair[0] = new_pair[0];
                slot->pair[1] = new_pair[1];
            }
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

    auto* rctx = new ResponseReadCtx{ std::move(callback), slot, 0, false, {} };
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

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
        DEFAULT_LOG_WARN("JRPC response channel closed before full response, events={}", events);
        rctx->callback(std::string());
    }

    // 归还 pair[0]：来自池则归还槽位，否则直接释放
    if (rctx->pool_slot)
        BuffereventPairPool::ReleaseHalf(rctx->pool_slot, false);
    else
        bufferevent_free(bev);
    delete rctx;
}
