#ifndef ZM_MODULE_USER_ADMIN_H
#define ZM_MODULE_USER_ADMIN_H

// ============================================================================
// ZmUserAdminModule:用户管理模块(设计文档 §3.9,系统管理→用户管理)
//  全部管理操作编排(列表/列元数据/属性修改/角色变更/模块授权/停用启用/
//  删除恢复/强制重置密码);API 授权功能权限点 userManage(门禁已拦截),
//  门户入口由 systemManager 控制(两者分层,子功能可独立授权)。
//  等级压制:操作者 level > 目标,不可操作自己;developer(level 3)天然自我保护。
//  每次操作联动操作审计 + 安全事件 + 权限缓存失效 + 策略版本递增。
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <string>

class ZmDbModule;
class ZmUserModule;
class ZmPasswordModule;
class ZmSessionModule;
class ZmPermissionModule;
class ZmSecurityModule;
class ZmAuditModule;
class ZmHttpRestfulServer;
class ZmAuthGateModule;
struct ZmSessionCtx;

class ZmUserAdminModule
{
public:
    ZmUserAdminModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                      ZmPasswordModule* password, ZmSessionModule* session,
                      ZmPermissionModule* permission, ZmSecurityModule* security,
                      ZmAuditModule* audit, ZmDbModule* db, ZmAuthGateModule* gate);
    ~ZmUserAdminModule();

    void RegisterRoutes();

private:
    /// 等级压制 + 不可操作自己;返回错误信息(空 = 允许)
    static std::string CheckOperable(const ZmSessionCtx& op, int64_t targetUid,
                                             int targetLevel, bool allowSelf = false);
    /// 从请求提取 {1} 占位符 uid(先取参数,取不到则从路径解析)
    /// 路径参数 uid 文本 → int64(非法返回 0;调用方按 uid<=0 → 400)
    /// P3/v2.11:路径参数由路由形参直接传入,不再从 req 手工解析路径
    static int64_t ParseUid(const std::string& s);

    drogon::Task<drogon::HttpResponsePtr> HandleList(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleColumns(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandlePermCodes(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleGet(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandlePatch(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleRole(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandlePermissions(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleDisable(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleEnable(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleDelete(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleRestore(drogon::HttpRequestPtr req,
                                                   std::string uidStr);
    drogon::Task<drogon::HttpResponsePtr> HandleResetPassword(drogon::HttpRequestPtr req,
                                                   std::string uidStr);

    ZmHttpRestfulServer* m_rest = nullptr;
    ZmUserModule* m_user = nullptr;
    ZmPasswordModule* m_password = nullptr;
    ZmSessionModule* m_session = nullptr;
    ZmPermissionModule* m_permission = nullptr;
    ZmSecurityModule* m_security = nullptr;
    ZmAuditModule* m_audit = nullptr;
    ZmDbModule* m_db = nullptr;
    ZmAuthGateModule* m_gate = nullptr;
};

#endif // ZM_MODULE_USER_ADMIN_H
