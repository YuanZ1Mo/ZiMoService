#include "http_frontend_manager.h"

#include <zm_util_logger.h>

using std::string;

void HttpFrontendManager::Init(const string& ip, bool useSSL,
                               const string& documentRoot)
{
    // ── primary:完整前端实例(443 HTTPS 或 80 HTTP) ──
    m_primary = std::make_unique<ZmHttpFrontendServer>(/*redirectOnly=*/false);

    // 文档根 + 404 页(绝对路径);documentRoot 为空则不配置静态服务
    if (!documentRoot.empty())
    {
        m_primary->SetDocumentRoot(documentRoot);
        string notFound = documentRoot;
        if (!notFound.empty() && notFound.back() != '\\' && notFound.back() != '/')
            notFound += "\\";
        notFound += "html\\404.html";
        m_primary->SetNotFoundPage(notFound);
    }
    m_primary->SetupListeners(ip, useSSL, "");

    // ── redirect:仅 HTTPS 模式的 80→443 重定向实例(FR-22;一对象一端口) ──
    if (useSSL)
    {
        m_redirect = std::make_unique<ZmHttpFrontendServer>(/*redirectOnly=*/true);
        m_redirect->SetupListeners(ip, /*useSSL=*/false, "");
    }
}

void HttpFrontendManager::Setup()
{
    // Setup 内部幂等(m_setupDone 守卫);重定向面先注册(属"外来请求先裁决")
    if (m_redirect)
        m_redirect->Setup();
    if (m_primary)
        m_primary->Setup();
}

bool HttpFrontendManager::IsHttps() const
{
    return m_primary && m_primary->IsHttps();
}

uint16_t HttpFrontendManager::GetPort() const
{
    return m_primary ? m_primary->GetPort() : 0;
}

uint16_t HttpFrontendManager::GetRedirectPort() const
{
    return m_redirect ? m_redirect->GetPort() : 0;
}
