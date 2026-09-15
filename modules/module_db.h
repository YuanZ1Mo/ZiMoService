#ifndef ZM_MODULE_DB_H
#define ZM_MODULE_DB_H

// ============================================================================
// ZmDbModule:用户系统数据访问模块(模块划分见《2026-09-05-用户系统模块设计.md》§3.1)
//  职责:user.db 全新建表 + 种子;Redis 缓存/共享状态;周期清理任务。
//  连接与事务模型继承自公共库 ZmSqliteDb(写连接单写队列串行 + 读连接池多读并发,
//  WAL + busy_timeout;所有 SQL 经工作池离核)。
//  数据归属:本工程全部表(users 等 13 张)的 SQL 唯一出处,业务模块不直接拼 SQL。
// ============================================================================

#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>

#include <zm_util_json.h>
#include <zm_util_sqlite.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace drogon
{
namespace nosql
{
class RedisClient;
}
}  // namespace drogon

class ZmDbModule : public ZmSqliteDb
{
public:
    explicit ZmDbModule();
    ~ZmDbModule() override;

    /// 打开库 + 全新建表 + 种子(幂等:仅首次建库);后续表结构变更由运维手工执行 SQL
    /// @param dbPath 数据库文件绝对路径(exe 同级 db\user\user.db)
    bool Init(const std::string& dbPath);

    /// 注册事件循环启动回调:启动后注册每日 03:00 周期清理任务
    void StartPeriodicCleanup();

    // ── Redis 缓存/共享状态(值统一 JSON 字符串 UTF-8;失败只记日志+降级,不抛业务异常) ──
    /// @param ttlSec 0 = 不设过期
    drogon::Task<bool> RSet(const std::string& key, const std::string& val, int ttlSec = 0);
    /// 不存在/不可用返回空串
    drogon::Task<std::string> RGet(const std::string& key);
    drogon::Task<bool> RDel(const std::string& key);
    /// 自增;Redis 不可用时回退进程内原子计数器(不失效)
    drogon::Task<int64_t> RIncr(const std::string& key, int ttlSec = 0);
    drogon::Task<bool> RSetExpire(const std::string& key, int ttlSec);

    // ── 策略变更版本号(心跳轮询应答;Redis 优先,进程内回退) ──
    drogon::Task<int64_t> BumpPolicyVersion();
    drogon::Task<int64_t> GetPolicyVersion();

private:
    bool BuildSchema();
    bool SeedData();
    void CleanupOnce();

    /// Redis 客户端(惰性;启动期 TCP 探测可达后才创建;服务器不可用时命令抛异常 → 调用方降级)
    std::shared_ptr<drogon::nosql::RedisClient> Redis();
    /// 启动期 TCP 可达性探测(不可达则标记降级,不创建客户端,避免 hiredis 连接 ERROR 日志)
    static bool ProbeRedisTcp(const std::string& ip, uint16_t port, int timeoutMs);

    std::shared_ptr<drogon::nosql::RedisClient> m_redis;
    std::mutex m_redisMtx;
    std::atomic<int64_t> m_localPolicyVersion{0};
    std::atomic<bool> m_redisTried{false};
    std::atomic<bool> m_redisDown{false};   // Redis 首次失败后置位,后续直接降级
};

/// Redis 键命名空间助手(zimo:<域>:<维度>:<键值>)
namespace zm_redis_key
{
/// v2:userManager→systemManager 重命名后作废旧版缓存(旧值含废弃 code)
inline std::string PermCodes(int64_t uid) { return "zimo:perm:codes:v2:" + std::to_string(uid); }
inline std::string PermModules(int64_t uid) { return "zimo:perm:modules:v2:" + std::to_string(uid); }
inline std::string PolicyVersion() { return "zimo:policy:version"; }
}  // namespace zm_redis_key

#endif // ZM_MODULE_DB_H
