#include "modules/module_dev_tools.h"

#include "modules/module_gate.h"
#include "modules/module_permission.h"
#include "modules/module_session.h"

#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

#include <utility>

// ============================================================================
// 内部常量
// ============================================================================
namespace
{
constexpr const char* kPermCode    = "devTools";
constexpr const char* kRoutePrefix = "/zimo/api/devTools";
} // namespace

// ============================================================================
// 构造 / 注册
// ============================================================================
ZmDevToolsModule::ZmDevToolsModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                                   ZmPermissionModule* permission, ZmAuthGateModule* gate)
    : m_rest(rest), m_session(session), m_permission(permission), m_gate(gate)
{
}

void ZmDevToolsModule::RegisterRoutes()
{
    if (!m_rest)
    {
        DEFAULT_LOG_ERROR("ZmDevToolsModule: RESTful 面为空,路由未注册");
        return;
    }
    m_rest->RegisterCoro(std::string(kRoutePrefix) + "/check", drogon::HttpMethod::Get,
                         [this](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr>
                         { return HandleCheck(std::move(req)); });
    DEFAULT_LOG_INFO("ZmDevToolsModule: 页面鉴权接口已注册({}/check)", kRoutePrefix);
}

void ZmDevToolsModule::RegisterPermissions()
{
    if (!m_permission)
        return;
    ZMJSON perm         = ZMJSON::object();
    perm["code"]        = "devTools";
    perm["name"]        = "小工具";
    perm["module"]      = "portal";   // 分组展示字段,与 home/filehub 一致(不参与侧边栏过滤与鉴权)
    perm["url"]         = "/portal/dev-tools";
    perm["type"]        = 0;          // 门户模块:进侧边栏
    perm["index"]       = 4;          // 正数区升序:home(1) → filehub(2) → serverAudioStream(3) → devTools(4)
    perm["sort"]        = 7;
    perm["enabled"]     = 1;
    perm["description"] = "门户小工具集(JSON 格式化 / Markdown 编辑器)";
    // 默认只授予开发者;其余角色由管理员在授权弹窗按需授予
    if (!m_permission->RegisterPermCodeSync(perm, {"developer"}))
        DEFAULT_LOG_ERROR("ZmDevToolsModule: 权限点登记失败({})", kPermCode);
    else
        DEFAULT_LOG_INFO("ZmDevToolsModule: 权限点已登记({})", kPermCode);
}

// ============================================================================
// 鉴权与接口
// ============================================================================
drogon::Task<ZmGateResult> ZmDevToolsModule::Authorize(const drogon::HttpRequestPtr& req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    ZmGateResult      r;
    r.ctx                = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    const ZmGateResult g = ZmAuthGateModule::CheckCtxSync(r.ctx, kRoutePrefix);
    if (!g.ok)
    {
        r.status  = g.status;
        r.code    = g.code;
        r.message = g.message;
        co_return r;
    }
    if (!(co_await m_permission->HasPermission(r.ctx.uid, std::string(kPermCode))))
    {
        r.status  = 403;
        r.code    = "PERM_DENIED";
        r.message = "无权限访问";
        co_return r;
    }
    r.ok = true;
    co_return r;
}

drogon::Task<drogon::HttpResponsePtr> ZmDevToolsModule::HandleCheck(drogon::HttpRequestPtr req)
{
    const ZmGateResult gate = co_await Authorize(std::move(req));
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    // 只回答"能不能进":不携带任何工具数据
    co_return ZmAuthGateModule::ApiOk(ZMJSON::object());
}
