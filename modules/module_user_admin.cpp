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

#include <algorithm>

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
                                             int targetLevel, bool allowSelf)
{
    if (op.uid == targetUid)
        return allowSelf ? "" : "不可操作自己";
    if (op.level <= targetLevel)
        return "等级压制:仅可操作等级低于自己的用户";
    return "";
}

int64_t ZmUserAdminModule::ParseUid(const std::string& s)
{
    // P3/v2.11:路径参数经路由形参传入({1} → uidStr),不再从路径手工解析;
    // 非法值返回 0(语义同旧版:调用方按 uid<=0 回 400 BAD_REQUEST)
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
    // 全部接口需 systemManager 权限(门禁 Filter 按路径前缀拦截)
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
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}", HttpMethod::Get,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleGet(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}", HttpMethod::Patch,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandlePatch(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/role", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleRole(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/permissions", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandlePermissions(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/disable", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleDisable(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/enable", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleEnable(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}", HttpMethod::Delete,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleDelete(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/restore", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleRestore(std::move(req), std::move(uidStr));
                         });
    m_rest->RegisterCoroWithPathParams("/zimo/api/admin/users/{1}/reset-password", HttpMethod::Post,
                         [this](HttpRequestPtr req, std::string uidStr) -> Task<HttpResponsePtr> {
                             return HandleResetPassword(std::move(req), std::move(uidStr));
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
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
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
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
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
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    (void)req;
    ZMJSON d = ZMJSON::object();
    d["permissions"] = co_await m_permission->ListPermCodes();
    co_return ZmAuthGateModule::ApiOk(d);
}

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleGet(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    int64_t uid = ParseUid(uidStr);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto user = co_await m_user->FindByUid(uid);
    if (!zm_json_has(user, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    // 过滤敏感列
    for (const char* k : {"pass_salt", "pass_hash", "temp_pass_salt", "temp_pass_hash"})
        zm_json_erase(user, k);
    // 附有效权限码全集(角色 ∪ 授予 − 拒绝),供编辑表单初始化模块勾选
    auto perms = co_await m_permission->GetEffectiveCodes(uid);
    ZMJSON arr = ZMJSON::array();
    for (auto& c : perms)
        arr.push_back(c);
    user["permissions"] = std::move(arr);
    co_return ZmAuthGateModule::ApiOk(user);
}

// ============================================================================
// 修改主表属性(白名单 + 等级压制 + 状态联动吊销)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandlePatch(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    // 允许编辑自己(基本信息);平级/上级仍拒绝
    std::string deny = CheckOperable(op, uid, targetLevel, true);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);

    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");

    // 白名单(动态列元数据中可编辑列;敏感列不在白名单由模块强制)
    std::vector<std::string> whitelist = {
        "account", "nickname", "email", "phone", "status", "avatar", "signature", "preferences"};
    if (uid == op.uid)
    {
        // 自己不可经 PATCH 停用自己(status 联动吊销会话会自锁),仅资料字段可改
        whitelist.erase(std::remove(whitelist.begin(), whitelist.end(), "status"), whitelist.end());
    }
    ZMJSON patch = body;
    // 空操作幂等成功:统一保存表单下"只改角色/模块"时 PATCH 无白名单字段属常态
    bool hasField = false;
    for (const auto& it : patch.items())
    {
        if (std::find(whitelist.begin(), whitelist.end(), it.key()) != whitelist.end())
        {
            hasField = true;
            break;
        }
    }
    if (!hasField)
        co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
    bool ok = co_await m_user->UpdateUserRow(uid, whitelist, patch);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(400, "PATCH_REJECTED", "字段校验失败(昵称/账号/状态非法或重复)");
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
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleRole(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    std::string roleCode = zm_json_get_str(body, "roleCode");
    if (roleCode.empty())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "roleCode 不能为空");
    // 存在性校验(DEF-5 2026-09-12):缺此校验时对不存在的 uid 会"谎报成功"
    // (GetLevel 返 0 → CheckOperable 通过 → ChangeRole 对 0 行更新仍返回 true)
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
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
// 模块授权(单人覆盖:按目标集合 diff,勾选=授予/取消=拒绝或清除)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandlePermissions(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
    if (uid <= 0)
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "uid 非法");
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    // 请求体:{ codes: [权限 code 全集] }(空数组合法 = 全部取消)
    if (!body.contains("codes") || !body["codes"].is_array())
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "codes 必须为数组");
    std::vector<std::string> codes;
    for (const auto& v : body["codes"])
    {
        if (!v.is_string())
            co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "codes 元素必须为字符串");
        codes.push_back(v.get<std::string>());
    }
    // 存在性校验(DEF-5 2026-09-12;同 HandleRole:否则会对不存在用户写授权)
    auto target = co_await m_user->FindByUid(uid);
    if (!zm_json_has(target, "uid"))
        co_return ZmAuthGateModule::ApiError(404, "USER_NOT_FOUND", "用户不存在");
    int targetLevel = co_await m_permission->GetLevel(uid);
    // 允许配置自己的模块(权限点均不高于操作者可见范围,无越权);平级/上级仍拒绝
    std::string deny = CheckOperable(op, uid, targetLevel, true);
    if (!deny.empty())
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", deny);
    ZMJSON diff;
    bool ok = co_await m_permission->SetUserPermissions(uid, codes, op.uid, diff);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(400, "GRANT_FAILED", "授权失败(权限点不存在?)");
    if (diff.is_object() && !diff.empty())
    {
        co_await m_audit->RecordOperation(op.uid, op.account, "grant_permission", "user", uid,
                                          zm_json_dump(diff), op.ip);
    }
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}

// ============================================================================
// 停用 / 启用
// ============================================================================
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleDisable(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
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

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleEnable(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
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
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleDelete(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
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

drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleRestore(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
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
drogon::Task<HttpResponsePtr> ZmUserAdminModule::HandleResetPassword(HttpRequestPtr req, std::string uidStr)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (!(co_await m_permission->HasPermission(ctx.uid, "userManage")))
    {
        co_return ZmAuthGateModule::ApiError(403, "PERM_DENIED", "无权限访问");
    }
    auto& op = ctx;
    int64_t uid = ParseUid(uidStr);
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
