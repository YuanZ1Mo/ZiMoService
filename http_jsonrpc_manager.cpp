#include "http_jsonrpc_manager.h"

#include "service_define.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_net_req_loop_protocol.h"   // ZmReqLoopJrpc(static_cast/SetLoopFactory 依赖)+ ZmReqLoopPool
#include "zm_util_sys.h"

#include <thread>
#include <windows.h>

// ============================================================================
// HttpJsonRpcManager 构造 / 析构
// ============================================================================

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evLoopHttpServerJRPC(nullptr)
    , m_httpServerJRPC(nullptr)
    , m_reqLoopPool(nullptr)
    , m_jrpcRequestReadCB({})
{
}

HttpJsonRpcManager::~HttpJsonRpcManager()
{
    Close();
}

bool HttpJsonRpcManager::Open()
{
    if (m_httpServerJRPC)
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
        DEFAULT_LOG_INFO("未发现 SSL 证书（{}），HttpJsonRpc服务器将使用 HTTP 模式", certDir);
    }
    const char* pCert = useHttps ? certFile.c_str() : nullptr;
    const char* pKey = useHttps ? keyFile.c_str() : nullptr;

    // 1. 创建私有 A 池(预创建/上限/业务预算;低并发可调小预创建数)
    if (!m_reqLoopPool)
    {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        m_reqLoopPool = new ZmReqLoopPool();
        // ★ JRPC 业务经 ZmReqLoopJrpc::Response 回复:池必须产出子类实例(基类无 m_reply 成员)
        //   (必须置于 Init 之前:Init 预创建时即用工厂,否则预创建出的仍是基类实例)
        m_reqLoopPool->SetLoopFactory([]() { return new ZmReqLoopJrpc(); });
        if (!m_reqLoopPool->Init((int)hw, (int)hw * 4, 5000))
        {
            DEFAULT_LOG_ERROR("[JRPC] A 池初始化失败");
            delete m_reqLoopPool;
            m_reqLoopPool = nullptr;
            return false;
        }
    }

    // 2. 创建自有事件循环线程（供 HTTP JRPC 服务器使用）
    if (m_evLoopHttpServerJRPC == nullptr)
    {
        m_evLoopHttpServerJRPC = new ZmEvBaseRunLoop("JrpcHttpServerLoop");
        if (!m_evLoopHttpServerJRPC->Loop())
        {
            DEFAULT_LOG_ERROR("[JRPC] HTTP 事件循环启动失败");
            delete m_evLoopHttpServerJRPC;
            m_evLoopHttpServerJRPC = nullptr;
            return false;
        }
    }

    // 3. 创建 HTTP JSON-RPC 服务器（绑定到自有事件循环），注册异步回调
    if (m_httpServerJRPC == nullptr)
    {
        m_httpServerJRPC = new ZmJsonRpcServer(m_evLoopHttpServerJRPC->GetEventBase(),
            ZM_HTTP_JRPC_SERVER_ROOT_URI, ZM_JSONRPC_SERVER_PORT, pCert, pKey,
            4096, "JRPC");
        if (!m_httpServerJRPC->Init())
        {
            DEFAULT_LOG_ERROR("[JRPC] HTTP 服务器初始化失败，端口: {}", ZM_JSONRPC_SERVER_PORT);
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

    // 4. 启动成功:输出启动日志(与 RESTful 版对称)
    if (m_httpServerJRPC)
    {
        DEFAULT_LOG_INFO("[JRPC] 服务器已启动，端口: {}，前缀: {}", ZM_JSONRPC_SERVER_PORT, ZM_HTTP_JRPC_SERVER_ROOT_URI);
        return true;
    }
    return false;
}

void HttpJsonRpcManager::SetTicketKeys(const unsigned char* keys, size_t len)
{
    if (m_httpServerJRPC)
        m_httpServerJRPC->SetTicketKeys(keys, len);
}

void HttpJsonRpcManager::PostSetTicketKeys(const unsigned char* keys, size_t len)
{
    if (m_httpServerJRPC)
        m_httpServerJRPC->PostSetTicketKeys(keys, len);
}

void HttpJsonRpcManager::Close()
{
    // ★ ① 武装 close 通知器门:此后 closecb/登记/摘除不再触碰 map 与 A 池
    //    (A 池即将销毁,防 closecb 投 CLOSE 到已删除的 loop)
    if (m_httpServerJRPC)
        m_httpServerJRPC->BeginClose();

    // ② 排空 HTTP worker:join 后不再有 doer 进入 Acquire/START 投递
    //    (池饱和时 Acquire 等待最长剩余预算(~5s),关闭路径可接受)
    if (m_httpServerJRPC)
        m_httpServerJRPC->DrainWorkers();

    // ③ 停 A 池:join 全部 A 线程(在飞业务完成,其回复仍可投到存活的循环/doer池/evhttp)
    if (m_reqLoopPool)
    {
        m_reqLoopPool->Shutdown();
        delete m_reqLoopPool;
        m_reqLoopPool = nullptr;
    }

    // ④ 停 HTTP 服务器(释放 evhttp;此时无在飞 A 业务)
    if (m_httpServerJRPC != nullptr)
        m_httpServerJRPC->Close();

    // ⑤ 停自有事件循环(join)
    if (m_evLoopHttpServerJRPC != nullptr)
    {
        m_evLoopHttpServerJRPC->Stop();
        delete m_evLoopHttpServerJRPC;
        m_evLoopHttpServerJRPC = nullptr;
    }

    // ⑥ 最后销毁服务器(loop 已死,不会再有任何回调访问它)
    if (m_httpServerJRPC != nullptr)
    {
        delete m_httpServerJRPC;
        m_httpServerJRPC = nullptr;
    }
}

bool HttpJsonRpcManager::ReloadCertificate(const char* certFile, const char* keyFile)
{
    if (!m_httpServerJRPC || !m_httpServerJRPC->IsHttps())
        return false;
    return m_httpServerJRPC->ReloadCertificate(certFile, keyFile);
}

// ============================================================================
// 异步 JRPC 请求处理(Worker 线程 → A 池 Acquire → START 投递)
// ============================================================================

void HttpJsonRpcManager::OnJsonRpcCBAsync(ZmHttpdTask* task, const ZMJSON& request,
    std::function<void(const ZMJSON& response)> replyCB)
{
    if (!m_jrpcRequestReadCB)
    {
        ZMJSON err = { {"error", ZmJsonRpcServer::MakeError(ZM_JRPC_ERR_PORTAL_NOJRPC, "No JRPC callback")} };
        replyCB(err);
        return;
    }

    // ① A 池获取(排队上限 = 剩余预算;客户端已断则提前放弃)
    int64_t remainMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs() - (int64_t)::GetTickCount64();
    if (remainMs <= 0) remainMs = 1;
    ZmReqLoop* loop = m_reqLoopPool->Acquire((int)remainMs, &task->ConnClosedFlag());
    if (!loop)
    {
        // ★ 无论是否断连都回复:replyCB 驱动 doer 回收路径
        // (断连时回复被 evhttp 丢弃,doer 仍正常回收,防泄漏)
        ZMJSON err = { {"error", ZmJsonRpcServer::MakeError(ZM_JRPC_ERR_DROPPED, "No worker available")} };
        replyCB(err);
        return;
    }

    // ② 请求 JSON 拷贝进闭包(request 是服务器栈上对象,必须拷贝)
    std::string reqJson = request.dump();
    std::function<void(const ZMJSON&)> reply = std::move(replyCB);

    // ③ 绑定 + 投递 START(doer 线程立即返回;START 处理在 A 线程完成 Bind + 业务)
    task->BindLoop(loop);
    auto* ctx = new ZmReqLoop::StartCtx();
    ctx->task = task;
    ctx->deadlineMs = task->ArriveMs() + (int64_t)m_reqLoopPool->BudgetMs();
    ctx->handlers.onStart = [this, reqJson = std::move(reqJson), reply = std::move(reply)](ZmReqLoop* l) mutable
    {
        static_cast<ZmReqLoopJrpc*>(l)->SetReply(std::move(reply));
        m_jrpcRequestReadCB(l, reqJson.c_str());
    };
    loop->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_START, ctx,
        [](void* p) { delete static_cast<ZmReqLoop::StartCtx*>(p); });
}
