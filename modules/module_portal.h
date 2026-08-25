#ifndef MODULE_PORTAL_H
#define MODULE_PORTAL_H

#include "zm_net_req_loop_protocol.h"   // ZmReqLoop / ZmReqLoopRest / ZMJSON
#include "zm_net_http.h"                // ZmHttpdTask / evhttp_cmd_type

#include <cstdint>
#include <string>

class HttpFrontendManager;
class UserModule;

/**
 * @brief 门户模块(业务层)
 *
 * 门户外壳的服务端支撑:
 * - SPA history 路由 fallback(/portal 与 /portal/* 回落 portal.html,
 *   静态分支优先于门户兜底 Any("*") 命中,service_portal.cpp 兜底路由保持干净)
 * - 门户初始化信息接口(GET /portal/info:用户信息 + 已授权模块列表)
 *
 * 依赖 UserModule 的鉴权(AuthAndTouch)与权限查询(GetUserRole/GetUserModules),
 * 由 ServicePortal 构造时注入。
 */
class PortalModule
{
public:
    explicit PortalModule(UserModule* userModule);
    ~PortalModule() = default;

    /** @brief 自注册 /portal 与 /portal/* 静态路由(service_portal.cpp 调用一行) */
    void RegisterHttpRoutes(HttpFrontendManager* httpMgr);

    /**
     * @brief REST 分发入口(service_portal.cpp 的 else 链调用)
     * @return true 本模块已处理(含错误响应);false 未命中,走 portal 原逻辑
     */
    bool DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                      ZmHttpdTask* task, const BYTE* body, size_t bodyLen);

private:
    void HandlePortalInfo(ZmReqLoop* loop, ZmHttpdTask* task);

    /** @brief 用户管理分发(列表/操作),公共前置:鉴权 + developer/admin */
    void HandleUserManage(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                          const ZMJSON& body, ZmHttpdTask* task);

    void HandleUserList(ZmReqLoop* loop, const std::string& keyword, const std::string& role,
                        int status, int page, int pageSize);

    void HandleUserAction(ZmReqLoop* loop, const std::string& action, uint64_t targetId,
                          const ZMJSON& body, uint64_t opId,
                          const std::string& opAccount, const std::string& opRole);

    /**
     * @brief 等级校验:操作者 > 目标当前等级;newRole 非空时校验其合法且 < 操作者等级
     *        (提升最高到下一级、降级任意下级由该规则统一覆盖)
     */
    static bool CanOperate(const std::string& opRole, const std::string& targetRole,
                           const std::string& newRole, std::string& errText);

private:
    UserModule* m_userModule = nullptr;   ///< 注入:鉴权/角色/模块权限查询
};

#endif // MODULE_PORTAL_H
