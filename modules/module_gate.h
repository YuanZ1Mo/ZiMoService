#ifndef ZM_MODULE_GATE_H
#define ZM_MODULE_GATE_H

// ============================================================================
// ZmAuthGateModule:鉴权门禁模块(设计文档 §3.11,全局横切)
//  说明(重要):本捆绑 drogon 的 advice/filter 异步回调(cb/cc/fcb/fccb)仅支持
//    "调用线程内同步回调",跨线程调用会卡死链;故会话校验(异步查库)不放在
//    advice/filter,而由业务 handler 经本模块的 Authorize() 协程显式调用
//    (handler 为 drogon 原生协程,跨线程 resume 安全,注册/登录已实证)。
//  RESTful 面:RegisterPreRouting 同步 advice 仅做"cookie 存在性粗判"
//    (无 cookie → 401,防无效请求进 handler);完整会话校验/force_change 边界/
//    接口级权限由各 handler 首行 Authorize() 完成。
//  前端面(80/443):同步 advice——白名单免会话;未带会话访问受保护页面 302
//    /login(带 redirect);已登录访问 /login 等 302 /portal;SPA 页面渲染。
//  会话上下文经 Authorize() 返回,业务 handler 取用,不重复查会话。
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
    bool ok = false;
    int status = 401;
    std::string code;
    std::string message;
    ZmSessionCtx ctx;
};

class ZmAuthGateModule
{
public:
    ZmAuthGateModule(ZmSessionModule* session, ZmPermissionModule* permission);
    ~ZmAuthGateModule();

    /// 前端面门禁(ServicePortal::Init 内调用;须在 Open 前)
    void SetupFrontendGate(ZmHttpFrontendServer* fe);
    /// RESTful 面门禁:同步 cookie 粗判 advice(无 cookie 立即 401)
    void SetupRestfulGate(ZmHttpRestfulServer* rest);

    /// 业务 handler 首行调用的完整鉴权(会话校验 + 状态 + force_change 边界 +
    /// 接口级权限);@param requiredPerm 非空时要求该权限点
    /// 注:本捆绑 drogon/MSVC 下"handler → Authorize → AuthAndTouch"嵌套协程
    /// Task 链的对称转移会卡死;故 handler 应直接 co_await AuthAndTouch,
    /// 再用 CheckCtxSync 做同步判定 + 权限检查(见各模块 handler)。
    drogon::Task<ZmGateResult> Authorize(const drogon::HttpRequestPtr& req,
                                         const std::string& requiredPerm = "");

    /// 同步会话上下文判定(不含权限查询;供 handler 直接 AuthAndTouch 后调用):
    /// 返回 ok=false 时附带可回响应的 status/code/message
    static ZmGateResult CheckCtxSync(const ZmSessionCtx& ctx, const std::string& path);

    // ── 请求辅助 ──
    /// 客户端 IP
    static std::string ClientIp(const drogon::HttpRequestPtr& req);
    /// 统一 API 错误响应 {code,message}
    static drogon::HttpResponsePtr ApiError(int status, const std::string& code,
                                            const std::string& message);
    /// 统一 API 成功响应
    static drogon::HttpResponsePtr ApiOk(const ZMJSON& data);

    // ── 路径判定 ──
    static bool IsStaticPath(const std::string& path);
    static bool IsWhitelistPagePath(const std::string& path);
    static bool IsSpaPagePath(const std::string& path);
    static bool IsForceChangeAllowedApi(const std::string& path);
    /// 权限点→API 路径前缀映射(新增管理模块时在此补充;鉴权同源于权限推导)
    static std::string ApiPermForPath(const std::string& path);

private:
    void AuthorizeFrontend(const drogon::HttpRequestPtr& req,
                           drogon::AdviceCallback cb, drogon::AdviceChainCallback cc,
                           ZmHttpFrontendServer* fe);
    static drogon::HttpResponsePtr ServeIndex(ZmHttpFrontendServer* fe);

    ZmSessionModule* m_session = nullptr;
    ZmPermissionModule* m_permission = nullptr;
};

#endif // ZM_MODULE_GATE_H
