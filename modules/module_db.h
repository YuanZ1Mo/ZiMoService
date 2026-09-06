#ifndef ZM_MODULE_DB_H
#define ZM_MODULE_DB_H

// ============================================================================
// ZmDbModule:数据访问模块(模块划分见《2026-09-05-用户系统模块设计.md》§3.1)
//  职责:SQLite 持久化(user.db)全新建表 + 种子;通用 DAO(占位参数绑定、
//        行/多行 → JSON);事务封装;Redis 缓存/共享状态;周期清理任务。
//  数据归属:本工程全部表(users 等 13 张)的 SQL 唯一出处,业务模块不直接拼 SQL。
//  线程模型:所有 SQL 经工作池(事件循环离核)执行;写连接单写队列串行,
//        读连接池(多读并发),WAL + busy_timeout。
// ============================================================================

#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>

#include <zm_util_json.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct ZmSqliteConn;

namespace drogon
{
namespace nosql
{
class RedisClient;
}
}  // namespace drogon

class ZmDbModule
{
public:
    explicit ZmDbModule();
    ~ZmDbModule();

    /// 打开库 + 全新建表 + 种子(幂等:仅首次建库);后续表结构变更由运维手工执行 SQL
    /// @param dbPath 数据库文件绝对路径(exe 同级 db\user.db)
    bool Init(const std::string& dbPath);

    /// 注册事件循环启动回调:启动后注册每日 03:00 周期清理任务
    void StartPeriodicCleanup();

    // ── 协程数据访问(工作池离核;事件循环不阻塞) ──
    drogon::Task<bool> Exec(const std::string& sql,
                            const std::vector<std::string>& params = {});
    drogon::Task<ZMJSON> QueryRows(const std::string& sql,
                                   const std::vector<std::string>& params = {});
    drogon::Task<ZMJSON> QueryRow(const std::string& sql,
                                  const std::vector<std::string>& params = {});
    /// 事务封装:回调内写操作原子提交/回滚;回调在专用写线程(单写队列)执行,
    /// 回调内只能调用本模块 ExecSync/Query*Sync,不得再 co_await。
    drogon::Task<bool> WithTx(const std::function<bool(ZmDbModule&)>& fn);

    // ── 同步内部方法(仅 WithTx 回调 / RunOnPool 上下文内调用) ──
    bool ExecSync(const std::string& sql, const std::vector<std::string>& params);
    ZMJSON QueryRowSync(const std::string& sql, const std::vector<std::string>& params);
    ZMJSON QueryRowsSync(const std::string& sql, const std::vector<std::string>& params);
    bool BeginTxSync();
    bool CommitTxSync();
    bool RollbackTxSync();

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

    /// 数据库是否可用(Init 失败等)
    bool IsReady() const { return m_ready.load(); }

    /// 当前 unix 秒
    static int64_t Now();

private:
    /// 内部:工作池线程内执行的同步执行体
    bool ExecSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                        const std::vector<std::string>& params);
    ZMJSON QueryRowSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                              const std::vector<std::string>& params);
    ZMJSON QueryRowsSyncLocked(ZmSqliteConn* conn, const std::string& sql,
                               const std::vector<std::string>& params);
    bool BuildSchema();
    bool SeedData();
    void CleanupOnce();
    static int64_t NextUidSync(ZmSqliteConn* conn);

    ZmSqliteConn* AcquireReadConn();
    void ReleaseReadConn(ZmSqliteConn* conn);

    /// Redis 客户端(惰性;启动期 TCP 探测可达后才创建;服务器不可用时命令抛异常 → 调用方降级)
    std::shared_ptr<drogon::nosql::RedisClient> Redis();
    /// 启动期 TCP 可达性探测(不可达则标记降级,不创建客户端,避免 hiredis 连接 ERROR 日志)
    static bool ProbeRedisTcp(const std::string& ip, uint16_t port, int timeoutMs);

    bool CreateConnections();
    void CloseConnections();

    // 连接(写 1 + 读池 4)
    std::unique_ptr<ZmSqliteConn> m_write;
    std::vector<std::unique_ptr<ZmSqliteConn>> m_reads;
    std::atomic<size_t> m_readCursor{0};

    std::shared_ptr<drogon::nosql::RedisClient> m_redis;
    std::mutex m_redisMtx;
    std::atomic<int64_t> m_localPolicyVersion{0};
    std::atomic<bool> m_redisTried{false};
    std::atomic<bool> m_redisDown{false};   // Redis 首次失败后置位,后续直接降级
    std::atomic<bool> m_ready{false};
    std::string m_dbPath;
};

/// Redis 键命名空间助手(zimo:<域>:<维度>:<键值>)
namespace zm_redis_key
{
inline std::string PermCodes(int64_t uid) { return "zimo:perm:codes:" + std::to_string(uid); }
inline std::string PermModules(int64_t uid) { return "zimo:perm:modules:" + std::to_string(uid); }
inline std::string PolicyVersion() { return "zimo:policy:version"; }
}  // namespace zm_redis_key

#endif // ZM_MODULE_DB_H
