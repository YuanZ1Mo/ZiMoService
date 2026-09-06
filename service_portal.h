#ifndef SERVICE_PORTAL_H
#define SERVICE_PORTAL_H

// ============================================================================
// ServicePortal:业务层门户(重建,Drogon 版)
//  负责路由注册与业务模块编排:前端页面路由 / JRPC / RESTful / CORS 在此登记,
//  各业务模块(modules 目录)经 ServicePortal 装配并注册自身接口
//  (模块划分见《2026-09-05-用户系统模块设计.md》)。
// ============================================================================

#include <atomic>
#include <memory>
#include <string>

class NetDock;
class ZmHttpFrontendServer;
class ZmHttpJsonRpcServer;
class ZmHttpRestfulServer;

class ZmDbModule;
class ZmUserModule;
class ZmPasswordModule;
class ZmSessionModule;
class ZmPermissionModule;
class ZmSecurityModule;
class ZmAuditModule;
class ZmAuthGateModule;
class ZmAuthModule;
class ZmUserAdminModule;
class ZmPortalModule;

class ServicePortal
{
public:
    /// @param netDock 网络层宿主(Phase1:NetDock::Init 后传入;本类存裸指针)
    explicit ServicePortal(NetDock* netDock);
    ~ServicePortal();

    /// Phase1:注册前端页面路由 / JRPC / RESTful 与 CORS,并装配各业务模块(modules 目录)
    /// 必须在 ZmHttpServer::Open() 前调用
    void Init();

    /// 业务收尾(Phase3 前调用;本期无业务线程,保留接口)
    void Shutdown();

    /// 广播消息(39640 自定义 TCP,本期不接入;保留签名,恒 false)
    bool BroadcastMessage(const std::string& topic, const std::string& content,
                          const std::string& tag);

private:
    void CreateModules();
    void RegisterFrontendRoutes(ZmHttpFrontendServer* fe);
    void RegisterJsonRpcRoutes(ZmHttpJsonRpcServer* jrpc);
    void RegisterRestfulRoutes(ZmHttpRestfulServer* rest);
    void RegisterRestfulCors(ZmHttpRestfulServer* rest);

    NetDock* m_netDock = nullptr;
    ZmHttpFrontendServer* m_frontend = nullptr;
    ZmHttpJsonRpcServer* m_jrpc = nullptr;
    ZmHttpRestfulServer* m_restful = nullptr;

    // 业务模块(生命周期随 ServicePortal;依赖方向:编排模块 → 数据服务模块 → DbModule)
    std::unique_ptr<ZmDbModule> m_db;
    std::unique_ptr<ZmUserModule> m_user;
    std::unique_ptr<ZmPasswordModule> m_password;
    std::unique_ptr<ZmSessionModule> m_session;
    std::unique_ptr<ZmPermissionModule> m_permission;
    std::unique_ptr<ZmSecurityModule> m_security;
    std::unique_ptr<ZmAuditModule> m_audit;
    std::unique_ptr<ZmAuthGateModule> m_gate;
    std::unique_ptr<ZmAuthModule> m_auth;
    std::unique_ptr<ZmUserAdminModule> m_admin;
    std::unique_ptr<ZmPortalModule> m_portal;
};

#endif // SERVICE_PORTAL_H
