#include "modules/module_db.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <drogon/HttpAppFramework.h>
#include <trantor/net/InetAddress.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/nosql/RedisException.h>

#include <zm_net_http_server.h>
#include <zm_util_logger.h>

#include <cstring>
#include <ctime>

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================
ZmDbModule::ZmDbModule() = default;

ZmDbModule::~ZmDbModule() = default;

bool ZmDbModule::Init(const std::string& dbPath)
{
    if (!ZmSqliteDb::Init(dbPath))
    {
        DEFAULT_LOG_ERROR("ZmDbModule::Init: 打开库失败: {}", dbPath);
        return false;
    }

    // 运行配置:WAL + busy_timeout + 外键(写连接设置,persistent)
    ExecSync("PRAGMA journal_mode=WAL;", {});
    ExecSync("PRAGMA busy_timeout=5000;", {});
    ExecSync("PRAGMA foreign_keys=ON;", {});

    if (!BuildSchema())
    {
        DEFAULT_LOG_ERROR("ZmDbModule::Init: 建表失败");
        Close();
        return false;
    }
    if (!SeedData())
    {
        DEFAULT_LOG_ERROR("ZmDbModule::Init: 种子数据写入失败");
        Close();
        return false;
    }
    // Redis 可达性探测(启动期同步一次;不可达 → 降级,不创建客户端)
    if (!ProbeRedisTcp("127.0.0.1", 6379, 1000))
    {
        m_redisDown.store(true);
        DEFAULT_LOG_INFO("ZmDbModule: Redis 不可达(127.0.0.1:6379),降级为直查库/进程内值");
    }
    else
    {
        DEFAULT_LOG_INFO("ZmDbModule: Redis 可达(127.0.0.1:6379),客户端按需创建");
    }
    DEFAULT_LOG_INFO("ZmDbModule::Init 完成: db={} 全新建表+种子", DbPath());
    return true;
}

void ZmDbModule::StartPeriodicCleanup()
{
    // 事件循环启动后才可注册定时器
    drogon::app().registerBeginningAdvice([this]() {
        trantor::EventLoop* loop = drogon::app().getLoop();
        if (!loop)
            return;
        // 每小时检查一次,命中每日 03:00 窗口执行清理
        loop->runEvery(3600.0, [this]() {
            std::time_t now = std::time(nullptr);
            std::tm t{};
#ifdef _WIN32
            localtime_s(&t, &now);
#else
            localtime_r(&now, &t);
#endif
            if (t.tm_hour != 3)
                return;
            ZmHttpServer::WorkPool().Submit([this]() { CleanupOnce(); });
        });
        DEFAULT_LOG_INFO("ZmDbModule: 周期清理任务已注册(每日 03:00)");
    });
}

// ============================================================================
// 建表 + 种子(全新建表;不做旧数据迁移)
// ============================================================================
bool ZmDbModule::BuildSchema()
{
    // 与《2026-09-05-用户数据库表结构设计.md》§3 一一对应(13 表 + 索引)
    static const char* kTables[] = {
        // 3.1 users
        R"(CREATE TABLE IF NOT EXISTS users(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NOT NULL UNIQUE,
            account VARCHAR(30) NOT NULL UNIQUE,
            nickname VARCHAR(20) NOT NULL,
            email VARCHAR(255) NULL UNIQUE,
            phone VARCHAR(20) NULL UNIQUE,
            pass_salt BLOB NOT NULL,
            pass_hash BLOB NOT NULL,
            status TINYINT NOT NULL DEFAULT 1,
            deleted TINYINT NOT NULL DEFAULT 0,
            register_ip VARCHAR(64) NOT NULL,
            register_time INTEGER NOT NULL,
            last_login_ip VARCHAR(64) NOT NULL DEFAULT '',
            last_login_time INTEGER NOT NULL DEFAULT 0,
            force_change TINYINT NOT NULL DEFAULT 0,
            temp_pass_salt BLOB NULL,
            temp_pass_hash BLOB NULL,
            role_code VARCHAR(32) NOT NULL DEFAULT 'user',
            create_time INTEGER NOT NULL
        );)",
        // 3.2 user_profile
        R"(CREATE TABLE IF NOT EXISTS user_profile(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NOT NULL UNIQUE,
            avatar VARCHAR(255) NOT NULL DEFAULT '',
            signature VARCHAR(200) NOT NULL DEFAULT '',
            preferences TEXT NULL,
            updated_at INTEGER NOT NULL
        );)",
        // 3.3 sessions
        R"(CREATE TABLE IF NOT EXISTS sessions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            token_hash VARCHAR(64) NOT NULL UNIQUE,
            uid BIGINT NOT NULL,
            create_ip VARCHAR(64) NOT NULL,
            ua VARCHAR(255) NOT NULL DEFAULT '',
            create_time INTEGER NOT NULL,
            last_active INTEGER NOT NULL,
            expire_time INTEGER NOT NULL,
            absolute_expire INTEGER NOT NULL
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_sessions_uid ON sessions(uid);)",
        // 3.4 password_reset_tokens
        R"(CREATE TABLE IF NOT EXISTS password_reset_tokens(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NOT NULL,
            token_hash VARCHAR(64) NOT NULL UNIQUE,
            create_ip VARCHAR(64) NOT NULL,
            create_time INTEGER NOT NULL,
            expire_time INTEGER NOT NULL,
            used_at INTEGER NULL,
            status TINYINT NOT NULL DEFAULT 0
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_reset_uid ON password_reset_tokens(uid);)",
        // 3.5 login_locks
        R"(CREATE TABLE IF NOT EXISTS login_locks(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NULL,
            login_key VARCHAR(64) NOT NULL,
            ip VARCHAR(64) NOT NULL,
            fail_count INTEGER NOT NULL DEFAULT 0,
            locked_until INTEGER NOT NULL DEFAULT 0,
            last_fail_at INTEGER NOT NULL DEFAULT 0
        );)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_locks_uid_ip ON login_locks(uid, ip);)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_locks_key_ip ON login_locks(login_key, ip);)",
        R"(CREATE INDEX IF NOT EXISTS idx_locks_until ON login_locks(locked_until);)",
        // 3.6 rate_limits(复合主键)
        R"(CREATE TABLE IF NOT EXISTS rate_limits(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scope VARCHAR(32) NOT NULL,
            dimension VARCHAR(16) NOT NULL,
            dim_key VARCHAR(128) NOT NULL,
            window_start INTEGER NOT NULL,
            count INTEGER NOT NULL DEFAULT 0,
            UNIQUE(scope, dimension, dim_key)
        );)",
        // 3.7 security_events
        R"(CREATE TABLE IF NOT EXISTS security_events(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NULL,
            account VARCHAR(30) NOT NULL DEFAULT '',
            event_type VARCHAR(32) NOT NULL,
            ip VARCHAR(64) NOT NULL DEFAULT '',
            ua VARCHAR(255) NOT NULL DEFAULT '',
            detail TEXT NULL,
            create_time INTEGER NOT NULL
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_sec_uid ON security_events(uid);)",
        R"(CREATE INDEX IF NOT EXISTS idx_sec_type ON security_events(event_type);)",
        R"(CREATE INDEX IF NOT EXISTS idx_sec_time ON security_events(create_time);)",
        // 3.8 roles
        R"(CREATE TABLE IF NOT EXISTS roles(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            code VARCHAR(32) NOT NULL UNIQUE,
            name VARCHAR(32) NOT NULL,
            level TINYINT NOT NULL,
            permission_codes TEXT NOT NULL DEFAULT '[]',
            sort TINYINT NOT NULL DEFAULT 0,
            builtin TINYINT NOT NULL DEFAULT 0,
            description VARCHAR(200) NOT NULL DEFAULT '',
            updated_at INTEGER NOT NULL
        );)",
        // 3.9 permissions
        R"(CREATE TABLE IF NOT EXISTS permissions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            code VARCHAR(64) NOT NULL UNIQUE,
            name VARCHAR(64) NOT NULL,
            module VARCHAR(64) NOT NULL DEFAULT '',
            url VARCHAR(128) NOT NULL DEFAULT '',
            type TINYINT NOT NULL DEFAULT 0,
            "index" INT NOT NULL DEFAULT 0,
            sort TINYINT NOT NULL DEFAULT 0,
            enabled TINYINT NOT NULL DEFAULT 1,
            description VARCHAR(200) NOT NULL DEFAULT ''
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_perm_module ON permissions(module);)",
        // 3.10 user_permissions
        R"(CREATE TABLE IF NOT EXISTS user_permissions(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NOT NULL,
            perm_code VARCHAR(64) NOT NULL,
            grant_type TINYINT NOT NULL DEFAULT 1,
            grant_by BIGINT NULL,
            create_time INTEGER NOT NULL,
            UNIQUE(uid, perm_code)
        );)",
        // 3.11 operation_logs
        R"(CREATE TABLE IF NOT EXISTS operation_logs(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            operator_uid BIGINT NOT NULL,
            operator_account VARCHAR(30) NOT NULL,
            action VARCHAR(32) NOT NULL,
            target_type VARCHAR(32) NOT NULL,
            target_id BIGINT NOT NULL,
            detail TEXT NOT NULL DEFAULT '',
            ip VARCHAR(64) NOT NULL DEFAULT '',
            create_time INTEGER NOT NULL
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_op_operator ON operation_logs(operator_uid);)",
        R"(CREATE INDEX IF NOT EXISTS idx_op_target ON operation_logs(target_type, target_id);)",
        R"(CREATE INDEX IF NOT EXISTS idx_op_time ON operation_logs(create_time);)",
        // 3.12 login_logs
        R"(CREATE TABLE IF NOT EXISTS login_logs(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            uid BIGINT NULL,
            account VARCHAR(30) NOT NULL,
            result TINYINT NOT NULL,
            fail_reason VARCHAR(64) NOT NULL DEFAULT '',
            ip VARCHAR(64) NOT NULL,
            ua VARCHAR(255) NOT NULL DEFAULT '',
            create_time INTEGER NOT NULL
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_login_account ON login_logs(account);)",
        R"(CREATE INDEX IF NOT EXISTS idx_login_time ON login_logs(create_time);)",
        // 10.2 verify_codes(本期建表预留)
        R"(CREATE TABLE IF NOT EXISTS verify_codes(
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            scope VARCHAR(32) NOT NULL,
            target VARCHAR(128) NOT NULL,
            code_hash VARCHAR(64) NOT NULL,
            create_time INTEGER NOT NULL,
            expire_time INTEGER NOT NULL,
            used TINYINT NOT NULL DEFAULT 0,
            attempts INTEGER NOT NULL DEFAULT 0
        );)",
        R"(CREATE INDEX IF NOT EXISTS idx_verify_target ON verify_codes(scope, target);)",
        R"(CREATE INDEX IF NOT EXISTS idx_verify_expire ON verify_codes(expire_time);)",
    };
    for (const char* sql : kTables)
    {
        if (!ExecSync(sql, {}))
            return false;
    }
    return true;
}

bool ZmDbModule::SeedData()
{
    int64_t now = Now();
    // 角色(§6:developer/admin/user)
    std::vector<std::string> roles = {
        "developer", "开发者", "3", R"(["home","systemManager","userManage"])", "1", "1",
        "系统最高权限(首个注册用户)",
        "admin", "管理员", "2", R"(["home","systemManager","userManage"])", "2", "1",
        "管理操作(由提权授予)",
        "user", "用户", "1", R"(["home"])", "3", "1",
        "普通用户",
    };
    for (size_t i = 0; i + 7 <= roles.size(); i += 7)
    {
        if (!ExecSync("INSERT OR IGNORE INTO roles(code,name,level,permission_codes,sort,builtin,description,updated_at) "
                      "VALUES(?,?,?,?,?,?,?,?)",
                      {roles[i], roles[i + 1], roles[i + 2], roles[i + 3], roles[i + 4],
                       roles[i + 5], roles[i + 6], std::to_string(now)}))
            return false;
    }
    // 权限点(§3.9):type 0=门户模块(进侧边栏) / 1=功能权限(index=0 不进门户清单)
    // 门户模块与系统管理子功能分层:systemManager=门户入口,userManage=用户管理 API 授权
    std::vector<std::string> perms = {
        "home", "用户主页", "portal", "/portal/home", "0", "1", "1", "1", "用户主页模块",
        "systemManager", "系统管理", "system", "/portal/system-manager", "0", "-1", "2", "1", "系统管理权限",
        "userManage", "用户管理", "system", "", "1", "0", "3", "1", "系统管理-用户管理功能权限",
    };
    for (size_t i = 0; i + 9 <= perms.size(); i += 9)
    {
        if (!ExecSync("INSERT OR IGNORE INTO permissions(code,name,module,url,type,\"index\",sort,enabled,description) "
                      "VALUES(?,?,?,?,?,?,?,?,?)",
                      {perms[i], perms[i + 1], perms[i + 2], perms[i + 3], perms[i + 4],
                       perms[i + 5], perms[i + 6], perms[i + 7], perms[i + 8]}))
            return false;
    }
    return true;
}

// ============================================================================
// 周期清理(每日 03:00;单表批次,异常隔离)
// ============================================================================
void ZmDbModule::CleanupOnce()
{
    if (!IsReady())
        return;
    int64_t now = Now();
    struct CleanRule
    {
        const char* sql;
    };
    const CleanRule rules[] = {
        {"DELETE FROM sessions WHERE expire_time < ?1 OR absolute_expire < ?1;"},
        {"DELETE FROM password_reset_tokens WHERE expire_time < ?1;"},
        {"DELETE FROM login_locks WHERE last_fail_at < ?1;"},              // 7 天无动静
        {"DELETE FROM rate_limits WHERE window_start < ?1;"},              // 7 天
        {"DELETE FROM security_events WHERE create_time < ?1;"},           // 90 天
        {"DELETE FROM operation_logs WHERE create_time < ?1;"},            // 90 天
        {"DELETE FROM login_logs WHERE create_time < ?1;"},                // 30 天
        {"DELETE FROM verify_codes WHERE expire_time < ?1;"},              // 过期即清
    };
    int64_t days7 = now - 7LL * 86400;
    int64_t days30 = now - 30LL * 86400;
    int64_t days90 = now - 90LL * 86400;
    std::vector<std::string> p = {std::to_string(now)};
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
    {
        std::string sql(rules[i].sql);
        // 简单按序替换阈值占位:rules 内统一用 ?1 的保留原样;带 ?2 的补第二个参数
        if (sql.find("last_fail_at") != std::string::npos)
            p = {std::to_string(days7)};
        else if (sql.find("window_start") != std::string::npos)
            p = {std::to_string(days7)};
        else if (sql.find("security_events") != std::string::npos)
            p = {std::to_string(days90)};
        else if (sql.find("operation_logs") != std::string::npos)
            p = {std::to_string(days90)};
        else if (sql.find("login_logs") != std::string::npos)
            p = {std::to_string(days30)};
        else
            p = {std::to_string(now)};
        if (!ExecSync(sql, p))
            DEFAULT_LOG_WARN("ZmDbModule::CleanupOnce: 清理失败: {}", sql);
    }
    DEFAULT_LOG_INFO("ZmDbModule: 周期清理完成");
}

// ============================================================================
// Redis(缓存/共享状态;不可用 → 降级)
//   启动期 Init 同步 TCP 探测可达性:不可达 → m_redisDown 置位,不创建
//   drogon RedisClient(避免 hiredis 连接失败打印 RedisConnection.cc ERROR 日志);
//   可达 → 客户端按需(首次命令)惰性创建。
// ============================================================================
bool ZmDbModule::ProbeRedisTcp(const std::string& ip, uint16_t port, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return false;
    // 非阻塞 connect + select 等待可写,带超时
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        closesocket(s);
        return false;
    }
    bool ok = false;
    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0)
    {
        ok = true;
    }
    else if (WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int sel = select(0, nullptr, &wf, nullptr, &tv);
        if (sel > 0)
        {
            int err = 0;
            int len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
            ok = (err == 0);
        }
    }
    closesocket(s);
    return ok;
}

std::shared_ptr<drogon::nosql::RedisClient> ZmDbModule::Redis()
{
    if (m_redis)
        return m_redis;
    std::lock_guard<std::mutex> lock(m_redisMtx);
    if (m_redis)
        return m_redis;
    if (m_redisDown.load() || m_redisTried.load())
        return nullptr;
    m_redisTried.store(true);
    try
    {
        trantor::InetAddress addr("127.0.0.1", 6379);
        m_redis = drogon::nosql::RedisClient::newRedisClient(addr, 2, "", 0);
        // Redis 运行期无响应时快速失败降级(默认无超时会卡住协程)
        m_redis->setTimeout(2.0);
        DEFAULT_LOG_INFO("ZmDbModule: Redis 客户端已创建(127.0.0.1:6379)");
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("ZmDbModule: Redis 客户端创建失败,降级为直查库/进程内值: {}",
                         e.what());
        m_redis.reset();
        m_redisDown.store(true);
    }
    return m_redis;
}

drogon::Task<bool> ZmDbModule::RSet(const std::string& key, const std::string& val,
                                    int ttlSec)
{
    auto client = Redis();
    if (m_redisDown.load())
        co_return false;
    if (!client)
        co_return false;
    try
    {
        if (ttlSec > 0)
        {
            auto r = co_await client->execCommandCoro("setex %s %d %s", key.c_str(),
                                                      ttlSec, val.c_str());
            (void)r;
        }
        else
        {
            auto r = co_await client->execCommandCoro("set %s %s", key.c_str(),
                                                      val.c_str());
            (void)r;
        }
        co_return true;
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("Redis set 失败 key={}: {}", key, e.what());
        co_return false;
    }
}

drogon::Task<std::string> ZmDbModule::RGet(const std::string& key)
{
    auto client = Redis();
    if (m_redisDown.load())
        co_return "";
    if (!client)
        co_return "";
    try
    {
        auto r = co_await client->execCommandCoro("get %s", key.c_str());
        if (r.type() == drogon::nosql::RedisResultType::kNil)
            co_return "";
        co_return r.asString();
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("Redis get 失败 key={}: {}", key, e.what());
        m_redisDown.store(true);
        co_return "";
    }
}

drogon::Task<bool> ZmDbModule::RDel(const std::string& key)
{
    auto client = Redis();
    if (m_redisDown.load())
        co_return false;
    if (!client)
        co_return false;
    try
    {
        auto r = co_await client->execCommandCoro("del %s", key.c_str());
        (void)r;
        co_return true;
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("Redis del 失败 key={}: {}", key, e.what());
        co_return false;
    }
}

drogon::Task<int64_t> ZmDbModule::RIncr(const std::string& key, int ttlSec)
{
    auto client = Redis();
    if (m_redisDown.load())
        co_return -1;
    if (!client)
        co_return -1;
    try
    {
        auto r = co_await client->execCommandCoro("incr %s", key.c_str());
        int64_t v = r.asInteger();
        if (ttlSec > 0 && v == 1)
        {
            auto e = co_await client->execCommandCoro("expire %s %d", key.c_str(),
                                                      ttlSec);
            (void)e;
        }
        co_return v;
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("Redis incr 失败 key={}: {}", key, e.what());
        co_return -1;
    }
}

drogon::Task<bool> ZmDbModule::RSetExpire(const std::string& key, int ttlSec)
{
    auto client = Redis();
    if (m_redisDown.load())
        co_return false;
    if (!client)
        co_return false;
    try
    {
        auto r = co_await client->execCommandCoro("expire %s %d", key.c_str(), ttlSec);
        (void)r;
        co_return true;
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_WARN("Redis expire 失败 key={}: {}", key, e.what());
        m_redisDown.store(true);
        co_return false;
    }
}

drogon::Task<int64_t> ZmDbModule::BumpPolicyVersion()
{
    // Redis 优先;不可用回退进程内原子计数器(项目约束)
    int64_t v = co_await RIncr(zm_redis_key::PolicyVersion(), 0);
    if (v < 0)
        v = m_localPolicyVersion.fetch_add(1) + 1;
    co_return v;
}

drogon::Task<int64_t> ZmDbModule::GetPolicyVersion()
{
    std::string s = co_await RGet(zm_redis_key::PolicyVersion());
    if (s.empty())
    {
        int64_t local = m_localPolicyVersion.load();
        co_return local;
    }
    try
    {
        co_return std::stoll(s);
    }
    catch (...)
    {
        co_return m_localPolicyVersion.load();
    }
}
