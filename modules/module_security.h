#ifndef ZM_MODULE_SECURITY_H
#define ZM_MODULE_SECURITY_H

// ============================================================================
// ZmSecurityModule:安全防护模块(设计文档 §3.6)
//  数据归属:login_locks、rate_limits、security_events。
//  登录阶梯锁定:每 5 次失败升一档(档 1 锁 15min,档 2+ 锁 60min),锁定中不校验
//  不计数;双维度:解析出用户按 uid(防换标识绕过),解析不出按 login_key(防枚举),
//  查询先 uid 后 login_key。
//  多维限流:场景(register/reset/login/verify_code)× 维度(ip/uid/login_key/device)
//  滑动窗口 60s/20 次,落库持久化。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <string>

class ZmDbModule;

class ZmSecurityModule
{
public:
    explicit ZmSecurityModule(ZmDbModule* db);
    ~ZmSecurityModule();

    /// @return {locked, retryAfter}(剩余秒)
    drogon::Task<ZMJSON> CheckLocked(const std::string& loginKey, int64_t uid,
                                     const std::string& ip);
    /// 失败计数 + 档位推进(调用前须已通过 CheckLocked)
    drogon::Task<bool> RecordLoginFail(const std::string& loginKey, int64_t uid,
                                       const std::string& ip);
    /// 成功清零(删除锁定记录)
    drogon::Task<bool> ClearLock(const std::string& loginKey, int64_t uid,
                                 const std::string& ip);
    /// 限流:窗口内未超限返回 true(任一超限 false)
    drogon::Task<bool> CheckLimit(const std::string& scope, const std::string& dimension,
                                  const std::string& dimKey);
    drogon::Task<bool> BumpLimit(const std::string& scope, const std::string& dimension,
                                 const std::string& dimKey);
    /// 安全事件落库(login_anomaly/brute_force/admin_reset/force_change/permission_denied 等)
    drogon::Task<bool> RecordEvent(int64_t uid, const std::string& account,
                                   const std::string& eventType, const std::string& ip,
                                   const std::string& ua, const std::string& detail);

    /// 默认限流参数(60s 窗口 20 次)
    static int64_t LimitWindowSec() { return 60; }
    static int64_t LimitMaxCount() { return 20; }

private:
    ZmDbModule* m_db = nullptr;
};

#endif // ZM_MODULE_SECURITY_H
