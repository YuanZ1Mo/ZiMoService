#include "http_jsonrpc_manager.h"

#include <zm_util_logger.h>

using std::string;

/**
 * @brief 创建 JSON-RPC 面服务器并登记监听/根路径(Phase1,须先于 Open)
 *
 * @param port     监听端口
 * @param ip       绑定地址(0.0.0.0 = 通配)
 * @param useSSL   是否本面启用 TLS(证书为进程级全局,由 Init(opts) 注入)
 * @param rootPath 本面业务根路径(门禁与 handler 均以其为准)
 */
void HttpJsonRpcManager::Init(uint16_t port, const string& ip, bool useSSL,
                             const string& rootPath)
{
    // 只创建 + 登记监听;真正绑定发生在 ZmHttpServer::Open 跑 app().run() 时
    m_server = std::make_unique<ZmHttpJsonRpcServer>();
    m_server->SetupListeners(port, ip, useSSL, rootPath);
}

/// 注册本面结构路由(幂等:内部 m_setupDone 守卫,重复调用无副作用)
void HttpJsonRpcManager::Setup()
{
    if (m_server)
        m_server->Setup();
}

/// @return 本面监听端口;未创建服务器(未 Init)= 0
uint16_t HttpJsonRpcManager::GetPort() const
{
    return m_server ? m_server->GetPort() : 0;
}
