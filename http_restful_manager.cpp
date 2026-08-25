#include "http_restful_manager.h"

#include <zm_util_logger.h>

using std::string;

void HttpRestfulManager::Init(uint16_t port, const string& ip, bool useSSL,
                              const string& rootPath)
{
    m_server = std::make_unique<ZmHttpRestfulServer>();
    m_server->SetupListeners(port, ip, useSSL, rootPath);
}

void HttpRestfulManager::Setup()
{
    // Setup 内部幂等(m_setupDone 守卫)
    if (m_server)
        m_server->Setup();
}

std::vector<uint16_t> HttpRestfulManager::GetPorts() const
{
    return m_server ? m_server->GetPorts() : std::vector<uint16_t>{};
}
