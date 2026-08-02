#include "http_restful_manager.h"

#include "service_define.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_net_req_loop.h"
#include "zm_util_sys.h"

#include <thread>
#include <windows.h>

// ============================================================================
// HttpRestfulManager 构造 / 析构
// ============================================================================

HttpRestfulManager::HttpRestfulManager()
    : m_evLoopHttpServer(nullptr)
    , m_httpServerRESTful(nullptr)
    , m_reqLoopPool(nullptr)
    , m_restfulRequestCB({})
{
}

HttpRestfulManager::~HttpRestfulManager()
{
    Close();
}

// ============================================================================
// 初始化
// ============================================================================

bool HttpRestfulManager::Open()
{
    if (m_httpServerRESTful)
        return true;

    /*
    * @param certFile  证书 PEM 文件路径，非空时启用 HTTPS；nullptr = HTTP
    * @param keyFile   私钥 PEM 文件路径，非空时启用 HTTPS；nullptr = HTTP
    */
    
    // 从 exe 路径推导项目根目录（exe 在 $(SolutionDir)$(Configuration)\ 下，需上翻一层）
    // 同时推导证书目录（certs/ 在项目根目录下）
    char exePath[MAX_PATH];
    ZmSystem::GetModuleDir(exePath, MAX_PATH);
    std::string projRoot = std::string(exePath) + "\\..";
    std::string certDir = projRoot + "\\certs";

    // 构建证书路径（启用 HTTPS），证书不存在时退化为 HTTP
    std::string certFile = certDir + "\\server.crt";
    std::string keyFile = certDir + "\\server.key";
    bool useHttps = (GetFileAttributesA(certFile.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(keyFile.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (!useHttps)
    {
        DEFAULT_LOG_INFO("未发现 SSL 证书（{}），HttpRestful服务器将使用 HTTP 模式", certDir);
    }
    const char* pCert = useHttps ? certFile.c_str() : nullptr;
    const char* pKey = useHttps ? keyFile.c_str() : nullptr;

    // 1. 创建私有 A 池(预创建/上限/业务预算;低并发可调小预创建数)
    if (!m_reqLoopPool)
    {
        m_reqLoopPool = new ZmReqLoopPool();
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        if (!m_reqLoopPool->Init((int)hw, (int)hw * 4, 5000))
        {
            DEFAULT_LOG_ERROR("[RESTful] A 池初始化失败");
            delete m_reqLoopPool;
            m_reqLoopPool = nullptr;
            return false;
        }
    }

    // 2. 创建 HTTP 服务器事件循环
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

    // 3. 创建 ZmRESTfulServer
    if (m_httpServerRESTful == nullptr)
    {
        m_httpServerRESTful = new ZmRESTfulServer(
            m_evLoopHttpServer->GetEventBase(), ZM_HTTP_RESTFUL_SERVER_ROOT_URI, ZM_RESTFUL_SERVER_PORT, pCert, pKey,
            4096, "REST");
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

        // 注册异步回调（请求投递到 A 池处理）
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
    // ★ ① 武装 close 通知器门:此后 closecb/登记/摘除不再触碰 map 与 A 池
    //    (A 池即将销毁,防 closecb 投 CLOSE 到已删除的 loop)
    if (m_httpServerRESTful)
        m_httpServerRESTful->BeginClose();

    // ② 排空 HTTP worker:join 后不再有 doer 进入 Acquire/START 投递
    //    (池饱和时 Acquire 等待最长剩余预算(~5s),关闭路径可接受)
    if (m_httpServerRESTful)
        m_httpServerRESTful->DrainWorkers();

    // ③ 停 A 池:join 全部 A 线程(在飞业务完成,其回复仍可投到存活的循环/doer池/evhttp)
    if (m_reqLoopPool)
    {
        m_reqLoopPool->Shutdown();
        delete m_reqLoopPool;
        m_reqLoopPool = nullptr;
    }

    // ④ 停 HTTP 服务器(释放 evhttp;此时无在飞 A 业务)
    if (m_httpServerRESTful != nullptr)
        m_httpServerRESTful->Close();

    // ⑤ 停自有事件循环(join)
    if (m_evLoopHttpServer != nullptr)
    {
        m_evLoopHttpServer->Stop();
        delete m_evLoopHttpServer;
        m_evLoopHttpServer = nullptr;
    }

    // ⑥ 最后销毁服务器(loop 已死,不会再有任何回调访问它)
    if (m_httpServerRESTful != nullptr)
    {
        delete m_httpServerRESTful;
        m_httpServerRESTful = nullptr;
    }
}

bool HttpRestfulManager::ReloadCertificate(const char* certFile, const char* keyFile)
{
    if (!m_httpServerRESTful || !m_httpServerRESTful->IsHttps())
        return false;
    return m_httpServerRESTful->ReloadCertificate(certFile, keyFile);
}

void HttpRestfulManager::SetTicketKeys(const unsigned char* keys, size_t len)
{
    if (m_httpServerRESTful)
        m_httpServerRESTful->SetTicketKeys(keys, len);
}

void HttpRestfulManager::PostSetTicketKeys(const unsigned char* keys, size_t len)
{
    if (m_httpServerRESTful)
        m_httpServerRESTful->PostSetTicketKeys(keys, len);
}

// ============================================================================
// RESTful 异步回调：Acquire A → 绑定 → 投递 START
// ============================================================================

void HttpRestfulManager::OnRESTfulCBAsync(ZmHttpdTask* task,
    const BYTE* body, size_t body_len)
{
    if (!m_restfulRequestCB)
    {
        task->SetReply(ZM_HTTP_STATUS_CODE_INTERNAL_ERROR);
        task->TriggerReply();
        return;
    }

    // ① A 池获取(排队上限 = 剩余预算;客户端已断则提前放弃)
    int64_t remainMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs() - (int64_t)::GetTickCount64();
    if (remainMs <= 0) remainMs = 1;
    ZmReqLoop* loop = m_reqLoopPool->Acquire((int)remainMs, &task->ConnClosedFlag());
    if (!loop)
    {
        // ★ 无论是否断连都回复:TriggerReply 驱动 doer 回收路径
        // (断连时回复被 evhttp 丢弃,doer 仍正常回收,防泄漏)
        task->SetReply(ZM_HTTP_STATUS_CODE_SERVICE_UNAVAILABLE, "Service Unavailable");
        task->TriggerReply();
        return;
    }

    // ② 绑定 + 投递 START(doer 线程立即返回;START 处理在 A 线程完成 Bind + 业务)
    task->BindLoop(loop);
    auto* ctx = new ZmReqLoop::StartCtx();
    ctx->task = task;
    ctx->deadlineMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs();
    ctx->handlers.onStart = [this, body, body_len](ZmReqLoop* l)
    {
        // body 指向请求 evbuffer,回复发送或请求释放前有效(零拷贝按设计接受)
        m_restfulRequestCB(l, body, body_len);
    };
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_START, ctx,
        [](void* p) { delete static_cast<ZmReqLoop::StartCtx*>(p); });
}
