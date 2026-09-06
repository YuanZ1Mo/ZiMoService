#ifndef ZM_MODULE_AUTH_H
#define ZM_MODULE_AUTH_H

// ============================================================================
// ZmAuthModule:认证模块(设计文档 §3.8)
//  流程编排:注册(自动登录)、登录(限流+阶梯锁定)、登出、强制重置密码、
//  心跳(会话触达 + 策略变更版本号轮询应答)。
//  接口(根前缀 /zimo/api):
//    POST /auth/register {account,password,nickname?}   免鉴权(限流)
//    POST /auth/login     {account,password}            免鉴权(限流+锁定)
//    POST /auth/logout                                  需鉴权
//    POST /auth/force-reset {newPassword}               需鉴权 + force_change
//    POST /auth/heartbeat                                需鉴权(轮询应答)
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

class ZmDbModule;
class ZmUserModule;
class ZmPasswordModule;
class ZmSessionModule;
class ZmSecurityModule;
class ZmAuditModule;
class ZmPermissionModule;
class ZmAuthGateModule;
class ZmHttpRestfulServer;

class ZmAuthModule
{
public:
    ZmAuthModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                 ZmPasswordModule* password, ZmSessionModule* session,
                 ZmSecurityModule* security, ZmAuditModule* audit,
                 ZmPermissionModule* permission, ZmDbModule* db,
                 ZmAuthGateModule* gate);
    ~ZmAuthModule();

    /// 注册全部 /auth/* 接口(ServicePortal 装配时调用)
    void RegisterRoutes();

private:
    drogon::Task<drogon::HttpResponsePtr> HandleRegister(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleLogin(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleLogout(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleForceReset(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleHeartbeat(drogon::HttpRequestPtr req);

    ZmHttpRestfulServer* m_rest = nullptr;
    ZmUserModule* m_user = nullptr;
    ZmPasswordModule* m_password = nullptr;
    ZmSessionModule* m_session = nullptr;
    ZmSecurityModule* m_security = nullptr;
    ZmAuditModule* m_audit = nullptr;
    ZmPermissionModule* m_permission = nullptr;
    ZmDbModule* m_db = nullptr;
    ZmAuthGateModule* m_gate = nullptr;
};

#endif // ZM_MODULE_AUTH_H
