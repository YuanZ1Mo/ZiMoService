#include "http_restful_manager.h"

#include "service_define.h"

#include "zm_net_runloop.h"
#include "zm_logger.h"
#include "zm_util_sys.h"

#include <windows.h>

// ============================================================================
// HttpRestfulManager 构造 / 析构
// ============================================================================

HttpRestfulManager::HttpRestfulManager()
    : m_evLoopHttpServer(nullptr)
    , m_httpServerRESTful(nullptr)
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
        DEFAULT_LOG_INFO("未发现 SSL 证书（{}），HttpRestful服务器将使用 HTTP 模式", certDir);
    }
    const char* pCert = useHttps ? certFile.c_str() : nullptr;
    const char* pKey = useHttps ? keyFile.c_str() : nullptr;

    // 1. 创建自有事件循环线程（供 RESTful 服务器使用）
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

    // 2. 创建 ZmRESTfulServer（绑定到自有事件循环，内部含自治 ZmReqLoopPool）
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

        // ★ 业务回调直接设置到服务器(内部 ZmReqLoopPool 分发到 ZmReqLoop 线程执行)
        m_httpServerRESTful->SetRequestReadCB(m_restfulRequestCB);
    }

    DEFAULT_LOG_INFO("[RESTful] 服务器已启动，端口: {}，前缀: {}", ZM_RESTFUL_SERVER_PORT, ZM_HTTP_RESTFUL_SERVER_ROOT_URI);
    return true;
}

// ============================================================================
// 关闭
// ============================================================================

void HttpRestfulManager::Close()
{
    // ★ 服务器内部按序排空:worker 池 → ZmReqLoopPool → doer 池 → evhttp_free
    //    (ZmReqLoopPool 在飞业务完成,其回复仍可投到存活的事件循环/evhttp)
    if (m_httpServerRESTful != nullptr)
        m_httpServerRESTful->Close();

    // 停自有事件循环(join)
    if (m_evLoopHttpServer != nullptr)
    {
        m_evLoopHttpServer->Stop();
        delete m_evLoopHttpServer;
        m_evLoopHttpServer = nullptr;
    }

    // 最后销毁服务器(loop 已死,不会再有任何回调访问它)
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
