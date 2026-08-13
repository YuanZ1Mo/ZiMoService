#include "http_jsonrpc_manager.h"

#include "service_define.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_util_sys.h"

#include <windows.h>

// ============================================================================
// HttpJsonRpcManager 构造 / 析构
// ============================================================================

HttpJsonRpcManager::HttpJsonRpcManager()
    : m_evLoopHttpServerJRPC(nullptr)
    , m_httpServerJRPC(nullptr)
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

    // 从 exe 路径推导证书目录（exe 在 $(SolutionDir)$(Configuration)\ 下，certs/ 与 exe 同目录）
    char exePath[MAX_PATH];
    ZmSystem::GetModuleDir(exePath, MAX_PATH);
    std::string certDir = std::string(exePath) + "\\certs";

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

    // 1. 创建自有事件循环线程（供 HTTP JRPC 服务器使用）
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
        m_evLoopHttpServerJRPC->StartTimer();   // 定时器手动触发(默认 60s 心跳)
    }

    // 2. 创建 HTTP JSON-RPC 服务器（绑定到自有事件循环，内部含自治 ZmReqLoopPool）
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

        // ★ 业务回调直接设置到服务器(内部 ZmReqLoopPool 分发到 ZmReqLoop 线程执行)
        m_httpServerJRPC->SetRequestReadCB(m_jrpcRequestReadCB);
    }

    // 3. 启动成功:输出启动日志
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
    // ★ 服务器内部按序排空:worker 池 → 业务 ZmReqLoopPool → doer 池 → evhttp_free
    //    (ZmReqLoopPool 在飞业务完成,其回复仍可投到存活的事件循环/evhttp)
    if (m_httpServerJRPC != nullptr)
        m_httpServerJRPC->Close();

    // 停自有事件循环(join)
    if (m_evLoopHttpServerJRPC != nullptr)
    {
        m_evLoopHttpServerJRPC->Stop();
        delete m_evLoopHttpServerJRPC;
        m_evLoopHttpServerJRPC = nullptr;
    }

    // 最后销毁服务器(loop 已死,不会再有任何回调访问它)
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
