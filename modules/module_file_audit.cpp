#include "modules/module_file_audit.h"

#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>

using namespace drogon;

namespace
{
/// detail 内单条清单最多保留的条目数(超出只记总数)
constexpr size_t kDetailMaxItems = 200;

/// 截断字符串到上限(按字节;UTF-8 场景只用于出参快照)
std::string Clip(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    return s.substr(0, maxBytes);
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileAuditModule::ZmFileAuditModule(ZmFileDbModule* db) : m_db(db) {}

ZmFileAuditModule::~ZmFileAuditModule() = default;

// ============================================================================
// 写入(同事务)
// ============================================================================
bool ZmFileAuditModule::RecordFileOpSync(ZmSqliteDb& db, int64_t uid,
                                         const std::string& account, const std::string& action,
                                         int64_t space, int64_t nodeId,
                                         const std::string& nodeName,
                                         const std::string& detail, const std::string& ip,
                                         int result)
{
    return db.ExecSync(
        "INSERT INTO "
        "file_logs(uid,account,action,space,node_id,node_name,detail,ip,result,create_time) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
        {std::to_string(uid), account, action, std::to_string(space), std::to_string(nodeId),
         Clip(nodeName, 255), detail, Clip(ip, 64), std::to_string(result),
         std::to_string(ZmSqliteDb::Now())});
}

bool ZmFileAuditModule::RecordBatchSync(ZmSqliteDb& db, int64_t uid,
                                        const std::string& account, const std::string& action,
                                        int64_t space, const ZMJSON& items,
                                        const std::string& ip, int result)
{
    if (!items.is_array())
        return false;
    ZMJSON detail   = ZMJSON::object();
    size_t total    = items.size();
    detail["count"] = total;
    ZMJSON head     = ZMJSON::array();
    size_t keep     = std::min(total, kDetailMaxItems);
    for (size_t i = 0; i < keep; ++i)
    {
        const ZMJSON& it  = items[i];
        ZMJSON        one = ZMJSON::object();
        one["id"]         = zm_file_row_int(it, "id", 0);
        one["name"]       = zm_file_row_str(it, "name");
        if (it.contains("type"))
            one["type"] = zm_file_row_int(it, "type", 0);
        if (it.contains("size"))
            one["size"] = zm_file_row_int(it, "size", 0);
        if (it.contains("path"))
            one["path"] = zm_file_row_str(it, "path");
        head.push_back(std::move(one));
    }
    detail["items"] = std::move(head);
    if (total > kDetailMaxItems)
        detail["truncated"] = true;

    std::string first    = total > 0 ? zm_file_row_str(items[0], "name") : "";
    std::string nodeName = first;
    if (total > 1)
        nodeName += " 等 " + std::to_string(total) + " 项";
    return RecordFileOpSync(db, uid, account, action, space, 0, nodeName, detail.dump(), ip,
                            result);
}

// ============================================================================
// 写入(异步;只读行为)
// ============================================================================
drogon::Task<bool> ZmFileAuditModule::RecordDownload(int64_t uid, const std::string& account,
                                                     int64_t space, int64_t nodeId,
                                                     const std::string& nodeName,
                                                     int64_t bytesSent, bool ranged,
                                                     const std::string& ip, int result)
{
    ZMJSON detail    = ZMJSON::object();
    detail["bytes"]  = bytesSent;
    detail["ranged"] = ranged;
    bool ok          = co_await m_db->Exec(
        "INSERT INTO "
                 "file_logs(uid,account,action,space,node_id,node_name,detail,ip,result,create_time) "
                 "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
        {std::to_string(uid), account, zm_file::kActDownload, std::to_string(space),
                  std::to_string(nodeId), Clip(nodeName, 255), detail.dump(), Clip(ip, 64),
                  std::to_string(result), std::to_string(ZmSqliteDb::Now())});
    if (!ok)
        DEFAULT_LOG_WARN("ZmFileAuditModule: 下载审计写入失败 node={}", nodeId);
    co_return ok;
}

drogon::Task<bool>
ZmFileAuditModule::RecordShareAccess(int64_t shareId, const std::string& token, int action,
                                     int result, int64_t uid, const std::string& ip,
                                     const std::string& ua, const std::string& detail)
{
    bool ok = co_await m_db->Exec(
        "INSERT INTO "
        "share_logs(share_id,token,action,result,uid,node_id,ip,ua,detail,create_time) "
        "VALUES(?1,?2,?3,?4,?5,0,?6,?7,?8,?9)",
        {std::to_string(shareId), token, std::to_string(action), std::to_string(result),
         std::to_string(uid), Clip(ip, 64), Clip(ua, 255), Clip(detail, 512),
         std::to_string(ZmSqliteDb::Now())});
    if (!ok)
        DEFAULT_LOG_WARN("ZmFileAuditModule: 分享日志写入失败 share={} action={}", shareId,
                         action);
    co_return ok;
}

// ============================================================================
// 查询
// ============================================================================
drogon::Task<ZMJSON> ZmFileAuditModule::QueryFileLogs(const ZmFileLogQuery& q, int page,
                                                      int size)
{
    std::string              where = " WHERE 1=1";
    std::vector<std::string> p;
    if (q.uid > 0)
    {
        where += " AND uid = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.uid));
    }
    if (!q.action.empty())
    {
        where += " AND action = ?" + std::to_string(p.size() + 1);
        p.push_back(q.action);
    }
    if (q.space >= 0)
    {
        where += " AND space = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.space));
    }
    if (q.from > 0)
    {
        where += " AND create_time >= ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.from));
    }
    if (q.to > 0)
    {
        where += " AND create_time <= ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.to));
    }
    if (!q.keyword.empty())
    {
        where += " AND node_name LIKE ?" + std::to_string(p.size() + 1);
        p.push_back("%" + q.keyword + "%");
    }

    ZMJSON totalRow =
        co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM file_logs" + where, p);
    int64_t total = zm_file_row_int(totalRow, "n", 0);

    std::vector<std::string> lp = p;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string((page - 1) * size));
    ZMJSON list = co_await m_db->QueryRows(
        "SELECT id,uid,account,action,space,node_id,node_name,detail,ip,result,create_time "
        "FROM file_logs" +
            where + " ORDER BY id DESC LIMIT ?" + std::to_string(lp.size() - 1) + " OFFSET ?" +
            std::to_string(lp.size()),
        lp);

    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return out;
}

drogon::Task<ZMJSON> ZmFileAuditModule::QueryShareLogs(const ZmShareLogQuery& q, int page,
                                                       int size)
{
    std::string              where = " WHERE 1=1";
    std::vector<std::string> p;
    if (!q.token.empty())
    {
        where += " AND token = ?" + std::to_string(p.size() + 1);
        p.push_back(q.token);
    }
    if (q.shareId > 0)
    {
        where += " AND share_id = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.shareId));
    }
    if (q.from > 0)
    {
        where += " AND create_time >= ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.from));
    }
    if (q.to > 0)
    {
        where += " AND create_time <= ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(q.to));
    }

    ZMJSON totalRow =
        co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM share_logs" + where, p);
    int64_t total = zm_file_row_int(totalRow, "n", 0);

    std::vector<std::string> lp = p;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string((page - 1) * size));
    ZMJSON list = co_await m_db->QueryRows(
        "SELECT id,share_id,token,action,result,uid,node_id,ip,ua,detail,create_time "
        "FROM share_logs" +
            where + " ORDER BY id DESC LIMIT ?" + std::to_string(lp.size() - 1) + " OFFSET ?" +
            std::to_string(lp.size()),
        lp);
    // 脱敏在写入侧统一处理,调用方不需要关心
    for (auto& row : list)
    {
        row["ip"] = MaskIp(zm_file_row_str(row, "ip"));
        row["ua"] = MaskUa(zm_file_row_str(row, "ua"));
    }

    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return out;
}

drogon::Task<ZMJSON> ZmFileAuditModule::QueryShareLogsByShare(int64_t shareId, int page,
                                                              int size)
{
    ZmShareLogQuery q;
    q.shareId = shareId;
    co_return co_await QueryShareLogs(q, page, size);
}

// ============================================================================
// 脱敏
// ============================================================================
std::string ZmFileAuditModule::MaskIp(const std::string& ip)
{
    if (ip.empty())
        return "";
    // IPv4:保留前三段。其余(IPv6 等)只保留前 6 个字符
    size_t first = ip.find('.');
    if (first != std::string::npos)
    {
        size_t second = ip.find('.', first + 1);
        if (second != std::string::npos)
        {
            size_t third = ip.find('.', second + 1);
            if (third != std::string::npos)
                return ip.substr(0, third) + ".x";
        }
    }
    return ip.size() > 6 ? ip.substr(0, 6) + "..." : ip;
}

std::string ZmFileAuditModule::MaskUa(const std::string& ua)
{
    if (ua.size() <= 24)
        return ua;
    // 按 UTF-8 字符边界截断,避免把多字节字符劈开
    size_t cut = 24;
    while (cut > 0 && (static_cast<unsigned char>(ua[cut]) & 0xC0) == 0x80)
        --cut;
    return ua.substr(0, cut) + "...";
}
