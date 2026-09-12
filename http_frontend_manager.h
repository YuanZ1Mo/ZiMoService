#pragma once
// ============================================================================
// HttpFrontendManager:前端服务器管理器
//  一对象一端口:HTTPS 模式由两个 ZmHttpFrontendServer 实例构成——
//    primary  : 443(HTTPS,完整前端:docroot 静态/自定义 404/路径封禁)
//    redirect : 80(HTTP,仅 80→443 重定向,无证书模式不创建)
//  生命周期(Init/Open/Close)归 ZmHttpServer 的静态方法,本类只做"持有 + 配置";
//  证书判定(hasCert)由 NetDock 完成,本类只透传 useSSL;页面/门禁等业务注册
//  由业务层经 GetServer()(primary 实例)完成。
//  分层:NetDock → HttpFrontendManager → ZmHttpFrontendServer
// ============================================================================

#include <memory>
#include <string>

#include "zm_net_http_frontend_server.h"

class HttpFrontendManager
{
public:
    HttpFrontendManager() = default;
    ~HttpFrontendManager() = default;

    /**
     * @brief 按协议模式创建前端实例并登记监听(Phase1,须先于 ZmHttpServer::Open)
     *
     * HTTPS 模式(useSSL=true)创建两个实例:443 完整面 + 80 仅重定向;
     * 非 HTTPS 模式只创建一个 80 完整面。重定向实例不配置文档根、不声明归属。
     * 同时为主实例设置文档根与自定义 404 页(404 页固定在 <documentRoot>\html\404.html)。
     *
     * @param ip           监听地址(默认 0.0.0.0 通配)
     * @param useSSL       HTTPS 模式(由 NetDock 依证书存在性判定)
     * @param documentRoot 文档根绝对路径;空 = 不配置静态服务(也不设 404 页)
     *
     * @example
     *   HttpFrontendManager mgr;
     *   mgr.Init("0.0.0.0", true, R"(D:\ZiMo\frontend)");
     */
    void Init(const std::string& ip = "0.0.0.0", bool useSSL = false,
              const std::string& documentRoot = "");

    /// 注册两个实例的结构路由与门禁 advice(幂等;Phase1,须先于 Open)
    void Setup();

    /// @return 是否为 HTTPS 模式(以 primary 实例的监听 useSSL 为准)
    bool IsHttps() const;

    /// @return 完整前端实例端口(未配置 = 0)
    uint16_t GetPort() const;

    /// @return 重定向实例端口(无证书模式 / 未创建 = 0)
    uint16_t GetRedirectPort() const;

    /// @return 完整前端实例(业务层注册页面路由与门禁 advice 的入口)
    ZmHttpFrontendServer* GetServer() const { return m_primary.get(); }

    /// @return 重定向专用实例(通常无业务注册,仅供诊断)
    ZmHttpFrontendServer* GetRedirectServer() const { return m_redirect.get(); }

private:
    std::unique_ptr<ZmHttpFrontendServer> m_primary;
    std::unique_ptr<ZmHttpFrontendServer> m_redirect;
};
