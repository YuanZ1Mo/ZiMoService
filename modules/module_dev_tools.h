#ifndef ZM_MODULE_DEV_TOOLS_H
#define ZM_MODULE_DEV_TOOLS_H

// ============================================================================
// ZmDevToolsModule:小工具(权限点 devTools)
//   唯一接口:GET /zimo/api/devTools/check —— 页面进入鉴权探测(会话 + 账号状态 + 权限点)
//   本模块无数据面:不建表、不写文件、不涉及 WS / 媒体面。工具(JSON 格式化 /
//   Markdown 编辑器)全部在浏览器本地处理,check 只回答"能不能进",不携带任何
//   工具数据;通过后页面不再向服务端发请求(切回复检除外)。
//   设计见《2026-09-22-小工具模块设计.md》§2。
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

#include <string>

class ZmHttpRestfulServer;
class ZmSessionModule;
class ZmPermissionModule;
class ZmAuthGateModule;
struct ZmGateResult;

class ZmDevToolsModule
{
  public:
    /**
     * @brief 构造并注入依赖
     *
     * @param rest       RESTful 面(注册 check 路由)
     * @param session    会话模块(会话校验与续期)
     * @param permission 权限模块(判定 devTools)
     * @param gate       门禁模块(与其余模块同姿态注入;判定与响应助手均为静态成员)
     */
    ZmDevToolsModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                     ZmPermissionModule* permission, ZmAuthGateModule* gate);

    ZmDevToolsModule(const ZmDevToolsModule&) = delete;
    ZmDevToolsModule& operator=(const ZmDevToolsModule&) = delete;

    /// @brief 注册 GET /zimo/api/devTools/check(须先于 ZmHttpServer::Open)
    void RegisterRoutes();

    /// @brief 登记权限点 devTools(幂等;默认挂 developer)
    void RegisterPermissions();

  private:
    /// @brief 标准门禁:会话 + 账号状态 + 权限点 devTools(与音频模块 Authorize 同构)
    drogon::Task<ZmGateResult> Authorize(const drogon::HttpRequestPtr& req);

    /// @brief GET /zimo/api/devTools/check
    drogon::Task<drogon::HttpResponsePtr> HandleCheck(drogon::HttpRequestPtr req);

    ZmHttpRestfulServer* m_rest       = nullptr;  ///< RESTful 面(装配注入)
    ZmSessionModule*     m_session    = nullptr;  ///< 会话模块(装配注入)
    ZmPermissionModule*  m_permission = nullptr;  ///< 权限模块(装配注入)
    ZmAuthGateModule*    m_gate       = nullptr;  ///< 门禁模块(装配注入)
};

#endif // ZM_MODULE_DEV_TOOLS_H
