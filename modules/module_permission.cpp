#include "modules/module_permission.h"

#include "modules/module_db.h"
#include "modules/module_user.h"

#include <zm_util_logger.h>

#include <algorithm>
#include <sstream>
#include <unordered_set>

// ============================================================================
// 构造
// ============================================================================
ZmPermissionModule::ZmPermissionModule(ZmDbModule* db, ZmUserModule* user)
    : m_db(db), m_user(user)
{
}

ZmPermissionModule::~ZmPermissionModule() = default;

drogon::Task<ZMJSON> ZmPermissionModule::GetRole(const std::string& code)
{
    auto row = co_await m_db->QueryRow("SELECT * FROM roles WHERE code=?1;", {code});
    co_return row;
}

drogon::Task<int> ZmPermissionModule::GetLevel(int64_t uid)
{
    auto row = co_await m_db->QueryRow(
        "SELECT COALESCE(r.level,0) AS level FROM users u "
        "LEFT JOIN roles r ON u.role_code=r.code WHERE u.uid=?1;",
        {std::to_string(uid)});
    co_return zm_json_get_int(row, "level", 0);
}

// ============================================================================
// 有效权限推导(角色默认 ∪ 授予 − 拒绝)
// ============================================================================
drogon::Task<std::vector<std::string>> ZmPermissionModule::GetEffectiveCodes(int64_t uid)
{
    std::vector<std::string> codes;
    // 优先 Redis 缓存
    std::string cached = co_await m_db->RGet(zm_redis_key::PermCodes(uid));
    if (!cached.empty())
    {
        std::string err;
        ZMJSON arr = zm_json_parse(cached, err);
        if (err.empty() && arr.is_array())
        {
            for (const auto& v : arr)
                codes.push_back(v.get<std::string>());
            co_return codes;
        }
    }
    // 查库推导
    codes = co_await [this, uid]() -> drogon::Task<std::vector<std::string>> {
        auto row = co_await m_db->QueryRow(
            "SELECT u.role_code, COALESCE(r.permission_codes,'[]') AS perms "
            "FROM users u LEFT JOIN roles r ON u.role_code=r.code WHERE u.uid=?1;",
            {std::to_string(uid)});
        std::vector<std::string> out;
        std::string perms = zm_json_get_str(row, "perms", "[]");
        std::string err;
        ZMJSON arr = zm_json_parse(perms, err);
        if (err.empty() && arr.is_array())
        {
            for (const auto& v : arr)
            {
                if (v.is_string())
                    out.push_back(v.get<std::string>());
            }
        }
        // 单人授予
        auto grants = co_await m_db->QueryRows(
            "SELECT perm_code, grant_type FROM user_permissions WHERE uid=?1;",
            {std::to_string(uid)});
        std::unordered_set<std::string> denies;
        for (const auto& g : grants)
        {
            std::string code = zm_json_get_str(g, "perm_code");
            int gt = zm_json_get_int(g, "grant_type", 1);
            if (gt == 2)
            {
                denies.insert(code);
                out.erase(std::remove(out.begin(), out.end(), code), out.end());
            }
            else
            {
                out.push_back(code);
            }
        }
        // 去重(保留顺序)
        std::vector<std::string> dedup;
        std::unordered_set<std::string> seen;
        for (auto& c : out)
        {
            if (denies.count(c))
                continue;
            if (seen.insert(c).second)
                dedup.push_back(std::move(c));
        }
        co_return dedup;
    }();
    // 回写缓存(TTL 600s,与心跳/续期周期对齐)
    if (!codes.empty())
    {
        ZMJSON arr = ZMJSON::array();
        for (auto& c : codes)
            arr.push_back(c);
        co_await m_db->RSet(zm_redis_key::PermCodes(uid), zm_json_dump(arr), 600);
    }
    co_return codes;
}

drogon::Task<bool> ZmPermissionModule::HasPermission(int64_t uid,
                                                     const std::string& permCode)
{
    auto codes = co_await GetEffectiveCodes(uid);
    co_return std::find(codes.begin(), codes.end(), permCode) != codes.end();
}

// ============================================================================
// 门户模块清单
// ============================================================================
drogon::Task<ZMJSON> ZmPermissionModule::GetModuleList(int64_t uid)
{
    ZMJSON out = ZMJSON::array();
    std::string cached = co_await m_db->RGet(zm_redis_key::PermModules(uid));
    if (!cached.empty())
    {
        std::string err;
        ZMJSON arr = zm_json_parse(cached, err);
        if (err.empty() && arr.is_array())
            co_return arr;
    }
    auto codes = co_await GetEffectiveCodes(uid);
    if (codes.empty())
        co_return out;
    // IN 子句
    std::stringstream in;
    std::vector<std::string> params;
    for (size_t i = 0; i < codes.size(); ++i)
    {
        if (i)
            in << ",";
        in << "?" << (i + 1);
        params.push_back(codes[i]);
    }
    auto rows = co_await m_db->QueryRows(
        "SELECT code, name, url, \"index\" FROM permissions "
        "WHERE enabled=1 AND type=0 AND \"index\"<>0 AND code IN (" + in.str() + ") "
        "ORDER BY (\"index\">0) DESC, ABS(\"index\") ASC, sort ASC;",
        params);
    for (auto& r : rows)
    {
        ZMJSON m = ZMJSON::object();
        m["code"] = zm_json_get_str(r, "code");
        m["name"] = zm_json_get_str(r, "name");
        m["url"] = zm_json_get_str(r, "url");
        m["index"] = zm_json_get_int(r, "index", 0);
        out.push_back(std::move(m));
    }
    co_await m_db->RSet(zm_redis_key::PermModules(uid), zm_json_dump(out), 600);
    co_return out;
}

// ============================================================================
// 提权/降权 / 单人授权
// ============================================================================
drogon::Task<bool> ZmPermissionModule::ChangeRole(int operatorLevel, int64_t targetUid,
                                                  const std::string& newRoleCode,
                                                  std::string& errMsg)
{
    auto role = co_await GetRole(newRoleCode);
    if (!zm_json_has(role, "code"))
    {
        errMsg = "角色不存在";
        co_return false;
    }
    int newLevel = zm_json_get_int(role, "level", 0);
    int targetLevel = co_await GetLevel(targetUid);
    if (operatorLevel <= targetLevel)
    {
        errMsg = "等级压制:仅可操作等级低于自己的用户";
        co_return false;
    }
    if (newLevel >= operatorLevel)
    {
        errMsg = "提升最高到操作者的下一级";
        co_return false;
    }
    bool ok = co_await m_user->SetRole(targetUid, newRoleCode);
    if (ok)
        co_await InvalidatePermCache(targetUid);
    else
        errMsg = "角色更新失败";
    co_return ok;
}

drogon::Task<bool> ZmPermissionModule::SetUserPermission(int64_t uid,
                                                         const std::string& permCode,
                                                         int grantType, int64_t grantBy)
{
    if (grantType != 1 && grantType != 2)
        co_return false;
    auto p = co_await m_db->QueryRow("SELECT code FROM permissions WHERE code=?1;",
                                     {permCode});
    if (!zm_json_has(p, "code"))
        co_return false;
    int64_t now = ZmDbModule::Now();
    bool ok = co_await m_db->Exec(
        "INSERT INTO user_permissions(uid, perm_code, grant_type, grant_by, create_time) "
        "VALUES(?1,?2,?3,?4,?5) "
        "ON CONFLICT(uid, perm_code) DO UPDATE SET grant_type=excluded.grant_type, "
        "grant_by=excluded.grant_by, create_time=excluded.create_time;",
        {std::to_string(uid), permCode, std::to_string(grantType),
         std::to_string(grantBy), std::to_string(now)});
    if (ok)
        co_await InvalidatePermCache(uid);
    co_return ok;
}

drogon::Task<void> ZmPermissionModule::InvalidatePermCache(int64_t uid)
{
    co_await m_db->RDel(zm_redis_key::PermCodes(uid));
    co_await m_db->RDel(zm_redis_key::PermModules(uid));
    co_await m_db->BumpPolicyVersion();
}

drogon::Task<ZMJSON> ZmPermissionModule::ListRoles()
{
    auto rows = co_await m_db->QueryRows(
        "SELECT code, name, level, permission_codes, description FROM roles "
        "ORDER BY level DESC, sort ASC;",
        {});
    co_return rows;
}

drogon::Task<ZMJSON> ZmPermissionModule::ListPermCodes()
{
    // 注意:index 是 SQLite 保留字,列名必须加引号,否则准备语句直接语法错误
    auto rows = co_await m_db->QueryRows(
        "SELECT code, name, module, url, type, \"index\", enabled FROM permissions "
        "ORDER BY sort ASC;",
        {});
    co_return rows;
}
