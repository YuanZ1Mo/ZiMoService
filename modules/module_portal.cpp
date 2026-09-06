#include "modules/module_portal.h"

#include "modules/module_user.h"
#include "modules/module_session.h"
#include "modules/module_permission.h"
#include "modules/module_gate.h"

#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

using namespace drogon;

// ============================================================================
// 构造 / 注册
// ============================================================================
ZmPortalModule::ZmPortalModule(ZmHttpRestfulServer* rest, ZmUserModule* user,
                               ZmSessionModule* session, ZmPermissionModule* permission,
                               ZmAuthGateModule* gate)
    : m_rest(rest), m_user(user), m_session(session), m_permission(permission),
      m_gate(gate)
{
}

ZmPortalModule::~ZmPortalModule() = default;

void ZmPortalModule::RegisterRoutes()
{
    if (!m_rest)
        return;
    // 全部需会话(门禁 Filter)
    m_rest->RegisterCoro("/zimo/api/portal/modules", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleModules(std::move(req));
                         });
    m_rest->RegisterCoro("/zimo/api/portal/home", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr> {
                             return HandleHome(std::move(req));
                         });
    DEFAULT_LOG_INFO("ZmPortalModule: /portal/* 接口已注册");
}

drogon::Task<HttpResponsePtr> ZmPortalModule::HandleModules(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }auto modules = co_await m_permission->GetModuleList(ctx.uid);
    co_return ZmAuthGateModule::ApiOk(modules);
}

drogon::Task<HttpResponsePtr> ZmPortalModule::HandleHome(HttpRequestPtr req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto ctx = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto r = ZmAuthGateModule::CheckCtxSync(ctx, req->path());
    if (!r.ok)
    {
        co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
    }
    ZMJSON data = ZMJSON::object();
    data["uid"] = ctx.uid;
    data["account"] = ctx.account;
    data["nickname"] = ctx.nickname;
    // 角色定位(角色名/等级)
    ZMJSON role = co_await m_permission->GetRole(ctx.roleCode);
    data["role"] = ZMJSON::object();
    data["role"]["code"] = ctx.roleCode;
    data["role"]["name"] = zm_json_get_str(role, "name", ctx.roleCode);
    data["role"]["level"] = ctx.level;
    // 拥有的模块清单
    data["modules"] = co_await m_permission->GetModuleList(ctx.uid);
    co_return ZmAuthGateModule::ApiOk(data);
}
