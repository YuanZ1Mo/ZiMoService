#include "modules/module_file_db.h"

#include "modules/module_file_defs.h"

#include <drogon/HttpAppFramework.h>

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <ctime>
#include <filesystem>

using namespace drogon;

// ============================================================================
// 构造 / 析构 / Init
// ============================================================================
ZmFileDbModule::ZmFileDbModule() = default;

ZmFileDbModule::~ZmFileDbModule() = default;

bool ZmFileDbModule::Init(const std::string& dbPath, const std::string& rootDir)
{
    m_rootDir = rootDir;
    if (m_rootDir.empty())
    {
        DEFAULT_LOG_ERROR("ZmFileDbModule::Init: rootDir 为空");
        return false;
    }
    if (!ZmSqliteDb::Init(dbPath))
    {
        DEFAULT_LOG_ERROR("ZmFileDbModule::Init: 打开库失败: {}", dbPath);
        return false;
    }
    ExecSync("PRAGMA journal_mode=WAL;", {});
    ExecSync("PRAGMA busy_timeout=5000;", {});
    if (!BuildSchema())
    {
        DEFAULT_LOG_ERROR("ZmFileDbModule::Init: 建表失败");
        Close();
        return false;
    }
    // 公共空间行 + 物理根目录(space\0 / space_cache\0)
    if (!SeedSpaceRoot())
    {
        Close();
        return false;
    }
    DEFAULT_LOG_INFO("ZmFileDbModule::Init 完成: db={} root={}", DbPath(), m_rootDir);
    return true;
}

void ZmFileDbModule::StartPeriodicCleanup()
{
    // 事件循环启动后才可注册定时器;每小时检查一次,命中每日 03:00 窗口执行一轮
    drogon::app().registerBeginningAdvice(
        [this]()
        {
            trantor::EventLoop* loop = drogon::app().getLoop();
            if (!loop)
                return;
            loop->runEvery(3600.0,
                           [this]()
                           {
                               std::time_t now = std::time(nullptr);
                               std::tm     t{};
#ifdef _WIN32
                               localtime_s(&t, &now);
#else
 localtime_r(&now, &t);
#endif
                               if (t.tm_hour != 3)
                                   return;
                               ZmHttpServer::WorkPool().Submit([this]() { CleanupOnce(); });
                           });
            DEFAULT_LOG_INFO("ZmFileDbModule: 周期清理任务已注册(每日 03:00)");
        });
}

bool ZmFileDbModule::EnsureSpaceDirs(int64_t space) const
{
    auto makeDir = [](const std::string& path) -> bool
    {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec)
        {
            DEFAULT_LOG_ERROR("ZmFileDbModule: 创建目录失败: {} ({})", path, ec.message());
            return false;
        }
        return true;
    };
    return makeDir(m_rootDir + "\\space\\" + std::to_string(space)) &&
           makeDir(m_rootDir + "\\space_cache\\" + std::to_string(space));
}

// ============================================================================
// 建表:9 张表 + 索引(含部分唯一索引)
// ============================================================================
bool ZmFileDbModule::BuildSchema()
{
    static const char* kTables[] = {
        // 5.2 spaces
        R"(CREATE TABLE IF NOT EXISTS spaces(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 space BIGINT NOT NULL UNIQUE,
 quota BIGINT NOT NULL DEFAULT 0,
 used_size BIGINT NOT NULL DEFAULT 0,
 used_items BIGINT NOT NULL DEFAULT 0,
 create_time INTEGER NOT NULL,
 update_time INTEGER NOT NULL
);)",
        // 5.3 nodes(目录与文件同表;deleted 只标记被删的顶层条目)
        R"(CREATE TABLE IF NOT EXISTS nodes(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 space BIGINT NOT NULL,
 parent_id BIGINT NOT NULL DEFAULT 0,
 type TINYINT NOT NULL,
 name VARCHAR(255) NOT NULL,
 size BIGINT NOT NULL DEFAULT 0,
 ext VARCHAR(32) NOT NULL DEFAULT '',
 hash VARCHAR(64) NOT NULL DEFAULT '',
 owner_uid BIGINT NOT NULL DEFAULT 0,
 items INTEGER NOT NULL DEFAULT 0,
 create_time INTEGER NOT NULL,
 update_time INTEGER NOT NULL,
 deleted TINYINT NOT NULL DEFAULT 0,
 delete_time INTEGER NOT NULL DEFAULT 0,
 origin_parent_id BIGINT NOT NULL DEFAULT 0,
 del_owner_uid BIGINT NOT NULL DEFAULT 0
);)",
        // 部分唯一索引:同名判定不区分大小写,且回收站条目不占用同级名称
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_nodes_uniq
 ON nodes(space, parent_id, name COLLATE NOCASE) WHERE deleted = 0;)",
        // 查询索引一律带 WHERE deleted=0(漏写该条件会退化为全表扫描)
        R"(CREATE INDEX IF NOT EXISTS idx_nodes_dir
 ON nodes(space, parent_id, type, name) WHERE deleted = 0;)",
        R"(CREATE INDEX IF NOT EXISTS idx_nodes_search
 ON nodes(space, name) WHERE deleted = 0;)",
        R"(CREATE INDEX IF NOT EXISTS idx_nodes_hash
 ON nodes(space, hash, size) WHERE deleted = 0;)",
        R"(CREATE INDEX IF NOT EXISTS idx_nodes_trash
 ON nodes(space, deleted, delete_time);)",
        // 5.4 upload_sessions
        R"(CREATE TABLE IF NOT EXISTS upload_sessions(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 upload_id VARCHAR(40) NOT NULL UNIQUE,
 uid BIGINT NOT NULL,
 space BIGINT NOT NULL,
 parent_id BIGINT NOT NULL,
 name VARCHAR(255) NOT NULL,
 size BIGINT NOT NULL,
 file_hash VARCHAR(64) NOT NULL DEFAULT '',
 chunk_size BIGINT NOT NULL,
 chunk_total INTEGER NOT NULL,
 chunk_done INTEGER NOT NULL DEFAULT 0,
 conflict VARCHAR(16) NOT NULL DEFAULT 'ask',
 status TINYINT NOT NULL DEFAULT 1,
 create_time INTEGER NOT NULL,
 expire_time INTEGER NOT NULL
);)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_upload_uniq
 ON upload_sessions(uid, space, parent_id, name COLLATE NOCASE) WHERE status=1;)",
        R"(CREATE INDEX IF NOT EXISTS idx_upload_expire ON upload_sessions(expire_time);)",
        // 5.5 upload_chunks
        R"(CREATE TABLE IF NOT EXISTS upload_chunks(
 upload_id VARCHAR(40) NOT NULL,
 chunk_index INTEGER NOT NULL,
 size INTEGER NOT NULL,
 done_time INTEGER NOT NULL
);)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_chunk_uniq
 ON upload_chunks(upload_id, chunk_index);)",
        // 5.6 transfer_tasks
        R"(CREATE TABLE IF NOT EXISTS transfer_tasks(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 task_no VARCHAR(40) NOT NULL UNIQUE,
 type TINYINT NOT NULL,
 uid BIGINT NOT NULL,
 space BIGINT NOT NULL,
 name VARCHAR(255) NOT NULL,
 target VARCHAR(512) NOT NULL DEFAULT '',
 size BIGINT NOT NULL DEFAULT 0,
 done_size BIGINT NOT NULL DEFAULT 0,
 total_items INTEGER NOT NULL DEFAULT 0,
 done_items INTEGER NOT NULL DEFAULT 0,
 status TINYINT NOT NULL DEFAULT 1,
 error VARCHAR(255) NOT NULL DEFAULT '',
 result VARCHAR(512) NOT NULL DEFAULT '',
 ref_id VARCHAR(40) NOT NULL DEFAULT '',
 create_time INTEGER NOT NULL,
 start_time INTEGER NOT NULL DEFAULT 0,
 end_time INTEGER NOT NULL DEFAULT 0
);)",
        R"(CREATE INDEX IF NOT EXISTS idx_task_uid ON transfer_tasks(uid, status, id DESC);)",
        R"(CREATE INDEX IF NOT EXISTS idx_task_stat ON transfer_tasks(status, create_time);)",
        R"(CREATE INDEX IF NOT EXISTS idx_task_ref ON transfer_tasks(ref_id);)",
        // 5.7 shares
        R"(CREATE TABLE IF NOT EXISTS shares(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 token VARCHAR(32) NOT NULL UNIQUE,
 uid BIGINT NOT NULL,
 space BIGINT NOT NULL,
 node_id BIGINT NOT NULL,
 node_type TINYINT NOT NULL,
 name VARCHAR(255) NOT NULL,
 pwd_hash VARCHAR(64) NOT NULL DEFAULT '',
 login_only TINYINT NOT NULL DEFAULT 0,
 expire_time INTEGER NOT NULL DEFAULT 0,
 max_downloads INTEGER NOT NULL DEFAULT 0,
 download_count INTEGER NOT NULL DEFAULT 0,
 view_count INTEGER NOT NULL DEFAULT 0,
 status TINYINT NOT NULL DEFAULT 1,
 create_time INTEGER NOT NULL,
 update_time INTEGER NOT NULL
);)",
        R"(CREATE INDEX IF NOT EXISTS idx_share_uid ON shares(uid, status);)",
        R"(CREATE INDEX IF NOT EXISTS idx_share_node ON shares(node_id, status);)",
        R"(CREATE INDEX IF NOT EXISTS idx_share_expire ON shares(status, expire_time);)",
        // 5.7.1 share_nodes(分享绑定的条目;单条分享也写一行,读路径统一为多根)
        R"(CREATE TABLE IF NOT EXISTS share_nodes(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 share_id BIGINT NOT NULL,
 node_id BIGINT NOT NULL,
 node_type TINYINT NOT NULL,
 name VARCHAR(255) NOT NULL DEFAULT '',
 sort INTEGER NOT NULL DEFAULT 0,
 create_time INTEGER NOT NULL
);)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_sharenode_uniq ON share_nodes(share_id, node_id);)",
        R"(CREATE INDEX IF NOT EXISTS idx_sharenode_share ON share_nodes(share_id, sort);)",
        // 5.8 share_logs
        R"(CREATE TABLE IF NOT EXISTS share_logs(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 share_id BIGINT NOT NULL,
 token VARCHAR(32) NOT NULL,
 action TINYINT NOT NULL,
 result TINYINT NOT NULL,
 uid BIGINT NOT NULL DEFAULT 0,
 node_id BIGINT NOT NULL DEFAULT 0,
 ip VARCHAR(64) NOT NULL DEFAULT '',
 ua VARCHAR(255) NOT NULL DEFAULT '',
 detail VARCHAR(512) NOT NULL DEFAULT '',
 create_time INTEGER NOT NULL
);)",
        R"(CREATE INDEX IF NOT EXISTS idx_sharelog ON share_logs(share_id, create_time);)",
        R"(CREATE INDEX IF NOT EXISTS idx_sharelog_time ON share_logs(create_time);)",
        // 5.9 file_logs
        R"(CREATE TABLE IF NOT EXISTS file_logs(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 uid BIGINT NOT NULL,
 account VARCHAR(30) NOT NULL DEFAULT '',
 action VARCHAR(32) NOT NULL,
 space BIGINT NOT NULL,
 node_id BIGINT NOT NULL DEFAULT 0,
 node_name VARCHAR(255) NOT NULL DEFAULT '',
 detail TEXT NULL,
 ip VARCHAR(64) NOT NULL DEFAULT '',
 result TINYINT NOT NULL DEFAULT 1,
 create_time INTEGER NOT NULL
);)",
        R"(CREATE INDEX IF NOT EXISTS idx_flog_uid ON file_logs(uid, create_time);)",
        R"(CREATE INDEX IF NOT EXISTS idx_flog_action ON file_logs(action, create_time);)",
        R"(CREATE INDEX IF NOT EXISTS idx_flog_time ON file_logs(create_time);)",
        // 5.10 write_limits(通用计数表)
        R"(CREATE TABLE IF NOT EXISTS write_limits(
 scope VARCHAR(32) NOT NULL,
 dim_key VARCHAR(160) NOT NULL,
 window_start INTEGER NOT NULL,
 count INTEGER NOT NULL DEFAULT 0
);)",
        R"(CREATE UNIQUE INDEX IF NOT EXISTS idx_wlimit ON write_limits(scope, dim_key);)",
    };
    for (const char* sql : kTables)
    {
        if (!ExecSync(sql, {}))
            return false;
    }
    return true;
}

bool ZmFileDbModule::SeedSpaceRoot()
{
    int64_t now = Now();
    if (!ExecSync("INSERT OR IGNORE INTO "
                  "spaces(space,quota,used_size,used_items,create_time,update_time) "
                  "VALUES(0,0,0,0,?,?)",
                  {std::to_string(now), std::to_string(now)}))
        return false;
    if (!EnsureSpaceDirs(0))
        return false;
    // 历史用户空间目录补齐:spaces 已有行但目录缺失时补建(幂等)
    ZMJSON rows = QueryRowsSync("SELECT space FROM spaces", {});
    for (const auto& r : rows)
    {
        int64_t space = zm_file_row_int(r, "space", 0);
        if (space != 0 && !EnsureSpaceDirs(space))
            return false;
    }
    return true;
}

// ============================================================================
// 路径装配
// ============================================================================
bool ZmFileDbModule::PathPartsSync(int64_t nodeId, int64_t& space, std::string& relPath)
{
    relPath.clear();
    if (nodeId == 0)
        return true;
    // 自底向上递归取父链,再按深度倒序拼根 → 节点;深度上限 64 防止脏数据成环
    ZMJSON rows =
        QueryRowsSync("WITH RECURSIVE up(id, parent_id, name, depth) AS ("
                      " SELECT id, parent_id, name, 0 FROM nodes WHERE id = ?1"
                      " UNION ALL"
                      " SELECT n.id, n.parent_id, n.name, up.depth + 1 FROM nodes n"
                      " JOIN up ON n.id = up.parent_id WHERE up.depth < 64)"
                      " SELECT id, parent_id, name, depth FROM up ORDER BY depth DESC;",
                      {std::to_string(nodeId)});
    if (rows.empty())
        return false;
    for (const auto& r : rows)
    {
        std::string name = zm_file_row_str(r, "name");
        if (!relPath.empty())
            relPath += "\\";
        relPath += name;
    }
    ZMJSON row =
        QueryRowSync("SELECT space FROM nodes WHERE id = ?1", {std::to_string(nodeId)});
    space = zm_file_row_int(row, "space", 0);
    return true;
}

ZMJSON ZmFileDbModule::NodeRowSync(int64_t nodeId)
{
    return QueryRowSync("SELECT * FROM nodes WHERE id = ?1", {std::to_string(nodeId)});
}

int64_t ZmFileDbModule::NodeSpaceSync(int64_t nodeId)
{
    ZMJSON row =
        QueryRowSync("SELECT space FROM nodes WHERE id = ?1", {std::to_string(nodeId)});
    if (row.empty())
        return -1;
    return zm_file_row_int(row, "space", 0);
}

// ============================================================================
// 空间
// ============================================================================
drogon::Task<ZMJSON> ZmFileDbModule::ListSpaces()
{
    co_return co_await QueryRows("SELECT * FROM spaces ORDER BY space ASC", {});
}

drogon::Task<ZMJSON> ZmFileDbModule::GetSpace(int64_t space)
{
    co_return co_await QueryRow("SELECT * FROM spaces WHERE space = ?1",
                                {std::to_string(space)});
}

bool ZmFileDbModule::EnsureSpaceSync(int64_t space)
{
    int64_t now = Now();
    if (!ExecSync("INSERT OR IGNORE INTO spaces(space,quota,used_size,used_items,create_time,"
                  "update_time) VALUES(?1,0,0,0,?2,?2)",
                  {std::to_string(space), std::to_string(now)}))
        return false;
    return EnsureSpaceDirs(space);
}

ZMJSON ZmFileDbModule::SpaceRowSync(int64_t space)
{
    return QueryRowSync("SELECT * FROM spaces WHERE space = ?1", {std::to_string(space)});
}

drogon::Task<bool> ZmFileDbModule::EnsureSpace(int64_t space)
{
    co_return co_await ZmHttpServer::RunOnPool<bool>(
        [this, space]() -> bool
        {
            int64_t now = Now();
            if (!ExecSync("INSERT OR IGNORE INTO "
                          "spaces(space,quota,used_size,used_items,create_time,update_time) "
                          "VALUES(?1,0,0,0,?2,?2)",
                          {std::to_string(space), std::to_string(now)}))
                return false;
            return EnsureSpaceDirs(space);
        });
}

drogon::Task<bool> ZmFileDbModule::SetQuota(int64_t space, int64_t quota)
{
    co_return co_await ZmHttpServer::RunOnPool<bool>(
        [this, space, quota]() -> bool
        {
            // 空间行可能尚未懒创建:先补行,否则 UPDATE 静默无效
            if (!EnsureSpaceSync(space))
                return false;
            return ExecSync("UPDATE spaces SET quota = ?1, update_time = ?2 WHERE space = ?3",
                            {std::to_string(quota), std::to_string(Now()),
                             std::to_string(space)});
        });
}

// ============================================================================
// 用量记账
// ============================================================================
bool ZmFileDbModule::ApplyUsageSync(ZmSqliteDb& db, int64_t space, int64_t dSize,
                                    int64_t dItems)
{
    return db.ExecSync("UPDATE spaces SET used_size = used_size + ?1, "
                       "used_items = used_items + ?2, update_time = ?3 WHERE space = ?4",
                       {std::to_string(dSize), std::to_string(dItems),
                        std::to_string(ZmSqliteDb::Now()), std::to_string(space)});
}

bool ZmFileDbModule::InvalidateSharesByNodesSync(ZmSqliteDb& db,
                                                 const std::vector<int64_t>& nodeIds)
{
    if (nodeIds.empty())
        return true;
    // IN 列表按整数拼接:值来自库内 id,不存在注入面
    std::string in;
    for (size_t i = 0; i < nodeIds.size(); ++i)
    {
        if (i)
            in += ",";
        in += std::to_string(nodeIds[i]);
    }
    return db.ExecSync(
        "UPDATE shares SET status = 3, update_time = ?1 WHERE status = 1 AND CASE"
        // 多选分享:所有绑定条目都即将消失(在本次删除集合里,或此前已被彻底删除)才置失效;
        // 只删到一部分时分享保持有效(该条在分享页跳过、其余照常)
        " WHEN EXISTS (SELECT 1 FROM share_nodes sn WHERE sn.share_id = shares.id)"
        " THEN NOT EXISTS (SELECT 1 FROM share_nodes sn JOIN nodes n ON n.id = sn.node_id"
        "                  WHERE sn.share_id = shares.id AND sn.node_id NOT IN (" + in + "))"
        // 旧行(改造前创建,无关联行):按主条目判
        " ELSE node_id IN (" + in + ") END",
        {std::to_string(ZmSqliteDb::Now())});
}

bool ZmFileDbModule::RecountUsageSync(ZmSqliteDb& db, int64_t space)
{
    // 计入 deleted=1 的占用(回收站条目仍占配额)
    ZMJSON row = db.QueryRowTxSync(
        "SELECT COUNT(*) AS items, COALESCE(SUM(size),0) AS bytes FROM nodes WHERE space = ?1",
        {std::to_string(space)});
    int64_t items = zm_file_row_int(row, "items", 0);
    int64_t bytes = zm_file_row_int(row, "bytes", 0);
    return db.ExecSync("UPDATE spaces SET used_size = ?1, used_items = ?2, update_time = ?3 "
                       "WHERE space = ?4",
                       {std::to_string(bytes), std::to_string(items),
                        std::to_string(ZmSqliteDb::Now()), std::to_string(space)});
}

void ZmFileDbModule::TrashUsageSync(ZmSqliteDb& db, int64_t space, int64_t uid, int64_t& bytes,
                                    int64_t& items, bool tx)
{
    const char* sql = "SELECT COUNT(*) AS items, COALESCE(SUM(size),0) AS bytes FROM nodes "
                      "WHERE space = ?1 AND deleted = 1 AND del_owner_uid = ?2";
    std::vector<std::string> p   = {std::to_string(space), std::to_string(uid)};
    ZMJSON                   row = tx ? db.QueryRowTxSync(sql, p) : db.QueryRowSync(sql, p);
    items                        = zm_file_row_int(row, "items", 0);
    bytes                        = zm_file_row_int(row, "bytes", 0);
}

// ============================================================================
// 通用计数表(提取码失败冷却)
// ============================================================================
int64_t ZmFileDbModule::LimitCountSync(ZmSqliteDb& db, const std::string& scope,
                                       const std::string& dimKey, bool tx)
{
    const char* sql =
        "SELECT count, window_start FROM write_limits WHERE scope = ?1 AND dim_key = ?2";
    std::vector<std::string> p   = {scope, dimKey};
    ZMJSON                   row = tx ? db.QueryRowTxSync(sql, p) : db.QueryRowSync(sql, p);
    if (row.empty())
        return 0;
    return zm_file_row_int(row, "count", 0);
}

int64_t ZmFileDbModule::LimitWindowStartSync(ZmSqliteDb& db, const std::string& scope,
                                             const std::string& dimKey)
{
    ZMJSON row = db.QueryRowSync(
        "SELECT window_start FROM write_limits WHERE scope = ?1 AND dim_key = ?2",
        {scope, dimKey});
    if (row.empty())
        return 0;
    return zm_file_row_int(row, "window_start", 0);
}

bool ZmFileDbModule::BumpLimitSync(ZmSqliteDb& db, const std::string& scope,
                                   const std::string& dimKey, int64_t windowSec, int64_t now)
{
    // 窗口已过期(或无记录)→ 重置为 1;否则自增
    ZMJSON row = db.QueryRowTxSync(
        "SELECT count, window_start FROM write_limits WHERE scope = ?1 AND dim_key = ?2",
        {scope, dimKey});
    if (row.empty())
    {
        return db.ExecSync("INSERT INTO write_limits(scope,dim_key,window_start,count) "
                           "VALUES(?1,?2,?3,1)",
                           {scope, dimKey, std::to_string(now)});
    }
    int64_t start = zm_file_row_int(row, "window_start", 0);
    if (now - start >= windowSec)
    {
        return db.ExecSync("UPDATE write_limits SET window_start = ?1, count = 1 "
                           "WHERE scope = ?2 AND dim_key = ?3",
                           {std::to_string(now), scope, dimKey});
    }
    return db.ExecSync("UPDATE write_limits SET count = count + 1 "
                       "WHERE scope = ?1 AND dim_key = ?2",
                       {scope, dimKey});
}

bool ZmFileDbModule::ClearLimitSync(ZmSqliteDb& db, const std::string& scope,
                                    const std::string& dimKey)
{
    return db.ExecSync("DELETE FROM write_limits WHERE scope = ?1 AND dim_key = ?2",
                       {scope, dimKey});
}

drogon::Task<int64_t> ZmFileDbModule::DbFileSize()
{
    co_return co_await ZmHttpServer::RunOnPool<int64_t>(
        [this]() -> int64_t
        {
            std::error_code ec;
            auto            sz = std::filesystem::file_size(DbPath(), ec);
            return ec ? 0 : static_cast<int64_t>(sz);
        });
}

// ============================================================================
// 周期清理(每日 03:00;单表批次、异常隔离)
// ============================================================================
void ZmFileDbModule::CleanupOnce()
{
    if (!IsReady())
        return;
    int64_t       now    = Now();
    const int64_t days7  = now - 7LL * 86400;
    const int64_t days30 = now - 30LL * 86400;
    const int64_t days90 = now - 90LL * 86400;

    struct CleanRule
    {
        const char* sql;
        int64_t     threshold;
    };
    const CleanRule rules[] = {
        // 任务:终态超过 7 天删除
        {"DELETE FROM transfer_tasks WHERE status >= 3 AND create_time < ?1;", days7},
        // 分享:过期或达次数上限的有效分享置失效
        {"UPDATE shares SET status = 3, update_time = ?2 WHERE status = 1 AND "
         "((expire_time > 0 AND expire_time < ?1) OR (max_downloads > 0 AND "
         "download_count >= max_downloads));",
         now},
        // 分享:已取消(status=2)与已失效(status=3)超过 30 天的行删除(其访问日志一并删除)。
        // 已取消的行过去不会被清理,取消多了会一直堆在"我的分享"里
        {"DELETE FROM share_logs WHERE share_id IN (SELECT id FROM shares WHERE status IN (2,3) "
         "AND update_time < ?1);",
         days30},
        // 关联条目随分享行一起清掉(无外键;须在删 shares 之前,子查询要读得到行)
        {"DELETE FROM share_nodes WHERE share_id IN (SELECT id FROM shares WHERE status IN (2,3) "
         "AND update_time < ?1);",
         days30},
        {"DELETE FROM shares WHERE status IN (2,3) AND update_time < ?1;", days30},
        // 分享访问日志保留 90 天
        {"DELETE FROM share_logs WHERE create_time < ?1;", days90},
        // 业务审计日志保留 90 天
        {"DELETE FROM file_logs WHERE create_time < ?1;", days90},
        // 计数表:窗口超过 1 天且已结束的行删除
        {"DELETE FROM write_limits WHERE window_start < ?1;", days7},
    };
    for (const auto& r : rules)
    {
        if (!ExecSync(r.sql, {std::to_string(r.threshold), std::to_string(now)}))
            DEFAULT_LOG_WARN("ZmFileDbModule::CleanupOnce: 清理失败: {}", r.sql);
    }

    // 物理侧动作(由装配方注入;失败只记日志,不中断整轮)
    auto runHook = [](const std::function<void(int64_t)>& fn, const char* what, int64_t now)
    {
        if (!fn)
            return;
        try
        {
            fn(now);
        }
        catch (const std::exception& e)
        {
            DEFAULT_LOG_WARN("ZmFileDbModule::CleanupOnce: {} 异常: {}", what, e.what());
        }
        catch (...)
        {
            DEFAULT_LOG_WARN("ZmFileDbModule::CleanupOnce: {} 未知异常", what);
        }
    };
    runHook(m_hooks.purgeUploadChunks, "上传会话清理", now);
    runHook(m_hooks.purgeExpiredTrash, "回收站保留期清理", now);
    runHook(m_hooks.cleanCache, "打包缓存清理", now);
    runHook(m_hooks.verifyConsistency, "每日一致性校验", now);
    DEFAULT_LOG_INFO("ZmFileDbModule: 周期清理完成");
}
