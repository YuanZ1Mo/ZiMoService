#ifndef ZM_MODULE_GATE_H
#define ZM_MODULE_GATE_H

// ============================================================================
// ZmAuthGateModule:鉴权门禁模块(全局横切)
//  约束(重要):drogon 的 advice/filter 异步回调(cb/cc/fcb/fccb)只支持
//    "调用线程内同步回调",跨线程调用会卡死处理链;因此**会话校验(异步查库)不放在
//    advice/filter 内**,而由业务 handler 经本模块的 Authorize() 协程显式完成
//    (handler 是 drogon 原生协程,跨线程 resume 安全)。
//  RESTful 面:PreRouting 同步 advice 只做"cookie 存在性粗判"(无 cookie → 401,
//    防无效请求进 handler);完整会话校验 / force_change 边界 / 接口级权限由各
//    handler 首行 Authorize() 完成。
//  前端面(80/443):同步 advice —— 白名单页免会话;未带会话访问受保护页面 →
//    302 /login(带 redirect);已登录访问 /login 等 → 302 /portal;SPA 页渲染。
//  会话上下文经 Authorize() 返回给业务 handler 复用,避免重复查会话。
// ============================================================================

#include <drogon/drogon_callbacks.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include "modules/module_session.h"   // ZmSessionCtx 完整定义

#include <string>

class ZmSessionModule;
class ZmPermissionModule;
class ZmHttpFrontendServer;
class ZmHttpRestfulServer;
struct ZmSessionCtx;

/// 门禁鉴权结果(handler 首行经 Authorize 获得;ok=false 时可直接按字段回响应)
struct ZmGateResult
{
    bool ok = false;          ///< 是否通过鉴权(含会话有效、账号可用、权限足够)
    int status = 401;         ///< ok=false 时建议的 HTTP 状态码
    std::string code;         ///< ok=false 时的错误码(如 AUTH_REQUIRED / PERM_DENIED)
    std::string message;      ///< ok=false 时的错误消息(可直接回给客户端)
    ZmSessionCtx ctx;         ///< 会话上下文(ok=true 时有效;业务 handler 复用)
};

class ZmAuthGateModule
{
public:
    /**
     * @brief 构造门禁模块并注入依赖
     *
     * @param session    会话模块(会话校验与失效判定)
     * @param permission 权限模块(权限点查询与角色等级)
     */
    ZmAuthGateModule(ZmSessionModule* session, ZmPermissionModule* permission);
    ~ZmAuthGateModule();

    /**
     * @brief 注册前端面(80/443)门禁 advice
     *
     * 同步 advice:白名单页放行、未登录跳 /login、已登录访问登录页跳 /portal、
     * SPA 页壳响应。须在 ZmHttpServer::Open 之前调用。
     *
     * @param fe 前端面实例(primary)
     */
    void SetupFrontendGate(ZmHttpFrontendServer* fe);

    /**
     * @brief 注册 RESTful 面(39441)门禁 advice
     *
     * 同步 advice:非本面路径与免鉴权接口放行、无 cookie 直接 401(防无效请求进
     * handler);有 cookie 时交由 handler 细判。须在 Open 之前调用。
     *
     * @param rest RESTful 面实例
     */
    void SetupRestfulGate(ZmHttpRestfulServer* rest);

    /**
     * @brief 完整鉴权:会话校验 + 账号状态 + force_change 边界 + 可选权限点
     *
     * 供业务 handler 使用。**注意**:drogon/MSVC 下
     * "handler → Authorize → AuthAndTouch" 的嵌套协程 Task 链对称转移会卡死,
     * 因此各模块 handler 的惯用写法是**直接 co_await 会话模块的 AuthAndTouch,
     * 再用 CheckCtxSync 做同步判定 + 权限检查**;本函数适用于单层调用场景。
     *
     * @param req          请求(取 zm_session cookie)
     * @param requiredPerm 非空时要求当前用户具备该权限点
     * @return 鉴权结果;ok=false 时按 status/code/message 直接回响应
     *
     * @example
     *   auto r = co_await m_gate->Authorize(req, "systemManager");
     *   if (!r.ok) co_return ZmAuthGateModule::ApiError(r.status, r.code, r.message);
     */
    drogon::Task<ZmGateResult> Authorize(const drogon::HttpRequestPtr& req,
                                         const std::string& requiredPerm = "");

    /**
     * @brief 同步会话上下文判定(不含权限查询)
     *
     * 供 handler 自行 co_await AuthAndTouch 之后调用:校验会话有效性、账号状态,
     * 并对处于强制改密态(force_change)的会话限制可用接口。
     *
     * @param ctx  会话上下文(来自会话模块)
     * @param path 请求路径(判断是否在 force_change 允许的接口集合内)
     * @return 鉴权结果;ok=false 时附 status/code/message
     */
    static ZmGateResult CheckCtxSync(const ZmSessionCtx& ctx, const std::string& path);

    // ── 请求辅助 ──
    /// @return 客户端 IP(取自对端地址)
    static std::string ClientIp(const drogon::HttpRequestPtr& req);

    /**
     * @brief 构造统一 API 错误响应
     *
     * 响应体为 {"code":..., "message":...},HTTP 状态码取 status。
     *
     * @param status  HTTP 状态码(401/403/400/...)
     * @param code    业务错误码(如 AUTH_REQUIRED)
     * @param message 错误消息(展示给用户)
     * @return JSON 响应
     */
    static drogon::HttpResponsePtr ApiError(int status, const std::string& code,
                                            const std::string& message);

    /// @brief 构造统一 API 成功响应(HTTP 200 + 裸 JSON 数据)
    /// @param data 响应数据
    static drogon::HttpResponsePtr ApiOk(const ZMJSON& data);

    // ── 路径判定 ──
    /// @return 是否为静态资源路径(assets/svg/css/js 等,交 drogon 静态路由器)
    static bool IsStaticPath(const std::string& path);
    /// @return 是否为免会话白名单页面(/login /register /reset /404)
    static bool IsWhitelistPagePath(const std::string& path);
    /// @return 是否为 SPA 页面路径(/、/portal 及子路径、白名单页、/force-reset)
    static bool IsSpaPagePath(const std::string& path);
    /// @return 强制改密态会话是否允许访问该 API(仅改密/登出/心跳)
    static bool IsForceChangeAllowedApi(const std::string& path);
    /// @brief 权限点 → API 路径前缀映射(新增管理模块时在此补充)
    /// @return 需要的权限点;空 = 不额外要求权限
    static std::string ApiPermForPath(const std::string& path);

private:
    /// @brief RESTful 面门禁实现(OPTIONS/非本面路径/免鉴权接口放行,无 cookie → 401)
    void RestfulGateAdvice(const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
                           drogon::AdviceChainCallback&& cc);

    void AuthorizeFrontend(const drogon::HttpRequestPtr& req,
                           drogon::AdviceCallback cb, drogon::AdviceChainCallback cc,
                           ZmHttpFrontendServer* fe);
    /// SPA 页壳响应(req 用于条件请求:命中 → 304;文件不可用 → 404)
    static drogon::HttpResponsePtr ServeIndex(const drogon::HttpRequestPtr& req,
                                              ZmHttpFrontendServer* fe);

    ZmSessionModule* m_session = nullptr;
    ZmPermissionModule* m_permission = nullptr;
};

#endif // ZM_MODULE_GATE_H
