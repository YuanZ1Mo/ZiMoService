#include "http_restful_manager.h"

#include "service_define.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_net_tap.h"

#include <event2/bufferevent.h>
#include <event2/buffer.h>

// ============================================================================
// HttpRestfulManager 构造 / 析构
// ============================================================================

HttpRestfulManager::HttpRestfulManager()
    : m_evLoopHttpServer(nullptr)
    , m_httpServerRESTful(nullptr)
    , m_evLoopPairPool(nullptr)
    , m_pairPool(nullptr)
{
}

HttpRestfulManager::~HttpRestfulManager()
{
    Close();
    ShutdownPairPool();
}

// ============================================================================
// 初始化
// ============================================================================

bool HttpRestfulManager::Open(const char* certFile,
                              const char* keyFile)
{
    // 1. 创建 pair 池的事件循环
    if (!m_evLoopPairPool)
    {
        m_evLoopPairPool = new ZmEvBaseRunLoop("RESTfulPairPoolLoop");
        if (!m_evLoopPairPool->Loop())
        {
            DEFAULT_LOG_ERROR("[RESTful] Pair 池事件循环启动失败");
            delete m_evLoopPairPool;
            m_evLoopPairPool = nullptr;
            return false;
        }
    }

    // 2. 创建 bufferevent_pair 对象池
    if (m_pairPool == nullptr)
    {
        m_pairPool = new ZmBuffereventPairPool();
        m_pairPool->Init(m_evLoopPairPool->GetEventBase(), 128);
    }

    // 3. 创建 HTTP 服务器事件循环
    if (m_evLoopHttpServer == nullptr)
    {
        m_evLoopHttpServer = new ZmEvBaseRunLoop("RESTfulHttpServerLoop");
        if (!m_evLoopHttpServer->Loop())
        {
            DEFAULT_LOG_ERROR("[RESTful] HTTP 事件循环启动失败");
            delete m_evLoopHttpServer;
            m_evLoopHttpServer = nullptr;
            return false;
        }
    }

    // 4. 创建 ZmRESTfulServer
    if (m_httpServerRESTful == nullptr)
    {
        m_httpServerRESTful = new ZmRESTfulServer(
            m_evLoopHttpServer->GetEventBase(), ZM_HTTP_RESTFUL_SERVER_ROOT_URI, ZM_RESTFUL_SERVER_PORT, certFile, keyFile);
        if (!m_httpServerRESTful->Init())
        {
            DEFAULT_LOG_ERROR("[RESTful] HTTP 服务器初始化失败，端口: {}", ZM_RESTFUL_SERVER_PORT);
            delete m_httpServerRESTful;
            m_httpServerRESTful = nullptr;
            m_evLoopHttpServer->Stop();
            delete m_evLoopHttpServer;
            m_evLoopHttpServer = nullptr;
            return false;
        }

        // 注册异步回调（将请求打包成帧 → pair → Hub）
        m_httpServerRESTful->SetRESTfulCBAsync(
            std::bind(&HttpRestfulManager::OnRESTfulCBAsync, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));
    }

    DEFAULT_LOG_INFO("[RESTful] 服务器已启动，端口: {}，前缀: {}", ZM_RESTFUL_SERVER_PORT, ZM_HTTP_RESTFUL_SERVER_ROOT_URI);
    return true;
}

// ============================================================================
// 关闭
// ============================================================================

void HttpRestfulManager::Close()
{
    if (m_httpServerRESTful != nullptr)
    {
        m_httpServerRESTful->Close();
        delete m_httpServerRESTful;
        m_httpServerRESTful = nullptr;
    }

    if (m_evLoopHttpServer != nullptr)
    {
        m_evLoopHttpServer->Stop();
        delete m_evLoopHttpServer;
        m_evLoopHttpServer = nullptr;
    }
}

void HttpRestfulManager::ShutdownPairPool()
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
// RESTful 异步回调：打包帧 → 写 pair → 注入 Hub
// ============================================================================

void HttpRestfulManager::OnRESTfulCBAsync(ZmHttpdTask* task,
    const BYTE* body, size_t body_len)
{
    // ① 从池获取 bufferevent_pair
    ZmBuffereventPairHandle* handle = m_pairPool ? m_pairPool->Acquire() : nullptr;
    if (handle == nullptr)
    {
        DEFAULT_LOG_ERROR("[RESTful] Acquire pair 失败");
        task->SetReply(ZM_HTTP_STATUS_CODE_SERVICE_UNAVAILABLE, "Service Unavailable");
        task->TriggerReply();
        return;
    }

    // ② 写请求帧到 pair[0]: "REST" + body_len + body
    struct evbuffer* output = bufferevent_get_output(handle->bev0);

    // 帧头 "REST"
    if (evbuffer_add(output, "REST", 4) < 0)
    {
        DEFAULT_LOG_ERROR("[RESTful] 写帧头失败");
        handle->ReleasePair();
        task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR);
        task->TriggerReply();
        return;
    }

    // body_len（大端）
    uint32_t bodyLen = htonl((uint32_t)body_len);
    if (evbuffer_add(output, &bodyLen, 4) < 0)
    {
        DEFAULT_LOG_ERROR("[RESTful] 写 body_len 失败");
        handle->ReleasePair();
        task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR);
        task->TriggerReply();
        return;
    }

    // raw_body（零拷贝引用）
    if (body && body_len > 0)
    {
        char* bodyCopy = new char[body_len];
        memcpy(bodyCopy, body, body_len);

        if (evbuffer_add_reference(output, bodyCopy, body_len,
            [](const void*, size_t, void* extra) {
                delete[] static_cast<char*>(extra);
            }, bodyCopy) < 0)
        {
            DEFAULT_LOG_ERROR("[RESTful] 写 body 失败");
            delete[] bodyCopy;
            handle->ReleasePair();
            task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR);
            task->TriggerReply();
            return;
        }
    }

    // ④ pair[1] 注入 Hub 代理链（直接把 task 传进去，Hub 创建 TAP 时自动设 tap->httpd_task）
    struct sockaddr_storage srcAddr = {};
    std::string ip = task->Ip();
    if (!ip.empty())
    {
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
    }

    if (!ZmTapContextEventHandler::OnPairAcceptBev("HubProxy", handle->bev1,
            (struct sockaddr*)&srcAddr, handle, task))
    {
        DEFAULT_LOG_ERROR("[RESTful] OnPairAcceptBev 失败");
        handle->ReleasePair();
        task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR);
        task->TriggerReply();
        return;
    }

    // ★ 注册 pair[0] 清理回调 — 参考 JRPC 的 OnResponseEvent
    // RESTful 响应不走 pair 回传，但 pair[0] EOF 时仍需回收 handle 归还池
    // 业务层调 tap->Drop() → FreeRequesterEnd → Pair0EOF → 触发本回调 → ReleasePair()
    auto* cleanupCtx = new PairCleanupCtx{ handle };
    bufferevent_setcb(handle->bev0, nullptr, nullptr,
        HttpRestfulManager::OnPair0Event, cleanupCtx);
    bufferevent_enable(handle->bev0, EV_READ | EV_WRITE);
}

// ============================================================================
// pair[0] 清理回调（仿 JRPC OnResponseEvent，但无需读数据只管回收）
// ============================================================================

void HttpRestfulManager::OnPair0Event(struct bufferevent* bev, short events, void* ctx)
{
    auto* c = static_cast<PairCleanupCtx*>(ctx);
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
        // 正常流程：pair[1] 端 Drop → Pair0EOF → 本回调
    }
    else
    {
        DEFAULT_LOG_WARN("[RESTful] pair[0] 收到意外事件: events=0x{:x}", (unsigned)events);
    }
    // ★ 无论什么事件，都回收 pair 和 ctx（与 JRPC OnResponseEvent 对齐）
    c->handle->ReleasePair();  // 同时标记 pair0/pair1 完成 → TryReturn → 归还池
    delete c;
}
