#include "modules/module_user.h"

#include "modules/module_db.h"

#include <zm_util_logger.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>

// ============================================================================
// 构造
// ============================================================================
ZmUserModule::ZmUserModule(ZmDbModule* db)
    : m_db(db)
{
}

ZmUserModule::~ZmUserModule() = default;

// ============================================================================
// 账号/昵称规则
// ============================================================================
bool ZmUserModule::ValidateAccountFormat(const std::string& raw, std::string& errMsg)
{
    std::string account = raw;
    std::transform(account.begin(), account.end(), account.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (account.size() < 4 || account.size() > 30)
    {
        errMsg = "账号长度须为 4-30 位";
        return false;
    }
    if (account.front() == '_' || account.front() == '-' ||
        account.back() == '_' || account.back() == '-')
    {
        errMsg = "账号首尾不能是 _ 或 -";
        return false;
    }
    for (char c : account)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok)
        {
            errMsg = "账号仅允许小写字母、数字、_、-";
            return false;
        }
    }
    errMsg.clear();
    return true;
}

bool ZmUserModule::ValidateNickname(const std::string& nickname, std::string& errMsg)
{
    if (nickname.empty() || nickname.size() > 20)
    {
        errMsg = "昵称长度须为 1-20 个字符";
        return false;
    }
    for (unsigned char c : nickname)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x0B || c == 0x0C)
        {
            errMsg = "昵称不能包含空白字符";
            return false;
        }
    }
    errMsg.clear();
    return true;
}

drogon::Task<bool> ZmUserModule::IsAccountUnique(const std::string& account)
{
    auto row = co_await m_db->QueryRow(
        "SELECT COUNT(*) AS c FROM users WHERE account=?1;", {account});
    co_return zm_json_get_int(row, "c", 0) == 0;
}

// ============================================================================
// 登录标识归一化
// ============================================================================
std::string ZmUserModule::NormalizeLoginKey(const std::string& raw)
{
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 手机号去格式(去空格/横线/括号;手机号含 '+' 前缀保留)
    std::string out;
    out.reserve(key.size());
    for (char c : key)
    {
        if (c == ' ' || c == '-' || c == '(' || c == ')')
            continue;
        out += c;
    }
    return out;
}

drogon::Task<ZMJSON> ZmUserModule::FindByLoginKey(const std::string& loginKey)
{
    // account/email/phone 任一命中(均唯一)
    auto row = co_await m_db->QueryRow(
        "SELECT * FROM users WHERE account=?1 OR email=?1 OR phone=?1 LIMIT 1;",
        {loginKey});
    co_return row;
}

drogon::Task<ZMJSON> ZmUserModule::FindByUid(int64_t uid)
{
    auto row = co_await m_db->QueryRow(
        "SELECT u.*, p.avatar, p.signature, p.preferences "
        "FROM users u LEFT JOIN user_profile p ON u.uid = p.uid "
        "WHERE u.uid=?1;",
        {std::to_string(uid)});
    co_return row;
}

// ============================================================================
// 建行(users + profile 同一事务)
// ============================================================================
drogon::Task<ZMJSON> ZmUserModule::CreateUser(const std::string& account,
                                              const std::string& nickname,
                                              const std::string& passSalt,
                                              const std::string& passHash,
                                              const std::string& ip,
                                              const std::string& roleCode)
{
    ZMJSON result = ZMJSON::object();
    int64_t now = ZmDbModule::Now();
    bool ok = co_await m_db->WithTx([&](ZmSqliteDb& db) -> bool {
        // uid 分配:10000001 起递增;单写队列串行保证 MAX+1 无并发竞态
        auto row = db.QueryRowSync("SELECT MAX(uid) AS m FROM users;", {});
        int64_t base = zm_json_get_int(row, "m", 10000000);
        int64_t uid = std::max<int64_t>(base + 1, 10000001);
        if (!db.ExecSync(
                "INSERT INTO users(uid, account, nickname, pass_salt, pass_hash, "
                "register_ip, register_time, role_code, create_time) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                {std::to_string(uid), account, nickname, passSalt, passHash, ip,
                 std::to_string(now), roleCode, std::to_string(now)}))
            return false;
        if (!db.ExecSync(
                "INSERT INTO user_profile(uid, updated_at) VALUES(?1,?2);",
                {std::to_string(uid), std::to_string(now)}))
            return false;
        result["uid"] = uid;
        return true;
    });
    if (!ok)
    {
        DEFAULT_LOG_WARN("ZmUserModule::CreateUser 失败: account={}", account);
        result = ZMJSON::object();
    }
    co_return result;
}

// ============================================================================
// 列表 / 列元数据
// ============================================================================
drogon::Task<ZMJSON> ZmUserModule::ListUsers(int page, int size,
                                             const std::string& search,
                                             const std::string& role, int status,
                                             bool includeDeleted)
{
    ZMJSON out = ZMJSON::object();
    if (page < 1)
        page = 1;
    if (size < 1 || size > 200)
        size = 20;
    std::string where = " WHERE 1=1 ";
    std::vector<std::string> params;
    if (!includeDeleted)
    {
        where += " AND u.deleted=0 ";
    }
    if (status == 1 || status == 2)
    {
        where += " AND u.status=? ";
        params.push_back(std::to_string(status));
    }
    if (!role.empty())
    {
        where += " AND u.role_code=? ";
        params.push_back(role);
    }
    if (!search.empty())
    {
        where += " AND (u.account LIKE ? OR u.nickname LIKE ?) ";
        std::string like = "%" + search + "%";
        params.push_back(like);
        params.push_back(like);
    }
    std::string countSql = "SELECT COUNT(*) AS c FROM users u" + where + ";";
    auto cntRow = co_await m_db->QueryRow(countSql, params);
    out["total"] = zm_json_get_int(cntRow, "c", 0);

    std::string listSql =
        "SELECT u.uid, u.account, u.nickname, u.email, u.phone, u.status, u.deleted, "
        "u.role_code, u.force_change, u.register_time, u.register_ip, "
        "u.last_login_time, u.last_login_ip "
        "FROM users u" + where + " ORDER BY u.uid LIMIT ? OFFSET ?;";
    std::vector<std::string> lp = params;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string(static_cast<int64_t>(page - 1) * size));
    ZMJSON rows = co_await m_db->QueryRows(listSql, lp);
    out["list"] = rows;
    co_return out;
}

ZMJSON ZmUserModule::GetColumnsMeta()
{
    // 服务端下发列元数据(列名/顺序/显示名/可否排序/可编辑/置灰),列表与表单共用
    ZMJSON cols = ZMJSON::array();
    auto add = [&cols](const std::string& key, const std::string& label,
                       bool editable, bool disabled, bool visible, bool sortable,
                       const std::string& type = "text") {
        ZMJSON c = ZMJSON::object();
        c["key"] = key;
        c["label"] = label;
        c["editable"] = editable;
        c["disabled"] = disabled;
        c["visible"] = visible;
        c["sortable"] = sortable;
        c["type"] = type;
        cols.push_back(std::move(c));
    };
    add("uid", "UID", false, true, true, true);
    add("account", "账号", true, false, true, true);
    add("nickname", "昵称", true, false, true, true);
    add("email", "邮箱", true, false, true, true);
    add("phone", "手机号", true, false, true, true);
    add("role_code", "角色", false, true, true, false);
    add("status", "状态", true, false, true, true, "status");
    add("deleted", "已删除", false, true, true, false);
    add("force_change", "强制改密", false, true, false, false);
    add("register_time", "注册时间", false, true, true, true);
    add("last_login_time", "最近登录", false, true, true, true);
    add("register_ip", "注册IP", false, true, false, false);
    add("last_login_ip", "最近登录IP", false, true, false, false);
    // 不可改敏感列(置灰/不展示):id/pass_salt/pass_hash/temp_pass_*
    add("id", "ID", false, true, false, false);
    add("pass_salt", "密码盐", false, true, false, false);
    add("pass_hash", "密码哈希", false, true, false, false);
    add("temp_pass_salt", "临时密码盐", false, true, false, false);
    add("temp_pass_hash", "临时密码哈希", false, true, false, false);
    return cols;
}

// ============================================================================
// 行更新(白名单强制)
// ============================================================================
drogon::Task<bool> ZmUserModule::UpdateUserRow(int64_t uid,
                                               const std::vector<std::string>& whitelist,
                                               const ZMJSON& patch)
{
    if (!patch.is_object())
        co_return false;
    // 允许更新集合 = 白名单 ∩ patch 字段
    static const std::unordered_set<std::string> kBase = {
        "account", "nickname", "email", "phone", "status"};
    static const std::unordered_set<std::string> kProfile = {
        "avatar", "signature", "preferences"};
    std::vector<std::string> cols;
    std::vector<std::string> params;
    for (const auto& it : patch.items())
    {
        const std::string& key = it.key();
        bool inWhite = std::find(whitelist.begin(), whitelist.end(), key) != whitelist.end();
        if (!inWhite)
            continue;
        if (kBase.count(key) == 0 && kProfile.count(key) == 0)
            continue;   // 非基础/资料列(如 id/uid/pass_*)一律拒绝
        // 取值(字符串化;布尔/数字转字符串)
        std::string val;
        const ZMJSON& v = it.value();
        if (v.is_string())
            val = v.get<std::string>();
        else if (v.is_number_integer() || v.is_number_unsigned())
            val = std::to_string(v.get<int64_t>());
        else if (v.is_boolean())
            val = v.get<bool>() ? "1" : "0";
        else if (v.is_null())
            val.clear();
        else
            continue;

        if (key == "account")
        {
            std::string err;
            if (!ValidateAccountFormat(val, err))
                continue;
            val = NormalizeLoginKey(val);
            // 唯一性
            auto dup = m_db->QueryRowSync("SELECT uid FROM users WHERE account=?1 AND uid<>?2 LIMIT 1;",
                                          {val, std::to_string(uid)});
            if (zm_json_has(dup, "uid"))
                continue;
        }
        if (key == "nickname")
        {
            std::string err;
            if (!ValidateNickname(val, err))
                continue;
        }
        if (key == "status")
        {
            if (val != "1" && val != "2")
                continue;
        }
        cols.push_back(key);
        params.push_back(val);
    }
    if (cols.empty())
        co_return false;

    // 主表与资料表分写
    bool ok = co_await m_db->WithTx([&](ZmSqliteDb& db) -> bool {
        // 可空唯一列(email/phone):空串落 NULL,否则多个"无邮箱"用户互相撞 UNIQUE
        static const std::unordered_set<std::string> kNullableUnique = {"email", "phone"};
        for (const std::string& key : cols)
        {
            if (kProfile.count(key))
                continue;
            const size_t idx = static_cast<size_t>(
                std::find(cols.begin(), cols.end(), key) - cols.begin());
            std::string sql;
            std::vector<std::string> p;
            if (kNullableUnique.count(key) && params[idx].empty())
            {
                sql = "UPDATE users SET " + key + "=NULL WHERE uid=?1;";
                p = {std::to_string(uid)};
            }
            else
            {
                sql = "UPDATE users SET " + key + "=?1 WHERE uid=?2;";
                p = {params[idx], std::to_string(uid)};
            }
            if (!db.ExecSync(sql, p))
                return false;
        }
        // profile 列
        for (const std::string& key : cols)
        {
            if (!kProfile.count(key))
                continue;
            std::string sql = "UPDATE user_profile SET " + key + "=?1, updated_at=?2 WHERE uid=?3;";
            if (!db.ExecSync(sql, {params[std::find(cols.begin(), cols.end(), key) -
                                   cols.begin()], std::to_string(ZmDbModule::Now()),
                                  std::to_string(uid)}))
                return false;
        }
        return true;
    });
    co_return ok;
}

drogon::Task<bool> ZmUserModule::SetStatus(int64_t uid, int status)
{
    co_return co_await m_db->Exec("UPDATE users SET status=?1 WHERE uid=?2;",
                                  {std::to_string(status), std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::SetDeleted(int64_t uid, int deleted)
{
    co_return co_await m_db->Exec("UPDATE users SET deleted=?1 WHERE uid=?2;",
                                  {std::to_string(deleted), std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::SetForceChange(int64_t uid, int v)
{
    co_return co_await m_db->Exec("UPDATE users SET force_change=?1 WHERE uid=?2;",
                                  {std::to_string(v), std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::SetRole(int64_t uid, const std::string& roleCode)
{
    co_return co_await m_db->Exec("UPDATE users SET role_code=?1 WHERE uid=?2;",
                                  {roleCode, std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::SetTempPassword(int64_t uid, const std::string& salt,
                                                 const std::string& hash)
{
    co_return co_await m_db->Exec(
        "UPDATE users SET temp_pass_salt=?1, temp_pass_hash=?2 WHERE uid=?3;",
        {salt, hash, std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::ClearTempPassword(int64_t uid)
{
    co_return co_await m_db->Exec(
        "UPDATE users SET temp_pass_salt=NULL, temp_pass_hash=NULL WHERE uid=?1;",
        {std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::UpdatePassword(int64_t uid, const std::string& salt,
                                                const std::string& hash)
{
    co_return co_await m_db->Exec(
        "UPDATE users SET pass_salt=?1, pass_hash=?2 WHERE uid=?3;",
        {salt, hash, std::to_string(uid)});
}

drogon::Task<bool> ZmUserModule::TouchLastLogin(int64_t uid, const std::string& ip,
                                                int64_t now)
{
    co_return co_await m_db->Exec(
        "UPDATE users SET last_login_ip=?1, last_login_time=?2 WHERE uid=?3;",
        {ip, std::to_string(now), std::to_string(uid)});
}

drogon::Task<int64_t> ZmUserModule::CountActiveUsers()
{
    auto row = co_await m_db->QueryRow(
        "SELECT COUNT(*) AS c FROM users WHERE deleted=0;", {});
    co_return zm_json_get_int(row, "c", 0);
}

drogon::Task<ZMJSON> ZmUserModule::GetNicknames(const std::vector<int64_t>& uids)
{
    ZMJSON out = ZMJSON::object();
    std::vector<int64_t> uniq;
    for (int64_t uid : uids)
    {
        if (uid <= 0)
            continue;
        if (std::find(uniq.begin(), uniq.end(), uid) == uniq.end())
            uniq.push_back(uid);
    }
    if (uniq.empty())
        co_return out;
    // 一次查齐(列表页最多几百个 uid,单条 IN 查询即可)
    std::string in;
    std::vector<std::string> params;
    for (size_t i = 0; i < uniq.size(); ++i)
    {
        if (i > 0)
            in += ",";
        in += "?";
        params.push_back(std::to_string(uniq[i]));
    }
    auto rows = co_await m_db->QueryRows(
        "SELECT uid, nickname FROM users WHERE uid IN (" + in + ")", params);
    for (const auto& r : rows)
        out[std::to_string(zm_json_get_int(r, "uid", 0))] = zm_json_get_str(r, "nickname");
    co_return out;
}
