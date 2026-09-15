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
    /// 单人授权(全域:门户模块 + 功能权限点;按目标集合 diff:勾选=授予,
    /// 取消=拒绝/清除覆盖;与角色默认的差集落 user_permissions,空 diff 不写库)
    /// @param targetCodes 用户最终应持有的权限 code 全集(去重)
    /// @param diff [out] 变更明细 granted/denied/cleared(code 列表),供审计
    drogon::Task<bool> SetUserPermissions(int64_t uid,
                                          const std::vector<std::string>& targetCodes,
                                          int64_t grantBy, ZMJSON& diff);
    /// 变更后失效缓存 + 递增策略版本
    drogon::Task<void> InvalidatePermCache(int64_t uid);

    /// 登记权限点(幂等)并挂到指定角色(角色不存在则跳过)
    ///
    /// 同步实现:在装配阶段(事件循环启动之前)调用,阻塞当前线程是可接受的;
    /// 与建库种子同类,不需要协程投递。
    ///
    /// @param perm  {"code","name","module","url","type","index","sort","description"}
    /// @param roleCodes 需要挂载该权限点的角色 code
    /// @return true 全部写入成功
    bool RegisterPermCodeSync(const ZMJSON& perm,
                              const std::vector<std::string>& roleCodes);

    // ── 角色/权限数据只读 ──
    drogon::Task<ZMJSON> ListRoles();
    drogon::Task<ZMJSON> ListPermCodes();

private:
    ZmDbModule* m_db = nullptr;
    ZmUserModule* m_user = nullptr;
};

#endif // ZM_MODULE_PERMISSION_H
