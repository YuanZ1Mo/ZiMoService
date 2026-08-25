#include "http_frontend_manager.h"

#include <zm_util_logger.h>

using std::string;

void HttpFrontendManager::Init(const string& ip, bool useSSL,
                               const string& documentRoot)
{
    m_server = std::make_unique<ZmHttpFrontendServer>();

    // 文档根 + 404 页(绝对路径);documentRoot 为空则不配置静态服务
    if (!documentRoot.empty())
    {
        m_server->SetDocumentRoot(documentRoot);
        string notFound = documentRoot;
        if (!notFound.empty() && notFound.back() != '\\' && notFound.back() != '/')
            notFound += "\\";
        notFound += "html\\404.html";
        m_server->SetNotFoundPage(notFound);
    }

    // useSSL 由 NetDock 判定(hasCert)传入;证书全局经 ZmHttpServer::Init 的 Options 注入
    m_server->SetupListeners(ip, useSSL, "");
}

void HttpFrontendManager::Setup()
{
    // Setup 内部幂等(m_setupDone 守卫)
    if (m_server)
        m_server->Setup();
}

bool HttpFrontendManager::IsHttps() const
{
    return m_server && m_server->IsHttps();
}

std::vector<uint16_t> HttpFrontendManager::GetPorts() const
{
    return m_server ? m_server->GetPorts() : std::vector<uint16_t>{};
}
