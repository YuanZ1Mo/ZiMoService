#include "modules/module_auth.h"

#include "modules/module_db.h"
#include "modules/module_user.h"
#include "modules/module_password.h"
#include "modules/module_session.h"
#include "modules/module_security.h"
#include "modules/module_audit.h"
#include "modules/module_permission.h"
#include "modules/module_gate.h"

#include "zm_net_http_server.h"
#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

using namespace drogon;

namespace
{
/// 时间均衡:账号不存在 / 密码错误耗时一致(防枚举)
/// 用固定 dummy 盐/哈希(格式合法即可,不关心校验结果,只保证 PBKDF2 计算耗时)
drogon::Task<void> TimingBalance(ZmPasswordModule* pwd)
{
    static const std::string kDummySalt(32, '0');
    static const std::string kDummyHash =
        "pbkdf2$sha256$600000$" + std::string(64, '0');
    co_await pwd->VerifyPassword("timing-equalizer-2026", kDummySalt, kDummyHash);
}

std::string GetUa(const HttpRequestPtr& req)
{
    return req->getHeader("User-Agent");
}
}  // namespace

// ============================================================================
// 构造 / 注册
// ============================================================================
ZmAuthModule::ZmAuthModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                           ZmPasswordModule* password, ZmSessionModule* session,
                           ZmSecurityModule* security, ZmAuditModule* audit,
                           ZmPermissionModule* permission, ZmDbModule* db,
                           ZmAuthGateModule* gate)
    : m_rest(rest), m_user(user), m_password(password), m_session(session),
      m_security(security), m_audit(audit), m_permission(permission), m_db(db),
      m_gate(gate)
{
}

ZmAuthModule::~ZmAuthModule() = default;

void ZmAuthModule::RegisterRoutes()
{
    if (!m_rest)
        return;
    // 注册/登录免鉴权(门禁放行;自身带限流 + 锁定)
    m_rest->RegisterCoro("/zimo/api/auth/register", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleRegister(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/auth/login", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleLogin(std::move(req));
                         });
    // 登出 / 强制改密 / 心跳:需鉴权(挂门禁 Filter)
    m_rest->RegisterCoro("/zimo/api/auth/logout", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleLogout(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/auth/force-reset", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleForceReset(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/auth/heartbeat", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleHeartbeat(std::move(req));
                         });
    DEFAULT_LOG_INFO("ZmAuthModule: /auth/* 接口已注册");
}

// ============================================================================
// 注册(自动登录)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmAuthModule::HandleRegister(HttpRequestPtr req)
{
    const std::string ip = ZmAuthGateModule::ClientIp(req);
    const std::string ua = GetUa(req);
    // 多维限流(register → ip / device)
    if (!(co_await m_security->CheckLimit("register", "ip", ip)))
    {
        co_return ZmAuthGateModule::ApiError(429, "RATE_LIMITED", "操作过于频繁,请稍后再试");
    }
    if (!(co_await m_security->CheckLimit("register", "device", ua)))
    {
        co_return ZmAuthGateModule::ApiError(429, "RATE_LIMITED", "操作过于频繁,请稍后再试");
    }
    co_await m_security->BumpLimit("register", "ip", ip);
    co_await m_security->BumpLimit("register", "device", ua);

    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
    {
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    }
    const std::string account = zm_json_get_str(body, "account");
    const std::string password = zm_json_get_str(body, "password");
    std::string nickname = zm_json_get_str(body, "nickname");
    if (account.empty() || password.empty())
    {
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "账号和密码不能为空");
    }
    std::string norm = ZmUserModule::NormalizeLoginKey(account);
    if (!m_user->ValidateAccountFormat(norm, err))
    {
        co_return ZmAuthGateModule::ApiError(400, "INVALID_ACCOUNT", err);
    }
    if (!(co_await m_user->IsAccountUnique(norm)))
    {
        co_return ZmAuthGateModule::ApiError(409, "USER_EXISTS", "账号已存在");
    }
    if (nickname.empty())
        nickname = norm;
    if (!m_user->ValidateNickname(nickname, err))
    {
        co_return ZmAuthGateModule::ApiError(400, "INVALID_NICKNAME", err);
    }
    if (!m_password->VerifyStrength(password, err))
    {
        co_return ZmAuthGateModule::ApiError(400, "PWD_WEAK", err);
    }
    // 哈希(工作池;600k 迭代)
    ZMJSON hashPair = co_await m_password->HashPassword(password);
    if (zm_json_get_str(hashPair, "hash").empty())
    {
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "密码哈希计算失败");
    }
    // 角色分配:首个注册用户 → developer,后续 → user(admin 由提权授予)
    int64_t active = co_await m_user->CountActiveUsers();
    std::string roleCode = (active == 0) ? "developer" : "user";
    // 建行(users + profile 同一事务)
    ZMJSON created = co_await m_user->CreateUser(norm, nickname,
                                                 zm_json_get_str(hashPair, "salt"),
                                                 zm_json_get_str(hashPair, "hash"),
                                                 ip, roleCode);
    int64_t uid = zm_json_get_int(created, "uid", 0);
    if (uid <= 0)
    {
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "用户创建失败");
    }
    // 签发会话(自动登录;cookie 写入响应)
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    bool isSecure = req->isOnSecureConnection();
    std::string token = co_await m_session->Issue(uid, ip, ua, isSecure, resp);
    if (token.empty())
    {
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "会话签发失败");
    }
    ZMJSON data = ZMJSON::object();
    data["uid"] = uid;
    data["account"] = norm;
    data["nickname"] = nickname;
    data["forceChange"] = false;
    resp->setBody(zm_json_dump(data));
    // 审计 + 事件
    co_await m_audit->RecordLogin(uid, norm, 1, "", ip, ua);
    co_await m_security->RecordEvent(uid, norm, "register", ip, ua, "");
    co_return resp;
}

// ============================================================================
// 登录(限流 + 阶梯锁定)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmAuthModule::HandleLogin(HttpRequestPtr req)
{
    const std::string ip = ZmAuthGateModule::ClientIp(req);
    const std::string ua = GetUa(req);
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
    {
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    }
    const std::string account = zm_json_get_str(body, "account");
    const std::string password = zm_json_get_str(body, "password");
    if (account.empty() || password.empty())
    {
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "账号和密码不能为空");
    }
    const std::string loginKey = ZmUserModule::NormalizeLoginKey(account);

    // 限流(login → login_key / ip)
    if (!(co_await m_security->CheckLimit("login", "login_key", loginKey)) ||
        !(co_await m_security->CheckLimit("login", "ip", ip)))
    {
        co_return ZmAuthGateModule::ApiError(429, "RATE_LIMITED", "尝试过于频繁,请稍后再试");
    }
    co_await m_security->BumpLimit("login", "login_key", loginKey);
    co_await m_security->BumpLimit("login", "ip", ip);

    // 查用户(定位锁定维度)
    ZMJSON user = co_await m_user->FindByLoginKey(loginKey);
    int64_t uid = zm_json_get_int(user, "uid", 0);
    // 阶梯锁定预检(先 uid 后 login_key)
    ZMJSON locked = co_await m_security->CheckLocked(loginKey, uid, ip);
    if (zm_json_get_bool(locked, "locked", false))
    {
        int64_t retryAfter = zm_json_get_int(locked, "retryAfter", 0);
        ZMJSON d = ZMJSON::object();
        d["code"] = "AUTH_LOCKED";
        d["message"] = "账号已锁定";
        d["retryAfter"] = retryAfter;
        co_return ZmHttpServer::JsonResponse(429, d);
    }

    // 账号不存在:时间均衡 + login_key 维度阶梯锁定(防枚举)
    if (uid <= 0)
    {
        co_await TimingBalance(m_password);
        co_await m_security->RecordLoginFail(loginKey, 0, ip);
        co_await m_audit->RecordLogin(0, loginKey, 2, "account_not_found", ip, ua);
        co_await m_security->RecordEvent(0, loginKey, "brute_force", ip, ua, "account_not_found");
        co_return ZmAuthGateModule::ApiError(401, "AUTH_FAILED", "账号或密码错误");
    }

    // 状态校验(停用/软删除)
    int status = zm_json_get_int(user, "status", 1);
    int deleted = zm_json_get_int(user, "deleted", 0);
    if (status == 2 || deleted == 1)
    {
        co_await m_audit->RecordLogin(uid, loginKey, 2, "disabled", ip, ua);
        co_await m_security->RecordEvent(uid, loginKey, "login_anomaly", ip, ua,
                                         "disabled_account_login");
        co_return ZmAuthGateModule::ApiError(401, "ACCOUNT_DISABLED", "账号不可登录");
    }

    // 密码校验(主密码;失败尝试临时密码)
    const std::string passSalt = zm_json_get_str(user, "pass_salt");
    const std::string passHash = zm_json_get_str(user, "pass_hash");
    bool pwdOk = co_await m_password->VerifyPassword(password, passSalt, passHash);
    int forceChange = zm_json_get_int(user, "force_change", 0);
    bool viaTemp = false;
    if (!pwdOk && forceChange == 1)
    {
        // 管理员重置后:临时密码可登录(命中 → 强制改密)
        std::string tempSalt = zm_json_get_str(user, "temp_pass_salt");
        std::string tempHash = zm_json_get_str(user, "temp_pass_hash");
        if (!tempSalt.empty() && !tempHash.empty())
        {
            viaTemp = co_await m_password->VerifyPassword(password, tempSalt, tempHash);
        }
    }
    if (!pwdOk && !viaTemp)
    {
        co_await TimingBalance(m_password);
        co_await m_security->RecordLoginFail(loginKey, uid, ip);
        co_await m_audit->RecordLogin(uid, loginKey, 2, "wrong_password", ip, ua);
        co_await m_security->RecordEvent(uid, loginKey, "brute_force", ip, ua, "wrong_password");
        co_return ZmAuthGateModule::ApiError(401, "AUTH_FAILED", "账号或密码错误");
    }

    // 成功:清零锁定 + 签发会话 + 更新 last_login + 审计
    co_await m_security->ClearLock(loginKey, uid, ip);
    co_await m_user->TouchLastLogin(uid, ip, ZmDbModule::Now());
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    bool isSecure = req->isOnSecureConnection();
    co_await m_session->Issue(uid, ip, ua, isSecure, resp);
    co_await m_audit->RecordLogin(uid, loginKey, 1, "", ip, ua);
    ZMJSON data = ZMJSON::object();
    data["forceChange"] = (forceChange == 1);
    resp->setBody(zm_json_dump(data));
    co_return resp;
}

// ============================================================================
// 登出
// ============================================================================
drogon::Task<HttpResponsePtr> ZmAuthModule::HandleLogout(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    co_await m_session->Revoke(cookie);
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    // 清 cookie
    drogon::Cookie c(ZmSessionModule::CookieName(), "");
    c.setMaxAge(0);
    c.setPath("/");
    resp->addCookie(c);
    resp->setBody("{}");
    co_return resp;
}

// ============================================================================
// 强制重置密码
// ============================================================================
drogon::Task<HttpResponsePtr> ZmAuthModule::HandleForceReset(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    if (ctx.forceChange != 1)
    {
        co_return ZmAuthGateModule::ApiError(403, "FORCE_CHANGE_NOT_REQUIRED",
                                             "当前会话无需强制重置密码");
    }
    std::string err;
    ZMJSON body = zm_json_parse(std::string(req->getBody()), err);
    if (!err.empty() || !body.is_object())
    {
        co_return ZmAuthGateModule::ApiError(400, "BAD_REQUEST", "请求体格式错误");
    }
    const std::string newPassword = zm_json_get_str(body, "newPassword");
    if (!m_password->VerifyStrength(newPassword, err))
    {
        co_return ZmAuthGateModule::ApiError(400, "PWD_WEAK", err);
    }
    ZMJSON hashPair = co_await m_password->HashPassword(newPassword);
    if (zm_json_get_str(hashPair, "hash").empty())
    {
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "密码哈希计算失败");
    }
    // 更新哈希 + 清临时密码 + 清 force_change
    bool ok = co_await m_password->ApplyPasswordChange(
        ctx.uid, zm_json_get_str(hashPair, "salt"),
        zm_json_get_str(hashPair, "hash"), true);
    if (!ok)
    {
        co_return ZmAuthGateModule::ApiError(500, "INTERNAL", "密码更新失败");
    }
    // 吊销旧会话并签发新会话(保持已登录状态,已确认决策)
    co_await m_session->RevokeAllByUid(ctx.uid);
    const std::string ip = ZmAuthGateModule::ClientIp(req);
    const std::string ua = GetUa(req);
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(k200OK);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    co_await m_session->Issue(ctx.uid, ip, ua, req->isOnSecureConnection(), resp);
    co_await m_security->RecordEvent(ctx.uid, ctx.account, "force_change", ip, ua, "");
    resp->setBody("{}");
    co_return resp;
}

// ============================================================================
// 心跳(10s;轮询应答:携带策略变更版本号)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmAuthModule::HandleHeartbeat(HttpRequestPtr req)
{
    // 实验:直接一层 Task(绕过 Authorize 双层链)
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    int64_t policyVersion = co_await m_db->GetPolicyVersion();
    ZMJSON data = ZMJSON::object();
    data["forceChange"] = (ctx.forceChange == 1);
    data["policyVersion"] = policyVersion;
    co_return ZmAuthGateModule::ApiOk(data);
}
