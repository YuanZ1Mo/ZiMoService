#pragma once
// ============================================================================
// HttpFrontendManager:前端服务器(80/443)管理器
//  持有 ZmHttpFrontendServer 并负责其配置(文档根/监听)。
//  生命周期为进程级静态(ZmHttpServer::Init/Open/Close),本类不再委托 Open/Close;
//  证书判定(hasCert)由 NetDock 完成,本类只透传 useSSL,与内部对象 SetupListeners 对齐;
//  路由注册(页面别名等)业务层经 GetServer() 或 NetDock 提供的引用完成。
//  分层:NetDock → HttpFrontendManager → ZmHttpFrontendServer(设计 §2.4)
// ============================================================================

#include <memory>
#include <string>
#include <vector>

#include "zm_net_http_frontend_server.h"

class HttpFrontendManager
{
public:
    HttpFrontendManager() = default;
    ~HttpFrontendManager() = default;

    /// 配置前端面(Phase1;先于 ZmHttpServer::Open)
    /// 参数序与内部派生类 SetupListeners(ip, useSSL, rootPath) 对齐;
    /// 前端端口固定由 useSSL 决定(HTTPS→443+80,否则仅 80),故不收 port。
    /// @param ip          监听地址(默认 0.0.0.0 通配)
    /// @param useSSL      HTTPS 模式(NetDock 判定的 hasCert)→ 前端 443+80;false → 仅 80
    /// @param documentRoot 文档根(绝对路径);空 = 不设置静态文档根
    void Init(const std::string& ip = "0.0.0.0", bool useSSL = false,
              const std::string& documentRoot = "");

    /// 注册结构路由(幂等;Phase1,Open 前)
    void Setup();

    bool IsHttps() const;
    std::vector<uint16_t> GetPorts() const;

    ZmHttpFrontendServer* GetServer() const { return m_server.get(); }

private:
    std::unique_ptr<ZmHttpFrontendServer> m_server;
};
