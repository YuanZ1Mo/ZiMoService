#include "modules/module_gate.h"

#include "modules/module_session.h"
#include "modules/module_permission.h"

#include "zm_net_http_server.h"
#include "zm_net_http_frontend_server.h"
#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

#include <filesystem>

using namespace drogon;

// ============================================================================
// 构造
// ============================================================================
ZmAuthGateModule::ZmAuthGateModule(ZmSessionModule* session,
                                   ZmPermissionModule* permission)
    : m_session(session), m_permission(permission)
{
}

ZmAuthGateModule::~ZmAuthGateModule() = default;

// ============================================================================
// 统一响应 / 请求辅助
// ============================================================================
drogon::HttpResponsePtr ZmAuthGateModule::ApiError(int status, const std::string& code,
                                                   const std::string& message)
{
    ZMJSON d = ZMJSON::object();
    d["code"] = code;
    d["message"] = message;
    return ZmHttpServer::JsonResponse(status, d);
}

drogon::HttpResponsePtr ZmAuthGateModule::ApiOk(const ZMJSON& data)
{
    return ZmHttpServer::JsonResponse(200, data);
}

std::string ZmAuthGateModule::ClientIp(const drogon::HttpRequestPtr& req)
{
    return req->getPeerAddr().toIp();
}

// ============================================================================
// 路径判定
// ============================================================================
bool ZmAuthGateModule::IsStaticPath(const std::string& path)
{
    // 登录/注册页渲染必需的静态资源(含 vite 产物 assets/、public 静态目录 svg/html)
    if (path == "/favicon.ico")
        return true;
    static const char* prefixes[] = {"/css/", "/js/", "/resource/", "/assets/", "/svg/"};
    for (const char* p : prefixes)
    {
        if (path.rfind(p, 0) == 0)
            return true;
    }
    return false;
}

bool ZmAuthGateModule::IsWhitelistPagePath(const std::string& path)
{
    // 免会话白名单页面(需求 §3.1)
    return path == "/login" || path == "/register" || path == "/reset" ||
           path == "/404";
}

bool ZmAuthGateModule::IsSpaPagePath(const std::string& path)
{
    if (path == "/" || IsWhitelistPagePath(path) || path == "/force-reset" ||
        path == "/portal" || path.rfind("/portal/", 0) == 0)
        return true;
    return false;
}

bool ZmAuthGateModule::IsForceChangeAllowedApi(const std::string& path)
{
    // force_change=1 会话仅允许:强制改密、登出、心跳(设计 §7.6)
    return path.rfind("/zimo/api/auth/force-reset", 0) == 0 ||
           path.rfind("/zimo/api/auth/logout", 0) == 0 ||
           path.rfind("/zimo/api/auth/heartbeat", 0) == 0;
}

std::string ZmAuthGateModule::ApiPermForPath(const std::string& path)
{
    // 权限点→API 路径前缀映射(服务端静态配置;新增管理模块时在此补充,
    // 鉴权判定同源于权限推导 GetEffectiveCodes)
    if (path.rfind("/zimo/api/admin", 0) == 0)
        return "userManager";
    return "";
}

// ============================================================================
// 同步会话上下文判定(handler 直接 AuthAndTouch 后调用;不含权限查询)
// ============================================================================
ZmGateResult ZmAuthGateModule::CheckCtxSync(const ZmSessionCtx& ctx,
                                            const std::string& path)
{
    ZmGateResult r;
    if (!ctx.valid)
    {
        r.status = 401;
        r.code = "AUTH_INVALID";
        r.message = "会话无效,请重新登录";
        return r;
    }
    if (ctx.status == 2 || ctx.deleted == 1)
    {
        r.status = 401;
        r.code = "ACCOUNT_DISABLED";
        r.message = "账号不可登录";
        return r;
    }
    if (ctx.forceChange == 1 && !IsForceChangeAllowedApi(path))
    {
        r.status = 403;
        r.code = "FORCE_CHANGE_REQUIRED";
        r.message = "需要强制重置密码";
        return r;
    }
    r.ok = true;
    r.ctx = ctx;
    return r;
}

// ============================================================================
// 完整鉴权(业务 handler 首行调用;handler 为 drogon 原生协程,跨线程 resume 安全)
// ============================================================================
drogon::Task<ZmGateResult> ZmAuthGateModule::Authorize(const HttpRequestPtr& req,
                                                       const std::string& requiredPerm)
{
    ZmGateResult r;
    const std::string path(req->path());
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    if (cookie.empty())
    {
        r.status = 401;
        r.code = "AUTH_REQUIRED";
        r.message = "请先登录";
        co_return r;
    }
    auto ctx = co_await m_session->AuthAndTouch(cookie, ClientIp(req));
    if (!ctx.valid)
    {
        r.status = 401;
        r.code = "AUTH_INVALID";
        r.message = "会话无效,请重新登录";
        co_return r;
    }
    if (ctx.status == 2 || ctx.deleted == 1)
    {
        r.status = 401;
        r.code = "ACCOUNT_DISABLED";
        r.message = "账号不可登录";
        co_return r;
    }
    if (ctx.forceChange == 1 && !IsForceChangeAllowedApi(path))
    {
        r.status = 403;
        r.code = "FORCE_CHANGE_REQUIRED";
        r.message = "需要强制重置密码";
        co_return r;
    }
    if (!requiredPerm.empty())
    {
        bool has = co_await m_permission->HasPermission(ctx.uid, requiredPerm);
        if (!has)
        {
            r.status = 403;
            r.code = "PERM_DENIED";
            r.message = "无权限访问";
            co_return r;
        }
    }
    r.ok = true;
    r.ctx = std::move(ctx);
    co_return r;
}

// ============================================================================
// RESTful 面门禁(39441):同步 cookie 粗判(完整校验由 handler 内 Authorize 完成)
// ============================================================================
void ZmAuthGateModule::SetupRestfulGate(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;
    rest->RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                                    AdviceChainCallback&& cc) {
        (void)this;
        if (req->method() == drogon::Options)
        {
            cc();
            return;   // 预检由 CORS advice 处理
        }
        const std::string path(req->path());
        // 非本面业务路径(/ping、根路径)放行
        if (path == "/ping" || path == "/zimo/api" || path.rfind("/zimo/api/", 0) != 0)
        {
            cc();
            return;
        }
        // 免鉴权业务接口:注册/登录(自身带限流 + 阶梯锁定)
        if (path.rfind("/zimo/api/auth/register", 0) == 0 ||
            path.rfind("/zimo/api/auth/login", 0) == 0)
        {
            cc();
            return;
        }
        // 无 cookie 立即 401(防无效请求进 handler);有 cookie 由 handler 细判
        const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
        if (cookie.empty())
        {
            cb(ApiError(401, "AUTH_REQUIRED", "请先登录"));
            return;
        }
        cc();
    });
    DEFAULT_LOG_INFO("ZmAuthGateModule: RESTful 门禁 advice 已注册(同步粗判)");
}

// ============================================================================
// 前端面门禁(80/443):同步 advice(不做异步会话校验,保证链同步安全)
//   页面本身不含数据,数据一律经 API 面鉴权;会话有效性由前端
//   (API 401 统一跳登录)兜底。
// ============================================================================
void ZmAuthGateModule::SetupFrontendGate(ZmHttpFrontendServer* fe)
{
    if (!fe)
        return;
    fe->RegisterPreRouting([this, fe](const HttpRequestPtr& req, AdviceCallback&& cb,
                                      AdviceChainCallback&& cc) {
        AuthorizeFrontend(req, std::move(cb), std::move(cc), fe);
    });
    DEFAULT_LOG_INFO("ZmAuthGateModule: 前端门禁 advice 已注册(同步)");
}

drogon::HttpResponsePtr ZmAuthGateModule::ServeIndex(ZmHttpFrontendServer* fe)
{
    std::string page = fe->GetDocumentRoot();
    if (!page.empty() && page.back() != '\\' && page.back() != '/')
        page += "\\";
    page += "index.html";
    if (!std::filesystem::exists(page))
    {
        DEFAULT_LOG_WARN("ZmAuthGateModule: index.html 不存在: {}", page);
        return drogon::HttpResponse::newNotFoundResponse();
    }
    auto resp = drogon::HttpResponse::newFileResponse(page);
    resp->addHeader("Cache-Control", "no-cache");
    return resp;
}

void ZmAuthGateModule::AuthorizeFrontend(const HttpRequestPtr& req, AdviceCallback cb,
                                         AdviceChainCallback cc, ZmHttpFrontendServer* fe)
{
    const std::string path(req->path());
    // 静态资源 → drogon StaticFileRouter
    if (IsStaticPath(path))
    {
        cc();
        return;
    }
    // 非 SPA 页面路径:
    //  - /zimo/... 交由框架处理(其余根路径挂载)
    //  - 其余未知页面路径视为 SPA 业务页面(需求 §3.2/§3.3:无会话 302 /login;
    //    有会话回落 index.html,由前端路由渲染 404,见设计 §8.3)
    if (!IsSpaPagePath(path))
    {
        if (path.rfind("/zimo/", 0) == 0)
        {
            cc();
            return;
        }
    }
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    auto redir = [](const std::string& url) {
        return drogon::HttpResponse::newRedirectionResponse(url);
    };

    // 白名单页面(/login /register /reset /404)
    if (IsWhitelistPagePath(path))
    {
        // 已登录访问 /login /register /reset → 跳 /portal(需求 §3.3);/404 恒渲染
        if (!cookie.empty() && path != "/404")
        {
            cb(redir("/portal"));
            return;
        }
        cb(ServeIndex(fe));
        return;
    }

    // 服务器根 "/":鉴权成功跳 /portal,失败跳 /login(需求 §3.3)
    if (path == "/")
    {
        cb(cookie.empty() ? redir("/login") : redir("/portal"));
        return;
    }

    // /force-reset:需会话(强制改密状态由前端守卫 + API 403 兜底)
    if (path == "/force-reset")
    {
        if (cookie.empty())
        {
            cb(redir("/login?redirect=/force-reset"));
            return;
        }
        cb(ServeIndex(fe));
        return;
    }

    // /portal 及子路径(需会话;SPA 渲染)
    if (cookie.empty())
    {
        cb(redir("/login?redirect=" + path));
        return;
    }
    cb(ServeIndex(fe));
}
