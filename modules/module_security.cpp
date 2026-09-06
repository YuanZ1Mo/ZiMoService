#include "modules/module_security.h"

#include "modules/module_db.h"

#include <zm_util_logger.h>

// ============================================================================
// 构造
// ============================================================================
ZmSecurityModule::ZmSecurityModule(ZmDbModule* db)
    : m_db(db)
{
}

ZmSecurityModule::~ZmSecurityModule() = default;

// ============================================================================
// 登录锁定
// ============================================================================
drogon::Task<ZMJSON> ZmSecurityModule::CheckLocked(const std::string& loginKey,
                                                   int64_t uid,
                                                   const std::string& ip)
{
    ZMJSON out = ZMJSON::object();
    out["locked"] = false;
    out["retryAfter"] = 0;
    int64_t now = ZmDbModule::Now();
    ZMJSON row;
    // 查询先按 uid(能解析出用户时)
    if (uid > 0)
    {
        row = co_await m_db->QueryRow(
            "SELECT fail_count, locked_until FROM login_locks "
            "WHERE uid=?1 AND ip=?2;",
            {std::to_string(uid), ip});
    }
    // 否则按 login_key
    if (!zm_json_has(row, "locked_until"))
    {
        row = co_await m_db->QueryRow(
            "SELECT fail_count, locked_until FROM login_locks "
            "WHERE login_key=?1 AND ip=?2;",
            {loginKey, ip});
    }
    if (zm_json_has(row, "locked_until"))
    {
        int64_t until = zm_json_get_int(row, "locked_until", 0);
        if (until > now)
        {
            out["locked"] = true;
            out["retryAfter"] = static_cast<int64_t>(until - now);
        }
    }
    co_return out;
}

drogon::Task<bool> ZmSecurityModule::RecordLoginFail(const std::string& loginKey,
                                                     int64_t uid,
                                                     const std::string& ip)
{
    int64_t now = ZmDbModule::Now();
    if (uid > 0)
    {
        // 主维度 uid(login_key 冗余记录备查)
        co_return co_await m_db->Exec(
            "INSERT INTO login_locks(uid, login_key, ip, fail_count, locked_until, last_fail_at) "
            "VALUES(?1,?2,?3,1,0,?4) "
            "ON CONFLICT(uid, ip) DO UPDATE SET "
            "login_key=excluded.login_key, fail_count=login_locks.fail_count+1, "
            "last_fail_at=excluded.last_fail_at, "
            "locked_until=CASE "
            "  WHEN (login_locks.fail_count+1)>=5 AND (login_locks.fail_count+1)<10 THEN ?4+900 "
            "  WHEN (login_locks.fail_count+1)>=10 THEN ?4+3600 "
            "  ELSE login_locks.locked_until END;",
            {std::to_string(uid), loginKey, ip, std::to_string(now)});
    }
    // 兜底维度 login_key(uid 空)
    co_return co_await m_db->Exec(
        "INSERT INTO login_locks(uid, login_key, ip, fail_count, locked_until, last_fail_at) "
        "VALUES(NULL,?1,?2,1,0,?3) "
        "ON CONFLICT(login_key, ip) DO UPDATE SET "
        "fail_count=login_locks.fail_count+1, last_fail_at=excluded.last_fail_at, "
        "locked_until=CASE "
        "  WHEN (login_locks.fail_count+1)>=5 AND (login_locks.fail_count+1)<10 THEN ?3+900 "
        "  WHEN (login_locks.fail_count+1)>=10 THEN ?3+3600 "
        "  ELSE login_locks.locked_until END;",
        {loginKey, ip, std::to_string(now)});
}

drogon::Task<bool> ZmSecurityModule::ClearLock(const std::string& loginKey,
                                               int64_t uid, const std::string& ip)
{
    bool ok = true;
    if (uid > 0)
    {
        ok = co_await m_db->Exec("DELETE FROM login_locks WHERE uid=?1 AND ip=?2;",
                                 {std::to_string(uid), ip});
    }
    bool ok2 = co_await m_db->Exec("DELETE FROM login_locks WHERE login_key=?1 AND ip=?2;",
                                   {loginKey, ip});
    co_return ok && ok2;
}

// ============================================================================
// 多维限流(滑动窗口 60s / 20 次;落库持久化,重启不丢)
// ============================================================================
drogon::Task<bool> ZmSecurityModule::CheckLimit(const std::string& scope,
                                                const std::string& dimension,
                                                const std::string& dimKey)
{
    auto row = co_await m_db->QueryRow(
        "SELECT window_start, count FROM rate_limits "
        "WHERE scope=?1 AND dimension=?2 AND dim_key=?3;",
        {scope, dimension, dimKey});
    if (!zm_json_has(row, "window_start"))
        co_return true;
    int64_t ws = zm_json_get_int(row, "window_start", 0);
    int64_t cnt = zm_json_get_int(row, "count", 0);
    int64_t now = ZmDbModule::Now();
    if (now - ws >= LimitWindowSec())
        co_return true;   // 窗口滑过,自然恢复
    co_return cnt < LimitMaxCount();
}

drogon::Task<bool> ZmSecurityModule::BumpLimit(const std::string& scope,
                                               const std::string& dimension,
                                               const std::string& dimKey)
{
    int64_t now = ZmDbModule::Now();
    // UPSERT:窗口滑过则重置,否则计数 +1
    co_return co_await m_db->Exec(
        "INSERT INTO rate_limits(scope, dimension, dim_key, window_start, count) "
        "VALUES(?1,?2,?3,?4,1) "
        "ON CONFLICT(scope, dimension, dim_key) DO UPDATE SET "
        "window_start=CASE WHEN ?4-rate_limits.window_start>=?5 THEN ?4 ELSE rate_limits.window_start END, "
        "count=CASE WHEN ?4-rate_limits.window_start>=?5 THEN 1 ELSE rate_limits.count+1 END;",
        {scope, dimension, dimKey, std::to_string(now), std::to_string(LimitWindowSec())});
}

// ============================================================================
// 安全事件
// ============================================================================
drogon::Task<bool> ZmSecurityModule::RecordEvent(int64_t uid,
                                                 const std::string& account,
                                                 const std::string& eventType,
                                                 const std::string& ip,
                                                 const std::string& ua,
                                                 const std::string& detail)
{
    // uid<=0 表示未登录/无用户场景,落 0(列可空;文本绑定统一走字符串)
    co_return co_await m_db->Exec(
        "INSERT INTO security_events(uid, account, event_type, ip, ua, detail, create_time) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7);",
        {std::to_string(uid > 0 ? uid : 0), account, eventType,
         ip, ua, detail.empty() ? std::string("") : detail, std::to_string(ZmDbModule::Now())});
}
