#ifndef ZM_MODULE_USER_H
#define ZM_MODULE_USER_H

// ============================================================================
// ZmUserModule:用户数据模块(设计文档 §3.2)
//  数据归属:users、user_profile。账号规则、login_key 归一化、uid 分配、
//  用户建行(含 profile 同步)、状态/角色/force_change 读写、列表查询、列元数据。
//  密码盐/哈希只落库不计算;pass_salt/pass_hash/temp_pass_*/id 为不可改列。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <string>
#include <vector>

class ZmDbModule;

class ZmUserModule
{
public:
    explicit ZmUserModule(ZmDbModule* db);
    ~ZmUserModule();

    // ── 账号规则 ──
    /// 账号 4-30 位 [a-z0-9_-],首尾非 _/-,统一小写
    bool ValidateAccountFormat(const std::string& account, std::string& errMsg);
    /// 昵称 1-20 码点,不含空白
    bool ValidateNickname(const std::string& nickname, std::string& errMsg);
    drogon::Task<bool> IsAccountUnique(const std::string& account);

    // ── 登录标识 ──
    /// account 小写 / email 小写 / phone 去格式
    static std::string NormalizeLoginKey(const std::string& raw);
    drogon::Task<ZMJSON> FindByLoginKey(const std::string& loginKey);
    /// 含 profile 合并结果
    drogon::Task<ZMJSON> FindByUid(int64_t uid);

    // ── 建行 ──
    /// users + user_profile 同一事务;成功返回 {uid},失败返回空对象
    drogon::Task<ZMJSON> CreateUser(const std::string& account,
                                    const std::string& nickname,
                                    const std::string& passSalt,
                                    const std::string& passHash,
                                    const std::string& ip,
                                    const std::string& roleCode);

    // ── 列表 / 列元数据 ──
    /// @return {total, list:[...]};includeDeleted=false 时默认不含软删除
    drogon::Task<ZMJSON> ListUsers(int page, int size, const std::string& search,
                                   const std::string& role, int status,
                                   bool includeDeleted);
    /// 用户表列元数据(列名/顺序/显示名/可否排序/可编辑/置灰),列表与表单共用
    ZMJSON GetColumnsMeta();

    // ── 行更新(白名单强制) ──
    /// @param whitelist 允许更新的列名子集;patch 中不在白名单的字段忽略
    drogon::Task<bool> UpdateUserRow(int64_t uid, const std::vector<std::string>& whitelist,
                                     const ZMJSON& patch);
    drogon::Task<bool> SetStatus(int64_t uid, int status);
    drogon::Task<bool> SetDeleted(int64_t uid, int deleted);
    drogon::Task<bool> SetForceChange(int64_t uid, int v);
    drogon::Task<bool> SetRole(int64_t uid, const std::string& roleCode);
    drogon::Task<bool> SetTempPassword(int64_t uid, const std::string& salt,
                                       const std::string& hash);
    drogon::Task<bool> ClearTempPassword(int64_t uid);
    drogon::Task<bool> UpdatePassword(int64_t uid, const std::string& salt,
                                      const std::string& hash);
    drogon::Task<bool> TouchLastLogin(int64_t uid, const std::string& ip, int64_t now);

    /// 有效用户数(注册角色分配:首个注册用户 → developer)
    drogon::Task<int64_t> CountActiveUsers();

private:
    ZmDbModule* m_db = nullptr;
};

#endif // ZM_MODULE_USER_H
