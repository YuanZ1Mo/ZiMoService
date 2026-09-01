#pragma once
// ============================================================================
// HttpFrontendManager:前端服务器管理器
//  一对象一端口(v2.5):HTTPS 模式由两个 ZmHttpFrontendServer 实例构成——
//    primary  : 443(HTTPS,完整前端:静态/SPA/404/门禁)
//    redirect : 80(HTTP,仅 80→443 重定向,FR-22;无证书模式不创建)
//  生命周期为进程级静态(ZmHttpServer::Init/Open/Close),本类不再委托 Open/Close;
//  证书判定(hasCert)由 NetDock 完成,本类只透传 useSSL;路由注册(页面别名等)
//  业务层经 GetServer()(primary)或 NetDock 提供的引用完成。
//  分层:NetDock → HttpFrontendManager → ZmHttpFrontendServer(设计 §2.4)
// ============================================================================

#include <memory>
#include <string>

#include "zm_net_http_frontend_server.h"

class HttpFrontendManager
{
public:
    HttpFrontendManager() = default;
    ~HttpFrontendManager() = default;

    /// 配置前端面(Phase1;先于 ZmHttpServer::Open)
    /// @param ip           监听地址(默认 0.0.0.0 通配)
    /// @param useSSL       HTTPS 模式(NetDock 判定的 hasCert):true → 443 完整面 + 80 重定向面;
    ///                     false → 仅 80 完整面
    /// @param documentRoot 文档根(绝对路径);空 = 不设置静态文档根(重定向面不传)
    void Init(const std::string& ip = "0.0.0.0", bool useSSL = false,
              const std::string& documentRoot = "");

    /// 注册结构路由(幂等;Phase1,Open 前;primary 与 redirect 都注册)
    void Setup();

    /// HTTPS 模式(以 primary 实例为准)
    bool IsHttps() const;
    /// 完整前端实例端口(0 = 未配置)
    uint16_t GetPort() const;
    /// 重定向实例端口(无证书模式 = 0)
    uint16_t GetRedirectPort() const;

    /// 完整前端实例(业务层路由注册入口)
    ZmHttpFrontendServer* GetServer() const { return m_primary.get(); }
    /// 重定向实例(通常无业务注册,仅供诊断)
    ZmHttpFrontendServer* GetRedirectServer() const { return m_redirect.get(); }

private:
    std::unique_ptr<ZmHttpFrontendServer> m_primary;
    std::unique_ptr<ZmHttpFrontendServer> m_redirect;
};
