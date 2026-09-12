#include "modules/module_gate.h"

#include "modules/module_session.h"
#include "modules/module_permission.h"

#include "zm_net_http_server.h"
#include "zm_net_http_frontend_server.h"
#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

using namespace drogon;

// ============================================================================
// 构造 / 析构
// ============================================================================
/// 构造:只保存依赖指针(不持有所有权);门禁 advice 在 SetupXxxGate 内注册
ZmAuthGateModule::ZmAuthGateModule(ZmSessionModule* session,
                                   ZmPermissionModule* permission)
    : m_session(session), m_permission(permission)
{
}

ZmAuthGateModule::~ZmAuthGateModule() = default;

// ============================================================================
// 统一响应 / 请求辅助
// ============================================================================
/**
 * @brief 构造统一 API 错误响应
 *
 * 响应体固定为 {"code":..., "message":...},HTTP 状态码取 status。
 *
 * @param status  HTTP 状态码(401/403/400/404/409/500 等)
 * @param code    业务错误码(前端据此分支,如 AUTH_REQUIRED)
 * @param message 错误消息(可直接展示给用户)
 * @return JSON 响应
 */
drogon::HttpResponsePtr ZmAuthGateModule::ApiError(int status, const std::string& code,
                                                   const std::string& message)
{
    ZMJSON d = ZMJSON::object();
    d["code"] = code;
    d["message"] = message;
    return ZmHttpServer::JsonResponse(status, d);
}

/**
 * @brief 构造统一 API 成功响应(HTTP 200 + 裸 JSON)
 *
 * @param data 业务数据(约定为 JSON 对象)
 * @return JSON 响应
 */
drogon::HttpResponsePtr ZmAuthGateModule::ApiOk(const ZMJSON& data)
{
    return ZmHttpServer::JsonResponse(200, data);
}

/// @return 客户端 IP(取 TCP 对端地址,不含端口)
std::string ZmAuthGateModule::ClientIp(const drogon::HttpRequestPtr& req)
{
    return req->getPeerAddr().toIp();
}

// ============================================================================
// 路径判定
// ============================================================================
/**
 * @brief 判断是否为静态资源路径
 *
 * 命中的路径放行给 drogon 静态文件路由器处理(登录/注册页所需的 JS/CSS/图片等,
 * 含 vite 构建产物 assets/ 与 public 目录 svg/)。
 *
 * @param path 请求路径
 * @return true 静态资源;false 需要按页面/接口规则处理
 */
bool ZmAuthGateModule::IsStaticPath(const std::string& path)
{
    if (path == "/favicon.ico")
        return true;

    // 按目录前缀匹配(vite 产物集中在 assets/,其余为约定目录)
    static const char* prefixes[] = {"/css/", "/js/", "/resource/", "/assets/", "/svg/"};
    for (const char* p : prefixes)
    {
        if (path.rfind(p, 0) == 0)
            return true;
    }
    return false;
}

/**
 * @brief 判断是否为免会话白名单页面
 *
 * @param path 请求路径
 * @return true 无需会话即可访问(/login /register /reset /404)
 */
bool ZmAuthGateModule::IsWhitelistPagePath(const std::string& path)
{
    // 免会话白名单页面
    return path == "/login" || path == "/register" || path == "/reset" ||
           path == "/404";
}

/**
 * @brief 判断是否为 SPA 页面路径
 *
 * SPA 页面统一返回 index.html 页壳,由前端路由渲染;非 SPA 页面路径走静态文件或
 * 框架路由(如 /zimo/* 交回框架)。
 *
 * @param path 请求路径
 * @return true 首页、白名单页、/force-reset 及 /portal 及其子路径
 */
bool ZmAuthGateModule::IsSpaPagePath(const std::string& path)
{
    if (path == "/" || IsWhitelistPagePath(path) || path == "/force-reset" ||
        path == "/portal" || path.rfind("/portal/", 0) == 0)
        return true;
    return false;
}

/**
 * @brief 判断接口是否允许"强制改密"态会话访问
 *
 * @param path 请求路径
 * @return true 仅改密、登出、心跳三类接口放行,其余一律 403
 */
bool ZmAuthGateModule::IsForceChangeAllowedApi(const std::string& path)
{
    // force_change=1 会话仅允许:强制改密、登出、心跳
    return path.rfind("/zimo/api/auth/force-reset", 0) == 0 ||
           path.rfind("/zimo/api/auth/logout", 0) == 0 ||
           path.rfind("/zimo/api/auth/heartbeat", 0) == 0;
}

/**
 * @brief 权限点 → API 路径前缀映射
 *
 * 服务端静态配置:新增管理模块时在此补充一行。判定与权限推导同源(GetEffectiveCodes)。
 *
 * @param path 请求路径
 * @return 该路径所需的权限点;空 = 不额外要求权限(仅需有效会话)
 *
 * @example
 *   // /zimo/api/admin/users/5 → "userManager"
 */
std::string ZmAuthGateModule::ApiPermForPath(const std::string& path)
{
    if (path.rfind("/zimo/api/admin", 0) == 0)
        return "userManager";
    return "";
}

// ============================================================================
// 同步会话上下文判定(供 handler 自行 AuthAndTouch 之后调用;不含权限查询)
// ============================================================================
/**
 * @brief 对已取得的会话上下文做同步判定
 *
 * 判定三类问题:会话无效、账号被停用/删除、强制改密态访问了非白名单接口。
 * 不做权限点查询(该查询是异步的,由调用方自行完成)。
 *
 * @param ctx  会话上下文(由会话模块 AuthAndTouch 返回)
 * @param path 请求路径(用于 force_change 白名单判断)
 * @return ok=true 表示通过;否则按 status/code/message 回响应
 */
ZmGateResult ZmAuthGateModule::CheckCtxSync(const ZmSessionCtx& ctx,
                                            const std::string& path)
{
    ZmGateResult r;

    // ① 会话无效(不存在 / 已过期 / 已注销)
    if (!ctx.valid)
    {
        r.status = 401;
        r.code = "AUTH_INVALID";
        r.message = "会话无效,请重新登录";
        return r;
    }

    // ② 账号被停用(status=2)或已删除
    if (ctx.status == 2 || ctx.deleted == 1)
    {
        r.status = 401;
        r.code = "ACCOUNT_DISABLED";
        r.message = "账号不可登录";
        return r;
    }

    // ③ 强制改密态:只放行改密/登出/心跳,其余接口 403 引导前端跳改密页
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
// 完整鉴权(会话校验 + 状态 + force_change + 可选权限点)
// ============================================================================
/**
 * @brief 完整鉴权:取 cookie → 查会话 → 状态/强制改密判定 →(可选)权限点
 *
 * handler 为 drogon 原生协程,跨线程 resume 安全,故异步查库放在这里。
 * 注意:多层协程嵌套(handler → Authorize → AuthAndTouch)在 drogon/MSVC 下会卡死,
 * 各业务模块的惯用写法是直接 co_await 会话模块的 AuthAndTouch 再调 CheckCtxSync。
 *
 * @param req          请求(从 cookie 读 zm_session)
 * @param requiredPerm 非空时要求当前用户具备该权限点
 * @return 鉴权结果;ok=false 时按 status/code/message 直接回响应
 */
drogon::Task<ZmGateResult> ZmAuthGateModule::Authorize(const HttpRequestPtr& req,
                                                       const std::string& requiredPerm)
{
    ZmGateResult r;
    const std::string path(req->path());
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());

    // ① 无会话 cookie:直接 401,不查库
    if (cookie.empty())
    {
        r.status = 401;
        r.code = "AUTH_REQUIRED";
        r.message = "请先登录";
        co_return r;
    }

    // ② 查库校验会话并续期(AuthAndTouch)
    auto ctx = co_await m_session->AuthAndTouch(cookie, ClientIp(req));
    if (!ctx.valid)
    {
        r.status = 401;
        r.code = "AUTH_INVALID";
        r.message = "会话无效,请重新登录";
        co_return r;
    }

    // ③ 账号状态
    if (ctx.status == 2 || ctx.deleted == 1)
    {
        r.status = 401;
        r.code = "ACCOUNT_DISABLED";
        r.message = "账号不可登录";
        co_return r;
    }

    // ④ 强制改密边界
    if (ctx.forceChange == 1 && !IsForceChangeAllowedApi(path))
    {
        r.status = 403;
        r.code = "FORCE_CHANGE_REQUIRED";
        r.message = "需要强制重置密码";
        co_return r;
    }

    // ⑤ 可选的接口级权限点
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
// RESTful 面门禁(39441):同步 cookie 粗判(完整校验在 handler 内)
// ============================================================================
/**
 * @brief 注册 RESTful 面(39441)门禁 advice
 *
 * 同步 advice,按序裁决:OPTIONS 预检放行(交 CORS advice)→ 非本面路径与共享路径
 * 放行 → 免鉴权接口(注册/登录)放行 → 无 cookie 直接 401。有 cookie 时放行,
 * 由 handler 做完整会话与权限校验。
 *
 * @param rest RESTful 面实例;为空则跳过
 */
void ZmAuthGateModule::SetupRestfulGate(ZmHttpRestfulServer* rest)
{
    if (!rest)
        return;

    // 同步 advice:只做"cookie 存在性粗判",完整校验留给 handler(见类头说明)
    rest->RegisterPreRouting([this](const HttpRequestPtr& req, AdviceCallback&& cb,
                                    AdviceChainCallback&& cc) {
        RestfulGateAdvice(req, std::move(cb), std::move(cc));
    });;
    DEFAULT_LOG_INFO("ZmAuthGateModule: RESTful 门禁 advice 已注册(同步粗判)");
}

// ============================================================================
// 前端面门禁(80/443):同步 advice(不做异步会话校验,保证处理链同步)
//   页面本身不含数据,数据一律经 API 面鉴权;会话有效性由前端在收到 API 401 时
//   统一跳登录兜底。
// ============================================================================
/**
 * @brief 注册前端面(80/443)门禁 advice
 *
 * 同步 advice,全部裁决逻辑在 AuthorizeFrontend 内。
 *
 * @param fe 前端面实例(primary);为空则跳过
 */
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

/**
 * @brief 生成 SPA 页壳响应(文档根下的 index.html)
 *
 * 经平台响应助手构造:自带条件请求(Last-Modified/ETag → 304)与"文件不可用 → 404"
 * 语义(业务不自行拼装 drogon 响应 边界)。页面入口保留 no-cache:
 * 不本地缓存但每次导航再校验,命中校验时由平台回 304 省掉正文传输。
 *
 * @param req 请求(条件请求判定用)
 * @param fe  前端面实例(取文档根)
 * @return 页壳响应(200/304)或 404
 */
drogon::HttpResponsePtr ZmAuthGateModule::ServeIndex(const HttpRequestPtr& req,
                                                     ZmHttpFrontendServer* fe)
{
    std::string page = fe->GetDocumentRoot();
    if (!page.empty() && page.back() != '\\' && page.back() != '/')
        page += "\\";
    page += "index.html";

    auto resp = ZmHttpServer::FileResponse(req, page);
    resp->addHeader("Cache-Control", "no-cache");
    return resp;
}

/**
 * @brief 前端面页面裁决(同步):放行判断 + 白名单/SPA 跳转规则
 *
 * 顺序:平台共享路径放行 → 静态资源放行 → 非 SPA 的 /zimo/* 交回框架 →
 * 白名单页(已登录跳 /portal)→ 根路径跳转 → /force-reset → SPA 页壳。
 *
 * 注意:本 advice 不做端口判定,对所有端口都会执行,因此必须显式放行共享路径
 * (/ping 等三面可达的路径),否则它们会被误当作页面路径跳转到 /login。
 *
 * @param req 请求
 * @param cb  短路回调(直接回响应)
 * @param cc  放行回调(交后续处理)
 * @param fe  前端面实例
 */
void ZmAuthGateModule::AuthorizeFrontend(const HttpRequestPtr& req, AdviceCallback cb,
                                         AdviceChainCallback cc, ZmHttpFrontendServer* fe)
{
    const std::string path(req->path());

    // ① 平台共享路径(/ping 等):三面可达,业务页面门禁不得接管
    if (ZmHttpServer::IsSharedPath(path))
    {
        cc();
        return;
    }

    // ② 静态资源:交 drogon 静态文件路由器
    if (IsStaticPath(path))
    {
        cc();
        return;
    }

    // ③ 非 SPA 页面路径:/zimo/... 交回框架处理(其余根路径挂载在此不接管);
    //    其余未知页面路径视为 SPA 业务页面(需求 /:无会话 302 /login,
    //    有会话回落 index.html 由前端路由渲染 404)
    if (!IsSpaPagePath(path))
    {
        if (path.rfind("/zimo/", 0) == 0)
        {
            cc();
            return;
        }
    }

    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());

    // 跳转统一走平台助手(默认 302;需要 301 语义时显式传状态码)
    auto redir = [](const std::string& url) {
        return ZmHttpServer::RedirectResponse(url);
    };

    // 白名单页面(/login /register /reset /404)
    if (IsWhitelistPagePath(path))
    {
        // 已登录访问 /login /register /reset → 跳 /portal;/404 恒渲染
        if (!cookie.empty() && path != "/404")
        {
            cb(redir("/portal"));
            return;
        }
        cb(ServeIndex(req, fe));
        return;
    }

    // 服务器根 "/":鉴权成功跳 /portal,失败跳 /login
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
        cb(ServeIndex(req, fe));
        return;
    }

    // /portal 及子路径(需会话;SPA 渲染)
    if (cookie.empty())
    {
        cb(redir("/login?redirect=" + path));
        return;
    }
    cb(ServeIndex(req, fe));
}
/**
 * @brief RESTful 面门禁实现:OPTIONS/非本面路径/免鉴权接口放行,无 cookie → 401
 *
 * 只做 cookie 存在性粗判(完整会话校验留给 handler);共享路径经归属表判定,
 * 不在各面硬编码。
 *
 * @param req 请求
 * @param cb  短路回调(无 cookie 回 401)
 * @param cc  放行回调
 */
void ZmAuthGateModule::RestfulGateAdvice(const HttpRequestPtr& req, AdviceCallback&& cb,
                                        AdviceChainCallback&& cc)
{
(void)this;
if (req->method() == drogon::Options)
{
    cc();
    return;   // 预检由 CORS advice 处理
}
const std::string path(req->path());
// 非本面业务路径(共享路径如 /ping、本面根路径)一律放行
// 共享路径经归属表判定,不在各面硬编码
if (ZmHttpServer::IsSharedPath(path) || path == "/zimo/api" ||
    path.rfind("/zimo/api/", 0) != 0)
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
}
