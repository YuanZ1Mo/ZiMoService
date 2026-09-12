#include "http_frontend_manager.h"

#include <zm_util_logger.h>

using std::string;

/**
 * @brief 按协议模式创建前端实例并登记监听(Phase1,须先于 ZmHttpServer::Open)
 *
 * HTTPS 模式创建 443 完整面 + 80 重定向面;非 HTTPS 模式只创建 80 完整面。
 * 主实例文档根为空时不配置静态服务与 404 页。
 *
 * @param ip           监听地址(0.0.0.0 = 通配)
 * @param useSSL       HTTPS 模式(由 NetDock 依证书存在性判定)
 * @param documentRoot 文档根绝对路径(空 = 不配置静态服务)
 */
void HttpFrontendManager::Init(const string& ip, bool useSSL,
                               const string& documentRoot)
{
    // ── primary:完整前端实例(HTTPS 模式 443,否则 80) ──
    m_primary = std::make_unique<ZmHttpFrontendServer>(/*redirectOnly=*/false);

    // 文档根 + 自定义 404 页;documentRoot 为空则跳过(不服务静态资源)
    if (!documentRoot.empty())
    {
        m_primary->SetDocumentRoot(documentRoot);

        // 404 页按约定固定在 <documentRoot>\html\404.html(先补齐目录分隔符再拼接)
        string notFound = documentRoot;
        if (!notFound.empty() && notFound.back() != '\\' && notFound.back() != '/')
            notFound += "\\";
        notFound += "html\\404.html";
        m_primary->SetNotFoundPage(notFound);
    }

    // 根路径传 "/" = 本面为**兜底归属面**:未被其他面认领的路径(页面/SPA/静态文件)
    // 都归前端;其他面 root 下的路径由本面门禁自动 404
    m_primary->SetupListeners(ip, useSSL, "/");

    // ── redirect:仅 HTTPS 模式的 80→443 重定向实例(一对象一端口) ──
    // 该实例只挂 301/302 advice,不声明归属(传空根路径)、不配置文档根
    if (useSSL)
    {
        m_redirect = std::make_unique<ZmHttpFrontendServer>(/*redirectOnly=*/true);
        m_redirect->SetupListeners(ip, /*useSSL=*/false, "");
    }
}

/// 注册结构路由(幂等);重定向实例先注册,使 80 端口的重定向先于其他 advice 裁决
void HttpFrontendManager::Setup()
{
    if (m_redirect)
        m_redirect->Setup();
    if (m_primary)
        m_primary->Setup();
}

/// @return 是否 HTTPS 模式(以 primary 实例为准;未创建 = false)
bool HttpFrontendManager::IsHttps() const
{
    return m_primary && m_primary->IsHttps();
}

/// @return 完整前端实例端口;未创建 = 0
uint16_t HttpFrontendManager::GetPort() const
{
    return m_primary ? m_primary->GetPort() : 0;
}

/// @return 重定向实例端口;无证书模式未创建 = 0
uint16_t HttpFrontendManager::GetRedirectPort() const
{
    return m_redirect ? m_redirect->GetPort() : 0;
}
