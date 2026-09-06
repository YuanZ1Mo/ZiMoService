#ifndef ZM_MODULE_PORTAL_H
#define ZM_MODULE_PORTAL_H

// ============================================================================
// ZmPortalModule:门户模块(设计文档 §3.10)
//  门户壳的服务端支撑与用户主页数据。
//  接口(全部需会话;根前缀 /zimo/api):
//    GET /portal/modules  模块清单(侧边栏/路由驱动,按 index 全序)
//    GET /portal/home     用户主页数据(基本信息 + 角色定位 + 模块清单)
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

class ZmUserModule;
class ZmSessionModule;
class ZmPermissionModule;
class ZmAuthGateModule;
class ZmHttpRestfulServer;

class ZmPortalModule
{
public:
    ZmPortalModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                   ZmSessionModule* session, ZmPermissionModule* permission,
                   ZmAuthGateModule* gate);
    ~ZmPortalModule();

    void RegisterRoutes();

private:
    drogon::Task<drogon::HttpResponsePtr> HandleModules(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleHome(drogon::HttpRequestPtr req);

    ZmHttpRestfulServer* m_rest = nullptr;
    ZmUserModule* m_user = nullptr;
    ZmSessionModule* m_session = nullptr;
    ZmPermissionModule* m_permission = nullptr;
    ZmAuthGateModule* m_gate = nullptr;
};

#endif // ZM_MODULE_PORTAL_H
