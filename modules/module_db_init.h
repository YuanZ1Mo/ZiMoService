#ifndef MODULE_DB_INIT_H
#define MODULE_DB_INIT_H

#include "zm_util_sqlite.h"

/** @brief 列定义:name + 类型/约束(def 不含列名,CREATE 与 ALTER 双用) */
struct ZmDbColumn
{
    const char* name;
    const char* def;
};

/**
 * @brief 表定义(声明式 schema,单一事实源)
 * @param cols    全量最新列清单(新增列 = 在此追加一行,存量库自动 ALTER 补列)
 * @param extra   表级约束(仅用于建表语句),如 "UNIQUE(account, ip)";空串=无
 * @param postSql 建表+补列后执行的种子/数据迁移(须幂等);空串=无
 */
struct ZmDbTable
{
    const char*       name;
    const ZmDbColumn* cols;
    int               colCount;
    const char*       extra;
    const char*       postSql;
};

/**
 * @brief 数据库初始化模块(业务模块构造前执行)
 *
 * 统一打开并维护全部 4 个 SQLite 库(db\user\user.db / db\rate\rate.db /
 * db\audit\audit.db / db\filehub\filehub.db,exe 同级),业务模块(UserModule/
 * FileHubModule)构造时接收连接引用,不再自行开库/建表。
 *
 * Schema 维护方式(声明式,单一事实源):
 *   - 每个表在 module_db_init.cpp 声明一份列清单(ZmDbColumn 数组),含全量最新列;
 *   - 新建表:CREATE TABLE IF NOT EXISTS 带全量列(约束在此生效,仅全新库执行);
 *   - 存量库:PRAGMA table_info 差集,缺列且定义可追加(见 AppendSafe)时逐列
 *     ALTER TABLE ADD COLUMN 补列 —— 后续新增普通列只需在列清单追加一行,无需迁移;
 *   - 注意:SQLite 的 ALTER ADD COLUMN 不允许 UNIQUE/PRIMARY KEY/无默认值 NOT NULL,
 *     这类列只允许存在于建表语句;若存量库缺这类列,记 ERROR 日志跳过(需人工迁移)。
 *
 * 连接生命周期:本类拥有 4 个连接;业务模块持引用(不拥有)。
 * 析构/Shutdown 必须晚于所有持引用模块(线程 join 之后)再关闭连接。
 */
class DbInitModule
{
public:
    DbInitModule();
    ~DbInitModule();                // Shutdown() 兜底

    /** @brief 建 db 目录 → 逐库打开 + 建表/补列/索引/种子;单库失败记日志继续 */
    bool Open();

    /** @brief 关闭全部连接(幂等) */
    void Shutdown();

    zm::ZmSqliteConn& UserDb();     // db\user\user.db
    zm::ZmSqliteConn& RateDb();     // db\rate\rate.db
    zm::ZmSqliteConn& AuditDb();    // db\audit\audit.db
    zm::ZmSqliteConn& FileHubDb();  // db\filehub\filehub.db

private:
    static bool EnsureDb(zm::ZmSqliteConn& conn, const char* tag,
                         const ZmDbTable* tables, int nTables,
                         const char* const* indexes, int nIndexes);
    static bool EnsureTable(zm::ZmSqliteConn& conn, const ZmDbTable& t);

    /**
     * @brief 校验声明表级约束(extra)在存量库中真实存在(经 PRAGMA index_list/index_info
     *        比对唯一索引列集合);缺失记 ERROR 并返回 false —— 依赖该约束的
     *        ON CONFLICT upsert 会静默失效(如登录锁定计数),宁可整库不可用也不静默
     */
    static bool EnsureConstraint(zm::ZmSqliteConn& conn, const ZmDbTable& t);

    /** @brief 列定义是否可 ALTER 追加:不允许 UNIQUE / PRIMARY KEY / NOT NULL 无 DEFAULT */
    static bool AppendSafe(const char* def);

    zm::ZmSqliteConn m_userDb;
    zm::ZmSqliteConn m_rateDb;
    zm::ZmSqliteConn m_auditDb;
    zm::ZmSqliteConn m_fileHubDb;
};

#endif // MODULE_DB_INIT_H
