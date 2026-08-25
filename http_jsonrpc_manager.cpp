#include "http_jsonrpc_manager.h"

#include <zm_util_logger.h>

using std::string;

void HttpJsonRpcManager::Init(uint16_t port, const string& ip, bool useSSL,
                             const string& rootPath)
{
    m_server = std::make_unique<ZmHttpJsonRpcServer>();
    m_server->SetupListeners(port, ip, useSSL, rootPath);
}

void HttpJsonRpcManager::Setup()
{
    // Setup 内部幂等(m_setupDone 守卫)
    if (m_server)
        m_server->Setup();
}

std::vector<uint16_t> HttpJsonRpcManager::GetPorts() const
{
    return m_server ? m_server->GetPorts() : std::vector<uint16_t>{};
}
