#include "module_portal.h"

#include "http_server_manager.h"
#include "module_user.h"

#include "zm_util_json.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <string>
#include <vector>

// ============================================================================
// 构造
// ============================================================================

PortalModule::PortalModule(UserModule* userModule)
    : m_userModule(userModule)
{
}

// ============================================================================
// 路由注册:SPA history fallback(静态分支优先于 Any("*") 兜底,与注册顺序无关)
// ============================================================================

void PortalModule::RegisterHttpRoutes(HttpServerManager* httpMgr)
{
    if (!httpMgr)
        return;

    auto& router = httpMgr->GetRouter();

    // 门户壳页:/portal 与 /portal/xxx 均回落 portal.html,由前端路由恢复视图
    router.Any("/portal", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        return httpMgr->ServeStaticFile(task, "/html/portal.html");
    });
    router.Any("/portal/*", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        std::string uri(task->Uri() ? task->Uri() : "/");
        // SPA 回落:先纯查询(无副作用),有同名静态文件则发文件,否则发 portal.html。
        // 不可用 ServeStaticFile(uri) 直接探测——其"文件不存在写 404 页"的行为
        // 会把 404.html 与 portal.html 拼接进同一响应体。
        std::string physical;
        if (httpMgr->ResolveStaticPath(uri, physical))
            return httpMgr->SendFile(task, physical);
        return httpMgr->ServeStaticFile(task, "/html/portal.html");
    });
}

// ============================================================================
// REST 分发
// ============================================================================

bool PortalModule::DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                                ZmHttpdTask* task, const BYTE* body, size_t bodyLen)
{
    // 仅处理 /portal/* 前缀,其余放行
    if (path.size() < 8 || path.compare(0, 8, "/portal/") != 0)
        return false;

    if (verb == EVHTTP_REQ_GET && path == "/portal/info")
    {
        HandlePortalInfo(loop, task);
        return true;
    }

    // 用户管理(/portal/userManager*):POST 需解析 body;段边界匹配,防 /portal/userManagerxyz 误配
    if (path.rfind("/portal/userManager", 0) == 0 &&
        (path.size() == 19 || path[19] == '/'))
    {
        ZMJSON req;
        if (verb == EVHTTP_REQ_POST)
        {
            std::string bodyStr(body ? reinterpret_cast<const char*>(body) : "", bodyLen);
            if (!bodyStr.empty())   // 无 body 操作(disable/enable 等)按空对象处理
            {
                std::string err;
                req = zm_json_parse(bodyStr, err);
                if (!err.empty())
                {
                    ZmReqLoopRest::ResponseError(loop, 400, "请求体不是合法 JSON");
                    return true;
                }
            }
        }
        HandleUserManage(loop, verb, path, req, task);
        return true;
    }

    ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
    return true;
}

// ============================================================================
// 用户管理(公共前置:鉴权 + developer/admin)
// ============================================================================

void PortalModule::HandleUserManage(ZmReqLoop* loop, evhttp_cmd_type verb,
                                    const std::string& path, const ZMJSON& body, ZmHttpdTask* task)
{
    if (!m_userModule)
    {
        ZmReqLoopRest::ResponseError(loop, 503, "门户服务未初始化");
        return;
    }

    // 鉴权
    UserModule::UserInfo ui;
    uint64_t opId = m_userModule->AuthAndTouch(task, &ui);
    if (!opId)
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }
    // 权限:该用户可见模块含「用户管理」(developer 全量天然可见;admin 提升时自动授权;普通用户被授权后也可见)
    std::string opRole;
    m_userModule->GetUserRole(opId, opRole);
    {
        std::vector<ZMJSON> mods;
        if (!m_userModule->GetUserModules(opId, opRole, mods))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        bool hasUsers = false;
        for (const auto& m : mods)
        {
            if (zm_json_get_str(m, "code") == "userManager")
            {
                hasUsers = true;
                break;
            }
        }
        if (!hasUsers)
        {
            ZmReqLoopRest::ResponseError(loop, 403, "权限不足");
            return;
        }
    }

    // 列表
    if (verb == EVHTTP_REQ_GET && path == "/portal/userManager")
    {
        const char* kw = task->GetQueryValue("keyword", "");
        const char* role = task->GetQueryValue("role", "");
        const char* status = task->GetQueryValue("status", "0");
        int page = atoi(task->GetQueryValue("page", "1"));
        int pageSize = atoi(task->GetQueryValue("pageSize", "20"));
        HandleUserList(loop, kw ? kw : "", role ? role : "",
                       atoi(status ? status : "0"), page, pageSize);
        return;
    }

    // 单用户详情(含当前授权模块,供授权弹窗回显):GET /portal/userManager/{id}
    if (verb == EVHTTP_REQ_GET && path.rfind("/portal/userManager/", 0) == 0)
    {
        const std::string rest = path.substr(20);
        char* end = nullptr;
        errno = 0;
        uint64_t userId = strtoull(rest.c_str(), &end, 10);
        if (end == rest.c_str() || (end && *end != '\0') || errno == ERANGE || userId == 0)
        {
            // 严格解析:多余路径段("1/foo")/非数字/溢出一律 404
            ZmReqLoopRest::ResponseError(loop, 404, "用户不存在");
            return;
        }
        UserModule::UserRow row;
        if (!m_userModule->GetUserRow(userId, row) || row.deleted)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "用户不存在");
            return;
        }
        std::vector<ZMJSON> mods;
        if (!m_userModule->GetUserModules(userId, row.role, mods))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        ZMJSON rsp;
        rsp["result"]["id"] = (int64_t)row.userId;   // 对外暴露 user_id(用户唯一身份)
        rsp["result"]["account"] = row.account;
        rsp["result"]["nickname"] = row.nickname;
        rsp["result"]["role"] = row.role;
        rsp["result"]["disabled"] = row.disabled;
        rsp["result"]["modules"] = mods;
        ZmReqLoopRest::ResponseJson(loop, 200, rsp);
        return;
    }

    // 操作:/portal/userManager/{id}/{action}
    if (verb == EVHTTP_REQ_POST && path.rfind("/portal/userManager/", 0) == 0)
    {
        const std::string rest = path.substr(20);   // "/portal/userManager/" 长度
        const size_t slash = rest.find('/');
        if (slash == std::string::npos)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
            return;
        }
        uint64_t targetId = strtoull(rest.substr(0, slash).c_str(), nullptr, 10);
        if (targetId == 0)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "用户不存在");
            return;
        }
        HandleUserAction(loop, rest.substr(slash + 1), targetId, body, opId, ui.account, opRole);
        return;
    }

    ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
}

void PortalModule::HandleUserList(ZmReqLoop* loop, const std::string& keyword,
                                  const std::string& role, int status, int page, int pageSize)
{
    int total = 0;
    std::vector<UserModule::UserRow> list;
    if (!m_userModule->ListUsers(keyword, role, status, page, pageSize, total, list))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    ZMJSON rsp;
    rsp["result"]["total"] = total;
    for (const auto& row : list)
    {
        ZMJSON u;
        u["id"] = (int64_t)row.userId;   // 对外暴露 user_id(用户唯一身份)
        u["account"] = row.account;
        u["nickname"] = row.nickname;
        u["role"] = row.role;
        u["disabled"] = row.disabled;
        u["registerTime"] = (int64_t)row.registerTime;
        u["lastLoginTime"] = (int64_t)row.lastLoginTime;
        rsp["result"]["list"].push_back(u);
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

bool PortalModule::CanOperate(const std::string& opRole, const std::string& targetRole,
                              const std::string& newRole, std::string& errText)
{
    const int opLv = UserModule::GetUserRoleLevel(opRole);
    const int tgtLv = UserModule::GetUserRoleLevel(targetRole);
    if (opLv <= tgtLv)
    {
        errText = "无权操作同级或更高级账号";
        return false;
    }
    if (!newRole.empty())
    {
        const int newLv = UserModule::GetUserRoleLevel(newRole);
        // 提升最高到下一级(≤ opLv-1)、降级任意下级(< opLv)由同一规则覆盖
        if (newLv <= 0 || newLv >= opLv)
        {
            errText = "目标角色无效或超出可授予范围";
            return false;
        }
    }
    return true;
}

void PortalModule::HandleUserAction(ZmReqLoop* loop, const std::string& action, uint64_t targetId,
                                    const ZMJSON& body, uint64_t opId,
                                    const std::string& opAccount, const std::string& opRole)
{
    // 目标存在性 + 当前信息
    UserModule::UserRow target;
    if (!m_userModule->GetUserRow(targetId, target))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "用户不存在");
        return;
    }

    auto audit = [&](const std::string& act, const std::string& detail) {
        if (!m_userModule->WriteAuditLog(opId, opAccount, act, targetId, target.account, detail))
            DEFAULT_LOG_ERROR("[Portal] 用户管理操作审计落库失败: act={} target={}",
                              act, target.account);
    };
    auto respondOk = [&]() {
        ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
    };

    // ---- 状态类:disable / enable / delete / restore ----
    if (action == "disable" || action == "enable" || action == "delete" || action == "restore")
    {
        std::string err;
        if (!CanOperate(opRole, target.role, "", err))
        {
            ZmReqLoopRest::ResponseError(loop, 403, err);
            return;
        }
        if (action == "disable")
        {
            if (!m_userModule->SetUserDisabled(targetId, true)) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            audit("disable", "{\"disabled\":true}");
        }
        else if (action == "enable")
        {
            if (!m_userModule->SetUserDisabled(targetId, false)) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            audit("enable", "{\"disabled\":false}");
        }
        else if (action == "delete")
        {
            if (!m_userModule->SetUserDeleted(targetId, true)) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            audit("delete", "{\"deleted\":true}");
        }
        else
        {
            if (!m_userModule->SetUserDeleted(targetId, false)) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            audit("restore", "{\"deleted\":false}");
        }
        respondOk();
        return;
    }

    // ---- 重置密码(方案 B:临时密码 + 强制改密) ----
    if (action == "reset-password")
    {
        std::string err;
        if (!CanOperate(opRole, target.role, "", err))
        {
            ZmReqLoopRest::ResponseError(loop, 403, err);
            return;
        }
        std::string tempPassword;
        if (!m_userModule->ResetUserPassword(targetId, tempPassword))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        audit("reset_password", "{\"tempPassword\":true}");
        ZmReqLoopRest::ResponseJson(loop, 200,
            {{"result", {{"tempPassword", tempPassword}}}});
        return;
    }

    // ---- 修改昵称 ----
    if (action == "nickname")
    {
        std::string err;
        if (!CanOperate(opRole, target.role, "", err))
        {
            ZmReqLoopRest::ResponseError(loop, 403, err);
            return;
        }
        std::string nickname = zm_json_get_str(body, "nickname");
        std::string errText;
        if (!m_userModule->SetUserNickname(targetId, nickname, errText))
        {
            ZmReqLoopRest::ResponseError(loop, 400, errText);
            return;
        }
        ZMJSON d;
        d["nickname"] = nickname;
        audit("nickname", zm_json_dump(d));
        respondOk();
        return;
    }

    // ---- 授权模块 ----
    if (action == "modules")
    {
        std::string err;
        if (!CanOperate(opRole, target.role, "", err))
        {
            ZmReqLoopRest::ResponseError(loop, 403, err);
            return;
        }
        std::vector<std::string> codes = zm_json_get_array<std::string>(body, "modules");
        // admin 强制保留「用户管理」授权(角色配套,提升时自动授予,不可取消)
        if (target.role == "admin")
        {
            bool hasUsers = std::find(codes.begin(), codes.end(), "userManager") != codes.end();
            if (!hasUsers)
                codes.push_back("userManager");
        }
        if (!m_userModule->SetUserModules(targetId, codes))
        {
            ZmReqLoopRest::ResponseError(loop, 400, "存在无效的模块编码");
            return;
        }
        ZMJSON d;
        d["modules"] = codes;
        audit("modules", zm_json_dump(d));
        respondOk();
        return;
    }

    // ---- 修改角色 ----
    if (action == "role")
    {
        std::string newRole = zm_json_get_str(body, "role");
        std::string err;
        if (!CanOperate(opRole, target.role, newRole, err))
        {
            ZmReqLoopRest::ResponseError(loop, 403, err);
            return;
        }
        if (!m_userModule->SetUserRole(targetId, newRole))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        ZMJSON d;
        d["role"] = newRole;
        audit("role", zm_json_dump(d));
        respondOk();
        return;
    }

    ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + action);
}

// ============================================================================
// GET /portal/info:用户信息(含角色)+ 已授权模块列表
// ============================================================================

void PortalModule::HandlePortalInfo(ZmReqLoop* loop, ZmHttpdTask* task)
{
    if (!m_userModule)
    {
        ZmReqLoopRest::ResponseError(loop, 503, "门户服务未初始化");
        return;
    }

    UserModule::UserInfo ui;
    uint64_t uid = m_userModule->AuthAndTouch(task, &ui);
    if (!uid)
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }

    std::string role;
    m_userModule->GetUserRole(uid, role);

    std::vector<ZMJSON> modules;
    if (!m_userModule->GetUserModules(uid, role, modules))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    ZMJSON rsp;
    rsp["result"]["user"]["account"] = ui.account;
    rsp["result"]["user"]["nickname"] = ui.nickname;
    rsp["result"]["user"]["role"] = role;
    rsp["result"]["modules"] = modules;
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}
