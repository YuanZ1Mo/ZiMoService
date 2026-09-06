#ifndef ZM_MODULE_PERMISSION_H
#define ZM_MODULE_PERMISSION_H

// ============================================================================
// ZmPermissionModule:权限模块(设计文档 §3.5)
//  数据归属:roles、permissions、user_permissions。
//  有效权限 = 角色默认 ∪ 授予(grant_type=1) − 拒绝(grant_type=2,优先级最高);
//  按会话缓存(Redis,键 zimo:perm:*),管理操作变更时主动失效 + 递增 policyVersion。
//  门户模块清单按 permissions.index 全序(正数升序在前,负数区 -1 最先)。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <string>
#include <vector>

class ZmDbModule;
class ZmUserModule;

class ZmPermissionModule
{
public:
    explicit ZmPermissionModule(ZmDbModule* db, ZmUserModule* user);
    ~ZmPermissionModule();

    drogon::Task<ZMJSON> GetRole(const std::string& code);
    drogon::Task<int> GetLevel(int64_t uid);
    /// 有效权限判定(模块级/资源级)
    drogon::Task<bool> HasPermission(int64_t uid, const std::string& permCode);
    /// 有效权限 code 集合(Redis 缓存,miss 查库回写)
    drogon::Task<std::vector<std::string>> GetEffectiveCodes(int64_t uid);
    /// 门户模块清单 [{code,name,url,index}](按 index 全序)
    drogon::Task<ZMJSON> GetModuleList(int64_t uid);
    /// 提权/降权:等级压制(操作者 level > 目标;提升最高到操作者下一级)
    drogon::Task<bool> ChangeRole(int operatorLevel, int64_t targetUid,
                                  const std::string& newRoleCode, std::string& errMsg);
    /// 单人授权/拒绝(UNIQUE(uid,perm_code) 覆盖;grantType 1=授予 2=拒绝)
    drogon::Task<bool> SetUserPermission(int64_t uid, const std::string& permCode,
                                         int grantType, int64_t grantBy);
    /// 变更后失效缓存 + 递增策略版本
    drogon::Task<void> InvalidatePermCache(int64_t uid);

    // ── 角色/权限数据只读 ──
    drogon::Task<ZMJSON> ListRoles();
    drogon::Task<ZMJSON> ListPermCodes();

private:
    ZmDbModule* m_db = nullptr;
    ZmUserModule* m_user = nullptr;
};

#endif // ZM_MODULE_PERMISSION_H
