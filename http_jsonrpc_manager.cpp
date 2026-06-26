#include "http_jsonrpc_manager.h"

#include "service_define.h"
#include "hub_proxy_manager.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

#include <event2/bufferevent.h>
#include <event2/buffer.h>

// ============================================================================
// HttpJsonRpcManager
// ============================================================================

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evLoopHttpServerJRPC(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_evLoopPairPool(nullptr)
    , m_pairPool(nullptr)
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();
    ShutdownPairPool();  // 兜底：若 NetDock 未通过 CloseHub 回调提前销毁
}

bool HttpJsonRpcManager::Open()
{
    if (!m_evLoopPairPool)
    {
        m_evLoopPairPool = new ZmEvBaseRunLoop("JRPCPairPoolLoop");
        if (!m_evLoopPairPool->Loop())
        {
            delete m_evLoopPairPool;
            m_evLoopPairPool = nullptr;
            return false;
        }
    }

    // 1. 创建 bufferevent_pair 对象池（预创建，减少高并发下的系统调用开销）
    if (m_pairPool == nullptr)
    {
        m_pairPool = new ZmBuffereventPairPool();
        m_pairPool->Init(m_evLoopPairPool->GetEventBase(), 128);
    }

    // 2. 创建自有事件循环线程（供 HTTP JRPC 服务器使用，与 Hub 事件循环独立）
    if (m_evLoopHttpServerJRPC == nullptr)
    {
        m_evLoopHttpServerJRPC = new ZmEvBaseRunLoop("JrpcHttpServerLoop");
        if (!m_evLoopHttpServerJRPC->Loop())
        {
            DEFAULT_LOG_ERROR("JRPC HTTP 事件循环启动失败");
            delete m_evLoopHttpServerJRPC;
            m_evLoopHttpServerJRPC = nullptr;
            return false;
        }
    }

    // 3. 创建 HTTP JSON-RPC 服务器（绑定到自有事件循环），注册异步回调
    if (m_httpServerJRPC == nullptr)
    {
        m_httpServerJRPC = new ZmJsonRpcServer(m_evLoopHttpServerJRPC->GetEventBase(),
            ZM_HTTPSERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT);
        if (!m_httpServerJRPC->Init())
        {
            DEFAULT_LOG_ERROR("JRPC HTTP 服务器初始化失败");
            delete m_httpServerJRPC;
            m_httpServerJRPC = nullptr;
            m_evLoopHttpServerJRPC->Stop();
            delete m_evLoopHttpServerJRPC;
            m_evLoopHttpServerJRPC = nullptr;
            return false;
        }
        m_httpServerJRPC->SetJsonRpcCBAsync(std::bind(&HttpJsonRpcManager::OnJsonRpcCBAsync, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3));
    }

    return (m_httpServerJRPC != nullptr);
}

void HttpJsonRpcManager::Close()
{
    // ★ 仅软关闭 HTTP 前端，不碰 pair 池
    // Pair 池由 ShutdownPairPool() 单独销毁，在 Hub 清完 TAP 之后调用

    // 停 HTTP 服务器（join 线程池，释放 evhttp）
    if (m_httpServerJRPC != nullptr)
    {
        m_httpServerJRPC->Close();
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }

    // 停自有事件循环（evhttp 已释放，evbase 安全释放）
    if (m_evLoopHttpServerJRPC != nullptr)
    {
        m_evLoopHttpServerJRPC->Stop();
        delete m_evLoopHttpServerJRPC;
        m_evLoopHttpServerJRPC = nullptr;
    }
}

void HttpJsonRpcManager::ShutdownPairPool()
{
    if (m_pairPool != nullptr)
    {
        m_pairPool->Shutdown();
        delete m_pairPool;
        m_pairPool = nullptr;
    }

    if (m_evLoopPairPool != nullptr)
    {
        m_evLoopPairPool->Stop();
        delete m_evLoopPairPool;
        m_evLoopPairPool = nullptr;
    }
}



// ============================================================================
// 异步 JRPC 请求处理（Worker 线程，直接写 pair，零中间队列）
// ============================================================================

void HttpJsonRpcManager::OnJsonRpcCBAsync(ZmHttpdTask* task, const ZMJSON& request,
    std::function<void(const ZMJSON& response)> replyCB)
{
    // 堆分配，配合 evbuffer_add_reference 零拷贝写入 evbuffer
    std::string* reqjs = new std::string(request.dump());  // 拷贝①: JSON树→堆string（必要开销）

    // 1. 从池获取 bufferevent_pair
    ZmBuffereventPairHandle* handle = m_pairPool ? m_pairPool->Acquire() : nullptr;

    if (handle == nullptr)
    {
        delete reqjs;
        replyCB(ZMJSON());
        return;
    }

    // 2. 写请求到 pair[0] 输出端：[4 字节 "JRPC"][4 字节大端长度][请求 JSON]
    char head[8];
    memcpy(head, "JRPC", 4);
    uint32_t qlen = htonl((uint32_t)reqjs->size());
    memcpy(head + 4, &qlen, 4);

    struct evbuffer* output = bufferevent_get_output(handle->bev0);

    // 8 字节帧头：小数据 evbuffer_add 直接拷贝
    if (evbuffer_add(output, head, 8) < 0)
    {
        DEFAULT_LOG_ERROR("OnJsonRpcAsync: evbuffer_add header failed");
        delete reqjs;
        handle->ReleasePair();
        replyCB(ZMJSON());
        return;
    }

    // JSON body：evbuffer_add_reference 零拷贝引用，libevent 消费后自动 delete
    if (evbuffer_add_reference(output, reqjs->data(), reqjs->size(),
            [](const void*, size_t, void* extra) {
                delete static_cast<std::string*>(extra);
            }, reqjs) < 0)
    {
        DEFAULT_LOG_ERROR("OnJsonRpcAsync: evbuffer_add_reference failed");
        delete reqjs;
        handle->ReleasePair();
        replyCB(ZMJSON());
        return;
    }

    // 3. pair[1] 注入 Hub 代理链，携带真实请求 IP（v4/v6 自适应）
    struct sockaddr_storage srcAddr = {};
    std::string ip = task->Ip();
    if (ip.find(':') != std::string::npos)
    {
        auto* addr6 = (struct sockaddr_in6*)&srcAddr;
        addr6->sin6_family = AF_INET6;
        evutil_inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr);
        addr6->sin6_port = htons((uint16_t)task->Port());
    }
    else
    {
        auto* addr4 = (struct sockaddr_in*)&srcAddr;
        addr4->sin_family = AF_INET;
        evutil_inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr);
        addr4->sin_port = htons((uint16_t)task->Port());
    }

    if (!ZmTapContextEventHandler::OnPairAcceptBev("HubProxy", handle->bev1, (struct sockaddr*)&srcAddr, handle))
    {
        DEFAULT_LOG_ERROR("OnJsonRpcAsync: OnPairAcceptBev failed");
        handle->ReleasePair();
        replyCB(ZMJSON());
        return;
    }

    // 4. 注册 pair[0] 响应回调
    auto* rctx = new ResponseReadCtx{ std::move(replyCB), handle, 0, false, {} };
    bufferevent_setcb(handle->bev0, HttpJsonRpcManager::OnResponseRead, nullptr,
                       HttpJsonRpcManager::OnResponseEvent, rctx);
    bufferevent_setwatermark(handle->bev0, EV_READ, 4, 0);
    bufferevent_enable(handle->bev0, EV_READ | EV_WRITE);
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


        std::string jerrstr;
        ZMJSON repjson = zm_json_parse(rctx->buffer, jerrstr);
        if (!jerrstr.empty())
        {
            repjson.clear();
            repjson = { {"error", ZmJsonRpcServer::MakeError(ZM_JRPC_ERR_FORMAT,"Response format error")} };
        }

        rctx->callback(repjson);
        rctx->callback = nullptr;          // ★ 标记已消费，OnResponseEvent 看到后不再回调
        // ★ 不回收，不 delete rctx — 等待 OnResponseEvent 统揽收尾
    }
}

void HttpJsonRpcManager::OnResponseEvent(struct bufferevent* bev, short events, void* ctx)
{
    auto* rctx = static_cast<ResponseReadCtx*>(ctx);

    // ★ 若 callback 非空，说明 OnResponseRead 还没消费（纯 EOF 路径）
    if ((events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) && rctx->callback)
    {
        DEFAULT_LOG_WARN("JRPC response channel closed before full response, events={}", events);

        ZMJSON err = { {"error", ZmJsonRpcServer::MakeError(ZM_JRPC_ERR_DROPPED,"Response is dropped")} };
        rctx->callback(err);
        rctx->callback = nullptr;
    }

    if (rctx->pair_handle)
        rctx->pair_handle->ReleasePair();
    delete rctx;
}
