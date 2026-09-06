#include "modules/module_user_admin.h"

#include "modules/module_db.h"
#include "modules/module_user.h"
#include "modules/module_password.h"
#include "modules/module_session.h"
#include "modules/module_permission.h"
#include "modules/module_security.h"
#include "modules/module_audit.h"
#include "modules/module_gate.h"

#include "zm_net_http_server.h"
#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

using namespace drogon;

// ============================================================================
// 构造 / 注册
// ============================================================================
ZmUserAdminModule::ZmUserAdminModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                                     ZmPasswordModule* password, ZmSessionModule* session,
                                     ZmPermissionModule* permission,
                                     ZmSecurityModule* security, ZmAuditModule* audit,
                                     ZmDbModule* db, ZmAuthGateModule* gate)
    : m_rest(rest), m_user(user), m_password(password), m_session(session),
      m_permission(permission), m_security(security), m_audit(audit), m_db(db),
      m_gate(gate)
{
}

ZmUserAdminModule::~ZmUserAdminModule() = default;

std::string ZmUserAdminModule::CheckOperable(const ZmSessionCtx& op, int64_t targetUid,
                                             int targetLevel)
{
    if (op.uid == targetUid)
        return "不可操作自己";
    if (op.level <= targetLevel)
        return "等级压制:仅可操作等级低于自己的用户";
    return "";
}

int64_t ZmUserAdminModule::UidOf(const HttpRequestPtr& req)
{
    std::string s = req->getParameter("1");
    if (s.empty())
    {
        // 兜底:从路径解析(/zimo/api/admin/users/{uid}[/suffix])
        const std::string p(req->path());
        const std::string marker = "/admin/users/";
        size_t pos = p.find(marker);
        if (pos != std::string::npos)
        {
            pos += marker.size();
            size_t end = p.find('/', pos);
            s = p.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }
    }
    try
    {
        return std::stoll(s);
    }
    catch (...)
    {
        return 0;
    }
}

void ZmUserAdminModule::RegisterRoutes()
{
    if (!m_rest)
        return;
    // 全部接口需 userManager 权限(门禁 Filter 按路径前缀拦截)
    m_rest->RegisterCoro("/zimo/api/admin/users", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleList(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/columns", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleColumns(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/perm-codes", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandlePermCodes(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleGet(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}", HttpMethod::Patch,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandlePatch(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/role", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleRole(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/permissions", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandlePermissions(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/disable", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleDisable(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/enable", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleEnable(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}", HttpMethod::Delete,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleDelete(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/restore", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleRestore(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/admin/users/{1}/reset-password", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleResetPassword(std::move(req));
                         });
    DEFAULT_LOG_INFO("ZmUserAdminModule: /admin/users/* 接口已注册");
}

// ============================================================================
// 列表 / 列元数据 / 权限点
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleList(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    int page = 1, size = 20;
    std::string search, role;
    int status = 0;
    bool includeDeleted = false;
    std::string ps = req->getParameter("page");
    if (!ps.empty()) { try { page = std::max(1, std::stoi(ps)); } catch (...) {} }
    std::string ss = req->getParameter("size");
    if (!ss.empty()) { try { size = std::min(200, std::max(1, std::stoi(ss))); } catch (...) {} }
    search = req->getParameter("search");
    role = req->getParameter("role");
    std::string st = req->getParameter("status");
    if (!st.empty()) { try { status = std::stoi(st); } catch (...) {} }
    std::string inc = req->getParameter("includeDeleted");
    includeDeleted = (inc == "1" || inc == "true");
    auto data = co_await m_user->ListUsers(page, size, search, role, status, includeDeleted);
    co_return ZmAuthGateModule::ApiOk(data);
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleColumns(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    (void)req;
    ZMJSON d = ZMJSON::object();
    d["columns"] = m_user->GetColumnsMeta();
    co_return ZmAuthGateModule::ApiOk(d);
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandlePermCodes(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    (void)req;
    ZMJSON d = ZMJSON::object();
    d["permissions"] = co_await m_permission->ListPermCodes();
    co_return ZmAuthGateModule::ApiOk(d);
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleGet(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto user = co_await m_user->FindByUid(uid);
    if (!zm_json_has(user, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    // 过滤敏感列
    for (const char* k : {"pass_salt", "pass_hash", "temp_pass_salt", "temp_pass_hash"})
        zm_json_erase(user, k);
    co_return ZmAuthGateModule::ApiOk(user);
}

// ============================================================================
// 修改主表属性(白名单 + 等级压制 + 状态联动吊销)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandlePatch(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);

    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");

    // 白名单(动态列元数据中可编辑列;敏感列不在白名单由模块强制)
    const std::vector<std::string> whitelist = {
        "account", "nickname", "email", "phone", "status", "avatar", "signature", "preferences"};
    ZMJSON patch = body;
    bool ok = co_await m_user->UpdateUserRow(uid, whitelist, patch);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(400, "PATCH_REJECTED", "无可更新字段或校验失败");
    // 停用(status=2)联动吊销全部会话
    if (patch.contains("status") && zm_json_get_int(patch, "status", 0) == 2)
    {
        co_await m_session->RevokeAllByUid(uid);
        co_await m_permission->InvalidatePermCache(uid);
        co_await m_security->RecordEvent(uid, zm_json_get_str(target, "account", ""),
                                         "admin_disable", op.ip, "", "");
    }
    co_await m_audit->RecordOperation(op.uid, op.account, "update_profile", "user", uid,
                                      zm_json_dump(patch), op.ip);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 提权 / 降权
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleRole(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    std::string roleCode = zm_json_get_str(body, "roleCode");
    if (roleCode.empty())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "roleCode 不能为空");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    bool ok = co_await m_permission->ChangeRole(op.level, uid, roleCode, err);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(400, "ROLE_CHANGE_FAILED", err);
    co_await m_audit->RecordOperation(op.uid, op.account, "change_role", "user", uid,
                                      R"({"role":")" + roleCode + R"("})", op.ip);
    co_await m_security->RecordEvent(uid, zm_json_get_str(body, "account", ""),
                                     "change_role", op.ip, "", roleCode);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 模块授权 / 解除
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandlePermissions(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    std::string permCode = zm_json_get_str(body, "permCode");
    int grantType = zm_json_get_int(body, "grantType", 0);
    if (permCode.empty() || (grantType != 1 && grantType != 2))
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "permCode/grantType 非法");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    bool ok = co_await m_permission->SetUserPermission(uid, permCode, grantType, op.uid);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(400, "GRANT_FAILED", "授权失败(权限点不存在?)");
    ZMJSON detail = ZMJSON::object();
    detail["permCode"] = permCode;
    detail["grantType"] = grantType;
    co_await m_audit->RecordOperation(op.uid, op.account, "grant_permission", "user", uid,
                                      zm_json_dump(detail), op.ip);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 停用 / 启用
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleDisable(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    if (zm_json_get_int(target, "status", 1) == 2)
        co_return ZmAuthGateModule::ApiError(409, "ALREADY_DISABLED", "用户已停用");
    bool ok = co_await m_user->SetStatus(uid, 2);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "停用失败");
    co_await m_session->RevokeAllByUid(uid);
    co_await m_permission->InvalidatePermCache(uid);
    co_await m_audit->RecordOperation(op.uid, op.account, "disable", "user", uid, "{}", op.ip);
    co_await m_security->RecordEvent(uid, zm_json_get_str(target, "account", ""),
                                     "admin_disable", op.ip, "", "");
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleEnable(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    if (zm_json_get_int(target, "status", 1) == 1)
        co_return ZmAuthGateModule::ApiError(409, "ALREADY_ENABLED", "用户未停用");
    bool ok = co_await m_user->SetStatus(uid, 1);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "启用失败");
    co_await m_permission->InvalidatePermCache(uid);
    co_await m_audit->RecordOperation(op.uid, op.account, "enable", "user", uid, "{}", op.ip);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 删除 / 恢复(软删除)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleDelete(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    if (zm_json_get_int(target, "deleted", 0) == 1)
        co_return ZmAuthGateModule::ApiError(409, "ALREADY_DELETED", "用户已删除");
    bool ok = co_await m_user->SetDeleted(uid, 1);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "删除失败");
    co_await m_session->RevokeAllByUid(uid);
    co_await m_permission->InvalidatePermCache(uid);
    co_await m_audit->RecordOperation(op.uid, op.account, "delete", "user", uid, "{}", op.ip);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleRestore(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    if (zm_json_get_int(target, "deleted", 0) == 0)
        co_return ZmAuthGateModule::ApiError(409, "NOT_DELETED", "用户未删除");
    bool ok = co_await m_user->SetDeleted(uid, 0);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "恢复失败");
    co_await m_audit->RecordOperation(op.uid, op.account, "restore", "user", uid, "{}", op.ip);
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 强制重置密码(临时密码 + force_change + 吊销会话)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleResetPassword(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManager")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = UidOf(req);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    std::string deny = CheckOperable(op, uid, targetLevel);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    // 生成一次性临时密码(哈希落 temp_pass_*,置 force_change=1)
    std::string tempPassword = ZmPasswordModule::GenerateTempPassword();
    ZMJSON hashPair = co_await m_password->HashPassword(tempPassword);
    if (zm_json_get_str(hashPair, "hash").empty())
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "哈希计算失败");
    bool ok = co_await m_user->SetTempPassword(uid, zm_json_get_str(hashPair, "salt"),
                                               zm_json_get_str(hashPair, "hash"));
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "临时密码写入失败");
    ok = co_await m_user->SetForceChange(uid, 1);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "force_change 置位失败");
    co_await m_session->RevokeAllByUid(uid);
    co_await m_permission->InvalidatePermCache(uid);
    co_await m_audit->RecordOperation(op.uid, op.account, "reset_password", "user", uid,
                                      "{}", op.ip);
    co_await m_security->RecordEvent(uid, zm_json_get_str(target, "account", ""),
                                     "admin_reset", op.ip, "", "");
    ZMJSON data = ZMJSON::object();
    data["tempPassword"] = tempPassword;   // 仅本次返回,管理员转交用户
    co_return ZmAuthGateModule::ApiOk(data);
}
