#include "module_db_init.h"

#include "zm_logger.h"
#include "zm_util_sys.h"

#include <direct.h>

#include <algorithm>
#include <set>
#include <string>

namespace
{
// ============================================================================
// Schema 声明(列清单即唯一事实源;新增列 = 在此追加一行)
// 约束(UNIQUE/PRIMARY KEY)只存在于建表语句:SQLite 的 ALTER TABLE ADD COLUMN
// 不允许 UNIQUE/PRIMARY KEY/无默认值 NOT NULL,补列仅限 AppendSafe 定义。
// ============================================================================

// ── user.db ──────────────────────────────────────────────────────────────

static const ZmDbColumn kUsersCols[] = {
    {"id",             "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"account",        "TEXT NOT NULL UNIQUE"},
    {"nickname",       "TEXT NOT NULL"},
    {"pass_salt",      "BLOB NOT NULL"},
    {"pass_hash",      "BLOB NOT NULL"},
    {"rescue_salt",    "BLOB NOT NULL"},
    {"rescue_hash",    "BLOB NOT NULL"},
    {"register_ip",    "TEXT NOT NULL"},
    {"create_time",    "INTEGER NOT NULL"},
    {"last_login_ip",  "TEXT NOT NULL DEFAULT ''"},
    {"last_login_time","INTEGER NOT NULL DEFAULT 0"},
    {"role",           "TEXT NOT NULL DEFAULT 'user'"},
    {"disabled",       "INTEGER NOT NULL DEFAULT 0"},
    {"deleted",        "INTEGER NOT NULL DEFAULT 0"},
    {"temp_pass_salt", "BLOB"},
    {"temp_pass_hash", "BLOB"},
    {"force_change",   "INTEGER NOT NULL DEFAULT 0"},
};

static const ZmDbColumn kSessionsCols[] = {
    {"id",              "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"token_hash",      "TEXT NOT NULL UNIQUE"},
    {"user_id",         "INTEGER NOT NULL"},
    {"create_ip",       "TEXT NOT NULL"},
    {"last_active_ip",  "TEXT NOT NULL"},
    {"create_time",     "INTEGER NOT NULL"},
    {"last_active",     "INTEGER NOT NULL"},
    {"expire_time",     "INTEGER NOT NULL"},
    {"absolute_expire", "INTEGER NOT NULL"},
};

static const ZmDbColumn kLoginLocksCols[] = {
    {"id",           "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"account",      "TEXT NOT NULL"},
    {"ip",           "TEXT NOT NULL"},
    {"fail_count",   "INTEGER NOT NULL DEFAULT 0"},
    {"locked_until", "INTEGER NOT NULL DEFAULT 0"},
    {"last_fail_at", "INTEGER NOT NULL DEFAULT 0"},
};

static const ZmDbColumn kModulesCols[] = {
    {"id",      "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"code",    "TEXT NOT NULL UNIQUE"},
    {"name",    "TEXT NOT NULL"},
    {"url",     "TEXT NOT NULL"},
    {"sort",    "INTEGER NOT NULL DEFAULT 0"},
    {"enabled", "INTEGER NOT NULL DEFAULT 1"},
};
/** 模块种子数据(幂等):主页/文件中心/服务器音频传输/用户管理 */
static const char* kModulesPost =
    "INSERT OR IGNORE INTO modules(code,name,url,sort) VALUES"
    " ('home','主页','/portal',0),"
    " ('filehub','文件中心','/portal/filehub',1),"
    " ('audio','服务器音频传输','/portal/audio',2),"
    " ('users','用户管理','/portal/users',3)";

static const ZmDbColumn kUserModulesCols[] = {
    {"user_id",     "INTEGER NOT NULL"},
    {"module_code", "TEXT NOT NULL"},
};

static const ZmDbTable kUserTables[] = {
    {"users",        kUsersCols,        (int)std::size(kUsersCols),        "", ""},
    {"sessions",     kSessionsCols,     (int)std::size(kSessionsCols),     "", ""},
    {"login_locks",  kLoginLocksCols,   (int)std::size(kLoginLocksCols),   "UNIQUE(account, ip)", ""},
    {"modules",      kModulesCols,      (int)std::size(kModulesCols),      "", kModulesPost},
    {"user_modules", kUserModulesCols,  (int)std::size(kUserModulesCols),  "PRIMARY KEY(user_id, module_code)", ""},
};

static const char* kUserIndexes[] = {
    "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id)",
    "CREATE INDEX IF NOT EXISTS idx_sessions_expire ON sessions(expire_time)",
};

// ── rate.db ──────────────────────────────────────────────────────────────

static const ZmDbColumn kRateLimitCols[] = {
    {"ip",           "TEXT PRIMARY KEY"},
    {"window_start", "INTEGER NOT NULL"},
    {"count",        "INTEGER NOT NULL DEFAULT 0"},
};

static const ZmDbTable kRateTables[] = {
    {"register_rate_limits", kRateLimitCols, (int)std::size(kRateLimitCols), "", ""},
    {"reset_rate_limits",    kRateLimitCols, (int)std::size(kRateLimitCols), "", ""},
};

// ── audit.db(业务日志库,表按业务命名,同模式追加) ─────────────────────────

static const ZmDbColumn kUserManageLogsCols[] = {
    {"id",               "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"operator_id",      "INTEGER NOT NULL"},
    {"operator_account", "TEXT NOT NULL"},
    {"action",           "TEXT NOT NULL"},
    {"target_id",        "INTEGER NOT NULL"},
    {"target_account",   "TEXT NOT NULL"},
    {"detail",           "TEXT NOT NULL DEFAULT ''"},
    {"create_time",      "INTEGER NOT NULL"},
};

static const ZmDbColumn kFileHubLogsCols[] = {
    {"id",               "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"operator_id",      "INTEGER NOT NULL"},
    {"operator_account", "TEXT NOT NULL"},
    {"action",           "TEXT NOT NULL"},
    {"target_type",      "TEXT NOT NULL"},
    {"target_id",        "INTEGER NOT NULL"},
    {"detail",           "TEXT NOT NULL DEFAULT ''"},
    {"create_time",      "INTEGER NOT NULL"},
};

static const ZmDbTable kAuditTables[] = {
    {"user_manage_logs", kUserManageLogsCols, (int)std::size(kUserManageLogsCols), "", ""},
    {"filehub_logs",     kFileHubLogsCols,    (int)std::size(kFileHubLogsCols),    "", ""},
};

static const char* kAuditIndexes[] = {
    "CREATE INDEX IF NOT EXISTS idx_user_manage_logs_create ON user_manage_logs(create_time)",
    "CREATE INDEX IF NOT EXISTS idx_filehub_logs_create ON filehub_logs(create_time)",
};

// ── filehub.db ───────────────────────────────────────────────────────────

static const ZmDbColumn kDirsCols[] = {
    {"id",          "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"space",       "INTEGER NOT NULL"},
    {"parent_id",   "INTEGER NOT NULL DEFAULT 0"},
    {"name",        "TEXT NOT NULL"},
    {"create_by",   "INTEGER NOT NULL"},
    {"create_time", "INTEGER NOT NULL"},
};
/** 公共空间根行种子(幂等) */
static const char* kDirsPost =
    "INSERT OR IGNORE INTO dirs(space,parent_id,name,create_by,create_time) VALUES(0,0,'',0,0)";

static const ZmDbColumn kFilesCols[] = {
    {"id",          "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"space",       "INTEGER NOT NULL"},
    {"dir_id",      "INTEGER NOT NULL"},
    {"name",        "TEXT NOT NULL"},
    {"size",        "INTEGER NOT NULL"},
    {"mtime",       "INTEGER NOT NULL"},
    {"uploader_id", "INTEGER NOT NULL"},
    {"upload_time", "INTEGER NOT NULL"},
};

static const ZmDbColumn kSharesCols[] = {
    {"id",          "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"token_hash",  "TEXT NOT NULL UNIQUE"},
    {"token",       "TEXT NOT NULL"},
    {"owner_id",    "INTEGER NOT NULL"},
    {"target_type", "TEXT NOT NULL"},
    {"target_id",   "INTEGER NOT NULL"},
    {"create_time", "INTEGER NOT NULL"},
};

static const ZmDbColumn kDownloadLogsCols[] = {
    {"id",                "INTEGER PRIMARY KEY AUTOINCREMENT"},
    {"share_id",          "INTEGER NOT NULL"},
    {"target_type",       "TEXT NOT NULL"},
    {"target_id",         "INTEGER NOT NULL"},
    {"downloader_id",     "INTEGER NOT NULL"},
    {"downloader_account","TEXT NOT NULL"},
    {"ip",                "TEXT NOT NULL"},
    {"create_time",       "INTEGER NOT NULL"},
};

static const ZmDbTable kFileHubTables[] = {
    {"dirs",               kDirsCols,          (int)std::size(kDirsCols),          "UNIQUE(space, parent_id, name)", kDirsPost},
    {"files",              kFilesCols,         (int)std::size(kFilesCols),         "UNIQUE(space, dir_id, name)", ""},
    {"shares",             kSharesCols,        (int)std::size(kSharesCols),        "", ""},
    {"share_download_logs",kDownloadLogsCols,  (int)std::size(kDownloadLogsCols),  "", ""},
};

static const char* kFileHubIndexes[] = {
    "CREATE INDEX IF NOT EXISTS idx_files_space_dir ON files(space, dir_id)",
    "CREATE INDEX IF NOT EXISTS idx_dirs_space_parent ON dirs(space, parent_id)",
};

} // namespace

DbInitModule::DbInitModule() = default;

DbInitModule::~DbInitModule()
{
    Shutdown();
}

bool DbInitModule::Open()
{
    char exePath[MAX_PATH];
    ZmSystem::GetModuleDir(exePath, MAX_PATH);
    const std::string base = std::string(exePath) + "\\db";

    // 目录不存在则创建(EEXIST 忽略)
    _mkdir(base.c_str());
    _mkdir((base + "\\user").c_str());
    _mkdir((base + "\\rate").c_str());
    _mkdir((base + "\\audit").c_str());
    _mkdir((base + "\\filehub").c_str());

    // 单库失败独立处理:该库不可用记日志,其余库继续(业务模块经 IsOpen 自判)
    bool anyOk = false;

    if (m_userDb.Open(base + "\\user\\user.db", "User"))
    {
        if (EnsureDb(m_userDb, "User", kUserTables, (int)std::size(kUserTables),
                     kUserIndexes, (int)std::size(kUserIndexes)))
            anyOk = true;
        else
            m_userDb.Close();
    }

    if (m_rateDb.Open(base + "\\rate\\rate.db", "Rate"))
    {
        if (EnsureDb(m_rateDb, "Rate", kRateTables, (int)std::size(kRateTables),
                     nullptr, 0))
            anyOk = true;
        else
            m_rateDb.Close();
    }

    if (m_auditDb.Open(base + "\\audit\\audit.db", "Audit"))
    {
        if (EnsureDb(m_auditDb, "Audit", kAuditTables, (int)std::size(kAuditTables),
                     kAuditIndexes, (int)std::size(kAuditIndexes)))
            anyOk = true;
        else
            m_auditDb.Close();
    }

    if (m_fileHubDb.Open(base + "\\filehub\\filehub.db", "FileHub"))
    {
        if (EnsureDb(m_fileHubDb, "FileHub", kFileHubTables, (int)std::size(kFileHubTables),
                     kFileHubIndexes, (int)std::size(kFileHubIndexes)))
            anyOk = true;
        else
            m_fileHubDb.Close();
    }

    if (!anyOk)
        DEFAULT_LOG_ERROR("[DbInit] 全部数据库打开/初始化失败,服务数据库不可用");
    else
        DEFAULT_LOG_INFO("[DbInit] 数据库初始化完成");
    return anyOk;
}

void DbInitModule::Shutdown()
{
    m_userDb.Close();
    m_rateDb.Close();
    m_auditDb.Close();
    m_fileHubDb.Close();
}

zm::ZmSqliteConn& DbInitModule::UserDb()    { return m_userDb; }
zm::ZmSqliteConn& DbInitModule::RateDb()    { return m_rateDb; }
zm::ZmSqliteConn& DbInitModule::AuditDb()   { return m_auditDb; }
zm::ZmSqliteConn& DbInitModule::FileHubDb() { return m_fileHubDb; }

bool DbInitModule::EnsureDb(zm::ZmSqliteConn& conn, const char* tag,
                             const ZmDbTable* tables, int nTables,
                             const char* const* indexes, int nIndexes)
{
    for (int i = 0; i < nTables; ++i)
    {
        if (!EnsureTable(conn, tables[i]))
        {
            DEFAULT_LOG_ERROR("[DbInit] 库 {} 表 {} 初始化失败", tag, tables[i].name);
            return false;
        }
    }
    for (int i = 0; i < nIndexes; ++i)
    {
        if (!conn.Exec(indexes[i]))
        {
            DEFAULT_LOG_ERROR("[DbInit] 库 {} 索引创建失败", tag);
            return false;
        }
    }
    return true;
}

bool DbInitModule::EnsureTable(zm::ZmSqliteConn& conn, const ZmDbTable& t)
{
    // 1) 建表:全量最新列(约束在此生效;存量库 IF NOT EXISTS 自动跳过)
    std::string create = "CREATE TABLE IF NOT EXISTS ";
    create += t.name;
    create += " (";
    for (int i = 0; i < t.colCount; ++i)
    {
        if (i > 0)
            create += ", ";
        create += t.cols[i].name;
        create += " ";
        create += t.cols[i].def;
    }
    if (t.extra && *t.extra)
    {
        create += ", ";
        create += t.extra;
    }
    create += ")";
    if (!conn.Exec(create.c_str()))
        return false;

    // 2) 差集:收集现有列(小写化,SQLite 标识符大小写不敏感)
    std::set<std::string> existing;
    {
        zm::ZmSqliteStmt st(conn, ("PRAGMA table_info(" + std::string(t.name) + ")").c_str());
        if (!st.p)
            return false;
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            const char* col = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            if (!col)
                continue;
            std::string low = col;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            existing.insert(low);
        }
    }

    // 3) 补列:缺列且定义可追加 → 逐列 ALTER TABLE ADD COLUMN
    for (int i = 0; i < t.colCount; ++i)
    {
        std::string low = t.cols[i].name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (existing.count(low))
            continue;

        if (!AppendSafe(t.cols[i].def))
        {
            DEFAULT_LOG_ERROR("[DbInit] 表 {} 缺列 {} 且定义含 UNIQUE/PRIMARY KEY(或 NOT NULL 无默认值),"
                "无法 ALTER 追加;请将该列并入建表语句或人工迁移", t.name, t.cols[i].name);
            continue;
        }
        std::string alter = "ALTER TABLE ";
        alter += t.name;
        alter += " ADD COLUMN ";
        alter += t.cols[i].name;
        alter += " ";
        alter += t.cols[i].def;
        if (!conn.Exec(alter.c_str()))
            return false;
        DEFAULT_LOG_INFO("[DbInit] 表 {} 补列 {}", t.name, t.cols[i].name);
    }

    // 4) 种子/数据迁移(幂等)
    if (t.postSql && *t.postSql && !conn.Exec(t.postSql))
        return false;

    return true;
}

bool DbInitModule::AppendSafe(const char* def)
{
    std::string d = def;
    std::transform(d.begin(), d.end(), d.begin(), ::toupper);
    if (d.find("UNIQUE") != std::string::npos ||
        d.find("PRIMARY KEY") != std::string::npos)
        return false;
    if (d.find("NOT NULL") != std::string::npos && d.find("DEFAULT") == std::string::npos)
        return false;
    return true;
}
