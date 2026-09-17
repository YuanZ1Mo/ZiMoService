#include "modules/module_file_admin.h"

#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_pack.h"
#include "modules/module_file_store.h"
#include "modules/module_file_upload.h"
#include "modules/module_file_task.h"
#include "modules/module_gate.h"
#include "modules/module_permission.h"
#include "modules/module_session.h"
#include "modules/module_user.h"

#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <set>

using namespace drogon;

namespace
{
/// 磁盘名与库名的大小写无关比较(Windows 文件名不区分大小写)
bool NameEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (::tolower(static_cast<unsigned char>(a[i])) !=
            ::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// 查询参数助手
int QueryInt(const HttpRequestPtr& req, const char* name, int def, int minV, int maxV)
{
    std::string s = req->getParameter(name);
    if (s.empty())
        return def;
    try
    {
        return std::min(maxV, std::max(minV, std::stoi(s)));
    }
    catch (...)
    {
        return def;
    }
}

int64_t QueryI64(const HttpRequestPtr& req, const char* name, int64_t def)
{
    std::string s = req->getParameter(name);
    if (s.empty())
        return def;
    try
    {
        return std::stoll(s);
    }
    catch (...)
    {
        return def;
    }
}

ZMJSON ParseBody(const HttpRequestPtr& req)
{
    std::string_view sv = req->getBody();
    if (sv.empty())
        return ZMJSON::object();
    std::string err;
    ZMJSON      j = zm_json_parse(std::string(sv), err);
    return j.is_object() ? j : ZMJSON::object();
}

std::vector<int64_t> BodyIds(const ZMJSON& body)
{
    std::vector<int64_t> ids;
    if (!body.contains("ids") || !body["ids"].is_array())
        return ids;
    for (const auto& v : body["ids"])
    {
        if (v.is_number_integer())
            ids.push_back(v.get<int64_t>());
        else if (v.is_string())
        {
            try
            {
                ids.push_back(std::stoll(v.get<std::string>()));
            }
            catch (...)
            {
            }
        }
    }
    return ids;
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileAdminModule::ZmFileAdminModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                                     ZmPermissionModule* permission, ZmAuthGateModule* gate,
                                     ZmUserModule* user, ZmFileDbModule* db,
                                     ZmFileNodeModule* node, ZmFileTaskModule* task,
                                     ZmFileAuditModule* audit, ZmFilePackModule* pack,
                                     ZmFileStoreModule* store, ZmFileUploadModule* upload)
    : m_rest(rest), m_session(session), m_permission(permission), m_gate(gate), m_user(user),
      m_db(db), m_node(node), m_task(task), m_audit(audit), m_pack(pack), m_store(store),
      m_upload(upload)
{
    m_syncProgress = ZMJSON::object();
}

ZmFileAdminModule::~ZmFileAdminModule() = default;

// ============================================================================
// 门禁与响应
// ============================================================================
drogon::Task<ZmGateResult> ZmFileAdminModule::Authorize(const HttpRequestPtr& req)
{
    ZmGateResult      r;
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    r.ctx  = co_await m_session->AuthAndTouch(cookie, ZmAuthGateModule::ClientIp(req));
    auto g = ZmAuthGateModule::CheckCtxSync(r.ctx, req->path());
    if (!g.ok)
    {
        r.status  = g.status;
        r.code    = g.code;
        r.message = g.message;
        co_return r;
    }
    if (!(co_await m_permission->HasPermission(r.ctx.uid, "filehubAdmin")))
    {
        r.status  = 403;
        r.code    = "PERM_DENIED";
        r.message = "无权限访问";
        co_return r;
    }
    r.ok = true;
    co_return r;
}

HttpResponsePtr ZmFileAdminModule::Respond(const ZMJSON& out)
{
    if (!ZmFileHasError(out))
        return ZmAuthGateModule::ApiOk(out);
    const ZMJSON& e    = out["error"];
    ZMJSON        body = ZMJSON::object();
    body["code"]       = ZmFileErrorCode(out);
    body["message"]    = ZmFileErrorMessage(out);
    for (auto it = e.begin(); it != e.end(); ++it)
    {
        const std::string& k = it.key();
        if (k == "code" || k == "message" || k == "status")
            continue;
        body[k] = it.value();
    }
    return ZmHttpServer::JsonResponse(ZmFileErrorStatus(out), body);
}

int64_t ZmFileAdminModule::TodayStart()
{
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    return static_cast<int64_t>(std::mktime(&tm));
}

// ============================================================================
// 路由注册
// ============================================================================
void ZmFileAdminModule::RegisterRoutes()
{
    if (!m_rest)
        return;
    const char* k = "/zimo/api/filehub/admin";
    m_rest->RegisterCoro(std::string(k) + "/stats", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleStats(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/sync/cancel", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSyncCancel(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/sync", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSyncStart(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/sync", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSyncStatus(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/cache/clean", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleCacheClean(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/cache", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleCache(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/restore", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashRestore(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/purge", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashPurge(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/clean", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashClean(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash/clear", HttpMethod::Post,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrashClearAll(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/trash", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTrash(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/tasks", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleTasks(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/tasks/{1}/cancel", HttpMethod::Post,
        [this](HttpRequestPtr req, std::string taskNo) -> Task<HttpResponsePtr>
        { return HandleTaskCancel(std::move(req), std::move(taskNo)); });
    m_rest->RegisterCoro(std::string(k) + "/logs", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleLogs(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/share_logs", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleShareLogs(std::move(req)); });
    m_rest->RegisterCoro(std::string(k) + "/spaces", HttpMethod::Get,
                         [this](HttpRequestPtr req) -> Task<HttpResponsePtr>
                         { return HandleSpaces(std::move(req)); });
    m_rest->RegisterCoroWithPathParams(
        std::string(k) + "/spaces/{1}", HttpMethod::Patch,
        [this](HttpRequestPtr req, std::string spaceStr) -> Task<HttpResponsePtr>
        { return HandleSetQuota(std::move(req), std::move(spaceStr)); });
    DEFAULT_LOG_INFO("ZmFileAdminModule: /filehub/admin/* 接口已注册");
}

void ZmFileAdminModule::StartMaintenance()
{
    if (!m_pack || !m_upload)
        return;
    // 事件循环启动后才可注册定时器;每 10 分钟一次巡检(时效性维护)
    drogon::app().registerBeginningAdvice(
        [this]()
        {
            trantor::EventLoop* loop = drogon::app().getLoop();
            if (!loop)
                return;
            loop->runEvery(600.0,
                           [this]()
                           {
                               int64_t now = ZmSqliteDb::Now();
                               ZmHttpServer::WorkPool().Submit(
                                   [this, now]()
                                   {
                                       if (m_pack)
                                           m_pack->CleanIdle(now);
                                       if (m_upload)
                                           m_upload->SweepStaleTasks(now);
                                   });
                           });
            DEFAULT_LOG_INFO("ZmFileAdminModule: 高频巡检已注册(每 10 分钟)");
        });
}

// ============================================================================
// 概览统计
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleStats(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t today = TodayStart();
    ZMJSON  out   = co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, today]() -> ZMJSON
        {
            ZMJSON o = ZMJSON::object();
            // 公共空间
            ZMJSON pub = m_db->QueryRowSync(
                "SELECT COUNT(*) AS items, COALESCE(SUM(size),0) AS bytes, "
                   "COALESCE(SUM(CASE WHEN deleted=1 THEN size ELSE 0 END),0) AS trash_bytes "
                   "FROM nodes WHERE space = 0",
                {});
            o["public"]                = ZMJSON::object();
            o["public"]["items"]       = zm_file_row_int(pub, "items", 0);
            o["public"]["bytes"]       = zm_file_row_int(pub, "bytes", 0);
            o["public"]["trash_bytes"] = zm_file_row_int(pub, "trash_bytes", 0);
            // 个人空间
            ZMJSON pri = m_db->QueryRowSync(
                "SELECT COUNT(*) AS users, COALESCE(SUM(used_size),0) AS bytes FROM spaces "
                   "WHERE space <> 0",
                {});
            o["personal"]          = ZMJSON::object();
            o["personal"]["users"] = zm_file_row_int(pri, "users", 0);
            o["personal"]["bytes"] = zm_file_row_int(pri, "bytes", 0);
            ZMJSON top             = m_db->QueryRowsSync(
                "SELECT space, used_size FROM spaces WHERE space <> 0 ORDER BY used_size DESC "
                               "LIMIT 10",
                {});
            ZMJSON topArr = ZMJSON::array();
            for (const auto& r : top)
            {
                ZMJSON t   = ZMJSON::object();
                t["uid"]   = zm_file_row_int(r, "space", 0);
                t["bytes"] = zm_file_row_int(r, "used_size", 0);
                topArr.push_back(std::move(t));
            }
            o["personal"]["top"] = std::move(topArr);
            // 回收站
            ZMJSON tr = m_db->QueryRowSync(
                "SELECT COUNT(*) AS items, COALESCE(SUM(size),0) AS bytes, "
                   "COALESCE(SUM(CASE WHEN delete_time < ?1 THEN 1 ELSE 0 END),0) AS expiring "
                   "FROM nodes WHERE deleted = 1",
                {std::to_string(today + 7 * 86400 - zm_file::kTrashRetainDays * 86400)});
            o["trash"]                = ZMJSON::object();
            o["trash"]["items"]       = zm_file_row_int(tr, "items", 0);
            o["trash"]["bytes"]       = zm_file_row_int(tr, "bytes", 0);
            o["trash"]["expiring_7d"] = zm_file_row_int(tr, "expiring", 0);
            // 分享
            ZMJSON sh = m_db->QueryRowSync("SELECT COALESCE(SUM(CASE WHEN status = 1 THEN 1 "
                                                 "ELSE 0 END),0) AS active FROM shares",
                                              {});
            ZMJSON sv = m_db->QueryRowSync(
                "SELECT COUNT(*) AS n FROM share_logs WHERE create_time >= ?1 AND action = 1",
                {std::to_string(today)});
            ZMJSON sd  = m_db->QueryRowSync("SELECT COUNT(*) AS n FROM share_logs WHERE "
                                                   "create_time >= ?1 AND action IN (4,5)",
                                               {std::to_string(today)});
            o["share"] = ZMJSON::object();
            o["share"]["active"]          = zm_file_row_int(sh, "active", 0);
            o["share"]["today_views"]     = zm_file_row_int(sv, "n", 0);
            o["share"]["today_downloads"] = zm_file_row_int(sd, "n", 0);
            // 任务
            ZMJSON tk = m_db->QueryRowSync("SELECT COALESCE(SUM(CASE WHEN status IN (1,2) "
                                                 "THEN 1 ELSE 0 END),0) AS running, "
                                                 "COALESCE(SUM(CASE WHEN status = 3 AND create_time "
                                                 ">= ?1 THEN 1 ELSE 0 END),0) AS ok, "
                                                 "COALESCE(SUM(CASE WHEN status = 4 AND create_time "
                                                 ">= ?1 THEN 1 ELSE 0 END),0) AS fail "
                                                 "FROM transfer_tasks",
                                              {std::to_string(today)});
            o["task"] = ZMJSON::object();
            o["task"]["running"]    = zm_file_row_int(tk, "running", 0);
            o["task"]["today_ok"]   = zm_file_row_int(tk, "ok", 0);
            o["task"]["today_fail"] = zm_file_row_int(tk, "fail", 0);
            return o;
        });
    // Top 用户昵称(展示层;经用户模块批量取,前端概览卡直接用 top[0].name)
    {
        std::vector<int64_t> uids;
        for (const auto& t : out["personal"]["top"])
        {
            int64_t uid = zm_file_row_int(t, "uid", 0);
            if (uid > 0)
                uids.push_back(uid);
        }
        ZMJSON names = co_await m_user->GetNicknames(uids);
        for (auto& t : out["personal"]["top"])
        {
            std::string key = std::to_string(zm_file_row_int(t, "uid", 0));
            t["name"] = names.contains(key) ? names[key].get<std::string>() : ("用户 " + key);
        }
    }
    // 缓存区与库大小(文件系统侧)
    int64_t cacheBytes = 0;
    int64_t zips       = 0;
    int64_t chunks     = 0;
    if (m_pack)
        m_pack->StatCache(cacheBytes, zips, chunks);
    out["cache"]           = ZMJSON::object();
    out["cache"]["bytes"]  = cacheBytes;
    out["cache"]["zips"]   = zips;
    out["cache"]["chunks"] = chunks;
    out["db_size"]         = co_await m_db->DbFileSize();
    co_return Respond(out);
}

// ============================================================================
// 一致性同步
// ============================================================================
drogon::Task<ZMJSON> ZmFileAdminModule::StartSync(bool dryRun, const ZmOpCtx& ctx)
{
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        if (m_syncRunning.load())
            co_return ZmFileError(zm_file_err::kSyncRunning, 409, "已有一致性同步在运行");
        m_syncRunning.store(true);
        m_syncCancel.store(false);
    }
    // 任务表登记(type=5),前端按任务轮询进度
    ZMJSON created = co_await m_task->Start(
        zm_file::kTaskSync, ctx.uid, -1, "全空间一致性同步", dryRun ? "预演" : "", 0, 0, "",
        [this, dryRun, ctx](ZmTaskHandle& h) { RunSyncBody(dryRun, ctx, h); });
    std::string taskNo = zm_file_row_str(created, "task_no");
    if (taskNo.empty())
    {
        ResetSyncFlags();   // 任务行没建起来 → 不留"运行中"标志,否则后续同步全被 409 挡住
        co_return created;
    }
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        m_syncTaskNo = taskNo;
    }
    co_return created;
}

bool ZmFileAdminModule::StartSyncDetached(bool dryRun, const ZmOpCtx& ctx)
{
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        if (m_syncRunning.load())
            return false;
        m_syncRunning.store(true);
        m_syncCancel.store(false);
    }
    // 任务行用同步接口创建:本入口由清理钩子在工作池线程调用,拿不到事件循环
    std::string taskNo;
    if (!m_task->CreateSync(zm_file::kTaskSync, ctx.uid, -1, "全空间一致性同步",
                            dryRun ? "预演" : "", 0, 0, "", taskNo))
    {
        ResetSyncFlags();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        m_syncTaskNo = taskNo;
    }
    ZmHttpServer::WorkPool().Submit(
        [this, taskNo, dryRun, ctx]()
        {
            m_task->SetStatusSync(taskNo, zm_file::kTaskRunning);
            ZmTaskHandle h(m_task, taskNo, ctx.uid, -1);
            RunSyncBody(dryRun, ctx, h);
        });
    return true;
}

void ZmFileAdminModule::ResetSyncFlags()
{
    std::lock_guard<std::mutex> lk(m_syncMtx);
    m_syncRunning.store(false);
    m_syncCancel.store(false);
    m_syncBeginMs = 0;
}

void ZmFileAdminModule::RunSyncBody(bool dryRun, const ZmOpCtx& ctx, ZmTaskHandle& h)
{
    try
    {
        ZMJSON report = RunSyncSync(h.TaskNo(), dryRun, &h);
        h.Finish(zm_file::kTaskDone, "", report.dump());
        // 审计:管理操作与业务同事务(此处只有一行日志)
        m_db->WithTxSync(
            [&](ZmSqliteDb& db) -> bool
            {
                return m_audit->RecordFileOpSync(
                    db, ctx.uid, ctx.account, zm_file::kActAdminSync, -1, 0,
                    "全空间一致性同步", report.dump(), ctx.ip, 1);
            });
    }
    catch (const std::exception& e)
    {
        DEFAULT_LOG_ERROR("一致性同步执行异常: {}", e.what());
        h.Finish(zm_file::kTaskFailed, std::string("内部错误:") + e.what());
        ResetSyncFlags();
    }
    catch (...)
    {
        h.Finish(zm_file::kTaskFailed, "内部错误");
        ResetSyncFlags();
    }
}

namespace
{
/// 同步报告初始化
ZMJSON NewSyncReport()
{
    ZMJSON r           = ZMJSON::object();
    r["scanned"]       = 0;   // 已扫描目录数
    r["total_dirs"]    = 0;   // 库中目录总数(进度分母;磁盘多出的目录会让 scanned 超过它)
    r["scanned_items"] = 0;   // 已扫描条目数(磁盘侧)
    r["added"]         = 0;
    r["removed"]       = 0;
    r["fixed"]         = 0;
    r["skipped"]       = 0;
    return r;
}
} // namespace

ZMJSON ZmFileAdminModule::SyncDirSync(int64_t space, int64_t dirId, bool dryRun,
                                      ZmTaskHandle* handle, ZMJSON& report)
{
    // 单个目录:读盘 → 与库比对 → 落库(锁内完成,不长时间持锁)
    std::string phys = m_store->SpaceRoot(space);
    if (dirId != 0)
    {
        int64_t     sp = space;
        std::string rel;
        if (!m_db->PathPartsSync(dirId, sp, rel))
            return report;
        phys += "\\" + rel;
    }
    std::vector<ZmDiskEntry> disk;
    if (!m_store->ScanDirSync(phys, disk).ok)
    {
        report["skipped"] = zm_file_row_int(report, "skipped", 0) + 1;
        return report;
    }
    report["scanned_items"] =
        zm_file_row_int(report, "scanned_items", 0) + static_cast<int64_t>(disk.size());
    // 库中该目录下的全部行(含 deleted=1:它们占着"已登记"名额,不能被补建)
    ZMJSON dbRows = m_db->QueryRowsSync(
        "SELECT id, name, type, deleted, size FROM nodes WHERE space = ?1 AND parent_id = ?2",
        {std::to_string(space), std::to_string(dirId)});

    // ── 磁盘 → 库:补建缺失条目 ──
    for (const auto& d : disk)
    {
        if (handle && handle->Cancelled())
            return report;
        bool found = false;
        for (const auto& r : dbRows)
        {
            if (NameEquals(zm_file_row_str(r, "name"), d.name))
            {
                found = true;
                break;
            }
        }
        if (found)
            continue;
        if (!dryRun)
        {
            int64_t     now = ZmSqliteDb::Now();
            std::string ext = d.isDir ? "" : ZmFileNodeModule::ExtOf(d.name);
            m_db->WithTxSync(
                [&](ZmSqliteDb& db) -> bool
                {
                    if (!db.ExecSync(
                            "INSERT INTO "
                            "nodes(space,parent_id,type,name,size,ext,hash,owner_uid,"
                            "items,create_time,update_time,deleted,delete_time,origin_parent_"
                            "id,"
                            "del_owner_uid) VALUES(?1,?2,?3,?4,?5,?6,'',0,0,?7,?7,0,0,0,0)",
                            {std::to_string(space), std::to_string(dirId),
                             std::to_string(d.isDir ? zm_file::kTypeDir : zm_file::kTypeFile),
                             d.name, std::to_string(d.size), ext,
                             std::to_string(d.mtime > 0 ? d.mtime : now)}))
                        return false;
                    return ZmFileDbModule::ApplyUsageSync(db, space, d.isDir ? 0 : d.size, 1);
                });
        }
        report["added"] = zm_file_row_int(report, "added", 0) + 1;
    }

    // ── 库 → 磁盘:删行 / 改名 / 重建 ──
    for (const auto& r : dbRows)
    {
        if (handle && handle->Cancelled())
            return report;
        int64_t            id      = zm_file_row_int(r, "id", 0);
        std::string        name    = zm_file_row_str(r, "name");
        int                type    = static_cast<int>(zm_file_row_int(r, "type", 0));
        int                deleted = static_cast<int>(zm_file_row_int(r, "deleted", 0));
        const ZmDiskEntry* hit     = nullptr;
        for (const auto& d : disk)
        {
            if (NameEquals(d.name, name))
            {
                hit = &d;
                break;
            }
        }
        if (deleted == 1)
            continue; // 回收站行:物理缺失属正常,补建方向已"计入已登记"
        if (!hit)
        {
            if (!dryRun)
            {
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    {
                        int64_t subBytes = 0;
                        ZMJSON  sub      = {};
                        if (type == zm_file::kTypeDir)
                        {
                            int64_t items = 0;
                            sub           = m_node->SubtreeIdsSync(id, &subBytes, &items);
                            if (!db.ExecSync(
                                    "WITH RECURSIVE s(id, depth) AS ("
                                    " SELECT id, 0 FROM nodes WHERE id = ?1"
                                    " UNION ALL"
                                    " SELECT n.id, s.depth + 1 FROM nodes n JOIN s ON "
                                    "n.parent_id = s.id"
                                    " WHERE s.depth < 64)"
                                    " DELETE FROM nodes WHERE id IN (SELECT id FROM s);",
                                    {std::to_string(id)}))
                                return false;
                            if (!ZmFileDbModule::ApplyUsageSync(
                                    db, space, -subBytes, -static_cast<int64_t>(sub.size())))
                                return false;
                        }
                        else
                        {
                            int64_t size = zm_file_row_int(r, "size", 0);
                            if (!db.ExecSync("DELETE FROM nodes WHERE id = ?1",
                                             {std::to_string(id)}))
                                return false;
                            if (!ZmFileDbModule::ApplyUsageSync(db, space, -size, -1))
                                return false;
                        }
                        return true;
                    });
            }
            report["removed"] = zm_file_row_int(report, "removed", 0) + 1;
            continue;
        }
        // 类型不符:以磁盘为准,删原行按磁盘类型重建
        bool typeMismatch = (type == zm_file::kTypeDir) != hit->isDir;
        if (typeMismatch)
        {
            if (!dryRun)
            {
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    {
                        if (!db.ExecSync("DELETE FROM nodes WHERE id = ?1",
                                         {std::to_string(id)}))
                            return false;
                        std::string ext = hit->isDir ? "" : ZmFileNodeModule::ExtOf(hit->name);
                        return db.ExecSync(
                            "INSERT INTO "
                            "nodes(space,parent_id,type,name,size,ext,hash,owner_uid,"
                            "items,create_time,update_time,deleted,delete_time,origin_parent_"
                            "id,"
                            "del_owner_uid) VALUES(?1,?2,?3,?4,?5,?6,'',0,0,?7,?7,0,0,0,0)",
                            {std::to_string(space), std::to_string(dirId),
                             std::to_string(hit->isDir ? zm_file::kTypeDir
                                                       : zm_file::kTypeFile),
                             hit->name, std::to_string(hit->size), ext,
                             std::to_string(hit->mtime)});
                    });
            }
            report["fixed"] = zm_file_row_int(report, "fixed", 0) + 1;
            continue;
        }
        // 名称大小写不一致:以磁盘为准
        if (hit->name != name)
        {
            if (!dryRun)
            {
                m_db->ExecSync("UPDATE nodes SET name = ?1 WHERE id = ?2",
                               {hit->name, std::to_string(id)});
            }
            report["fixed"] = zm_file_row_int(report, "fixed", 0) + 1;
        }
        // 文件大小漂移同样以磁盘为准
        if (!hit->isDir && zm_file_row_int(r, "size", 0) != hit->size)
        {
            if (!dryRun)
            {
                int64_t oldSize = zm_file_row_int(r, "size", 0);
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    {
                        if (!db.ExecSync("UPDATE nodes SET size = ?1 WHERE id = ?2",
                                         {std::to_string(hit->size), std::to_string(id)}))
                            return false;
                        return ZmFileDbModule::ApplyUsageSync(db, space, hit->size - oldSize,
                                                              0);
                    });
            }
            report["fixed"] = zm_file_row_int(report, "fixed", 0) + 1;
        }
    }

    // 展开子目录(以库为准遍历:补建出来的目录也在库里了)
    report["scanned"] = zm_file_row_int(report, "scanned", 0) + 1;
    if (handle)
        handle->Progress(zm_file_row_int(report, "scanned", 0),
                         zm_file_row_int(report, "scanned", 0));
    // 运行中进度快照:sync 状态接口据此实时返回(不依赖任务行的节流写库)
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        m_syncProgress            = report;
        m_syncProgress["total"]   = zm_file_row_int(report, "total_dirs", 0);
        m_syncProgress["scanned"] = zm_file_row_int(report, "scanned", 0);
    }
    ZMJSON children = m_db->QueryRowsSync(
        "SELECT id FROM nodes WHERE space = ?1 AND parent_id = ?2 AND type = ?3 "
        "AND deleted = 0 ORDER BY id ASC",
        {std::to_string(space), std::to_string(dirId), std::to_string(zm_file::kTypeDir)});
    for (const auto& c : children)
    {
        if (handle && handle->Cancelled())
            return report;
        SyncDirSync(space, zm_file_row_int(c, "id", 0), dryRun, handle, report);
    }
    return report;
}

ZMJSON ZmFileAdminModule::RunSyncSync(const std::string& taskNo, bool dryRun,
                                      ZmTaskHandle* handle)
{
    auto   begin  = std::chrono::steady_clock::now();
    ZMJSON report = NewSyncReport();
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        m_syncProgress            = ZMJSON::object();
        m_syncProgress["scanned"] = 0;
        m_syncProgress["total"]   = 0;
        m_syncProgress["added"]   = 0;
        m_syncProgress["removed"] = 0;
        m_syncProgress["fixed"]   = 0;
        m_syncProgress["skipped"] = 0;
        // 运行中 elapsed 的基准:结束后由 report.elapsed 给总耗时,中途靠它推算
        m_syncBeginMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            begin.time_since_epoch())
                            .count();
    }
    // 进度分母:库中可见目录总数(单条聚合查询,代价可忽略)
    {
        ZMJSON cnt = m_db->QueryRowSync(
            "SELECT COUNT(*) AS n FROM nodes WHERE type = ?1 AND deleted = 0",
            {std::to_string(zm_file::kTypeDir)});
        report["total_dirs"] = zm_file_row_int(cnt, "n", 0);
    }
    ZMJSON spaces = m_db->QueryRowsSync("SELECT space FROM spaces ORDER BY space ASC", {});
    for (const auto& s : spaces)
    {
        if (handle && handle->Cancelled())
            break;
        int64_t space = zm_file_row_int(s, "space", 0);
        SyncDirSync(space, 0, dryRun, handle, report);
        // 结束后重算该空间用量(含回收站占用)
        if (!dryRun)
        {
            m_db->WithTxSync([&](ZmSqliteDb& db) -> bool
                             { return ZmFileDbModule::RecountUsageSync(db, space); });
        }
    }
    auto    end = std::chrono::steady_clock::now();
    int64_t elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    report["elapsed"] = elapsedMs;
    report["dry_run"] = dryRun;
    report["task_no"] = taskNo;
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        m_syncProgress          = report;
        m_syncProgress["total"] = zm_file_row_int(report, "scanned", 0);
        m_syncRunning.store(false);
        m_syncCancel.store(false);
        m_syncBeginMs = 0;
    }
    DEFAULT_LOG_INFO(
        "一致性同步完成: 扫描目录={} 补建={} 删行={} 修正={} 跳过={} 耗时={}ms",
        zm_file_row_int(report, "scanned", 0), zm_file_row_int(report, "added", 0),
        zm_file_row_int(report, "removed", 0), zm_file_row_int(report, "fixed", 0),
        zm_file_row_int(report, "skipped", 0), elapsedMs);
    return report;
}

drogon::Task<ZMJSON> ZmFileAdminModule::CancelSync()
{
    if (!m_syncRunning.load())
        co_return ZmFileError(zm_file_err::kBadRequest, 400, "当前没有正在运行的同步");
    m_syncCancel.store(true);
    co_return ZMJSON::object();
}

// ============================================================================
// 管理端 handler
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleSyncStart(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body   = ParseBody(req);
    bool    dryRun = zm_json_get_bool(body, "dry_run", false);
    ZmOpCtx ctx;
    ctx.uid     = gate.ctx.uid;
    ctx.account = gate.ctx.account;
    ctx.ip      = ZmAuthGateModule::ClientIp(req);
    ZMJSON out  = co_await StartSync(dryRun, ctx);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleSyncStatus(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out     = ZMJSON::object();
    bool   running = m_syncRunning.load();
    out["running"] = running;
    if (running)
    {
        std::lock_guard<std::mutex> lk(m_syncMtx);
        ZMJSON p     = m_syncProgress.is_object() ? m_syncProgress : ZMJSON::object();
        p["task_no"] = m_syncTaskNo;
        // 已运行时长(毫秒):进度快照本身不带时间,按开始时刻现算
        if (m_syncBeginMs > 0)
            p["elapsed"] =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count() -
                m_syncBeginMs;
        out["progress"] = std::move(p);
    }
    else
    {
        out["progress"] = nullptr;
    }
    // 最近一次结果:取已完成(type=5)的最近一条任务
    ZMJSON last = co_await m_db->QueryRow(
        "SELECT * FROM transfer_tasks WHERE type = ?1 AND status = ?2 ORDER BY id DESC LIMIT "
        "1",
        {std::to_string(zm_file::kTaskSync), std::to_string(zm_file::kTaskDone)});
    if (last.empty())
    {
        out["last"] = nullptr;
    }
    else
    {
        ZMJSON l           = ZMJSON::object();
        l["task_no"]       = zm_file_row_str(last, "task_no");
        l["end_time"]      = zm_file_row_int(last, "end_time", 0);
        std::string result = zm_file_row_str(last, "result");
        std::string err;
        ZMJSON      rep = result.empty() ? ZMJSON::object() : zm_json_parse(result, err);
        l["report"]     = rep.is_object() ? rep : ZMJSON::object();
        out["last"]     = std::move(l);
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleSyncCancel(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await CancelSync();
    co_return Respond(out);
}

// ============================================================================
// 缓存区
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleCache(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_pack->ListCache();
    // 归属名:缓存文件名前缀是 uid
    std::vector<int64_t> uids;
    for (const auto& it : out["list"])
    {
        try
        {
            int64_t uid = std::stoll(zm_file_row_str(it, "space"));
            uids.push_back(uid);
        }
        catch (...)
        {
        }
    }
    ZMJSON names = co_await m_user->GetNicknames(uids);
    for (auto& it : out["list"])
    {
        std::string key = zm_file_row_str(it, "space");
        it["owner"]     = key == "0" ? "公共空间"
                                     : (names.contains(key) ? names[key].get<std::string>()
                                                            : ("用户 " + key));
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleCacheClean(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON                   body = ParseBody(req);
    std::vector<std::string> names;
    if (body.contains("names") && body["names"].is_array())
    {
        for (const auto& v : body["names"])
        {
            if (v.is_string())
                names.push_back(v.get<std::string>());
        }
    }
    ZMJSON  out = co_await m_pack->CleanCacheFiles(names);
    ZmOpCtx ctx;
    ctx.uid           = gate.ctx.uid;
    ctx.account       = gate.ctx.account;
    ctx.ip            = ZmAuthGateModule::ClientIp(req);
    ZMJSON detail     = ZMJSON::object();
    detail["deleted"] = zm_file_row_int(out, "deleted", 0);
    detail["bytes"]   = zm_file_row_int(out, "bytes", 0);
    co_await m_db->WithTx(
        [&](ZmSqliteDb& db) -> bool
        {
            return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, zm_file::kActAdminCache,
                                             -1, 0, "缓存清理", detail.dump(), ctx.ip, 1);
        });
    co_return Respond(out);
}

// ============================================================================
// 回收站(全量)
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTrash(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZmListQuery q;
    q.page        = QueryInt(req, "page", 1, 1, 1000000);
    q.size        = QueryInt(req, "size", 50, 1, 500);
    q.sort        = req->getParameter("sort").empty() ? "mtime" : req->getParameter("sort");
    q.order       = req->getParameter("order").empty() ? "desc" : req->getParameter("order");
    int64_t space = req->getParameter("space").empty() ? -1 : QueryI64(req, "space", -1);
    int64_t from  = QueryI64(req, "from", 0);
    int64_t to    = QueryI64(req, "to", 0);
    q.mtimeFrom   = from;
    q.mtimeTo     = to;
    ZMJSON out    = co_await m_node->TrashList(space, q, 0, true);
    if (!ZmFileHasError(out))
    {
        // 归属名与空间名(space_name)
        std::vector<int64_t> uids;
        for (const auto& it : out["list"])
        {
            int64_t uid = zm_file_row_int(it, "del_owner_uid", 0);
            if (uid > 0)
                uids.push_back(uid);
            int64_t sp = zm_file_row_int(it, "space", 0);
            if (sp > 0)
                uids.push_back(sp);
        }
        ZMJSON names = co_await m_user->GetNicknames(uids);
        for (auto& it : out["list"])
        {
            int64_t     uid = zm_file_row_int(it, "del_owner_uid", 0);
            std::string key = std::to_string(uid);
            it["del_owner_name"] =
                uid == 0
                    ? "系统"
                    : (names.contains(key) ? names[key].get<std::string>() : ("用户 " + key));
            int64_t     sp   = zm_file_row_int(it, "space", 0);
            std::string skey = std::to_string(sp);
            it["space_name"] = sp == 0 ? "公共空间"
                                       : (names.contains(skey) ? names[skey].get<std::string>()
                                                               : ("用户 " + skey));
        }
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTrashRestore(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZmOpCtx ctx;
    ctx.uid     = gate.ctx.uid;
    ctx.account = gate.ctx.account;
    ctx.ip      = ZmAuthGateModule::ClientIp(req);
    ZMJSON out  = co_await m_node->Restore(ids, ctx, true);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTrashPurge(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON               body = ParseBody(req);
    std::vector<int64_t> ids  = BodyIds(body);
    if (ids.empty())
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "未指定条目");
    ZmOpCtx ctx;
    ctx.uid     = gate.ctx.uid;
    ctx.account = gate.ctx.account;
    ctx.ip      = ZmAuthGateModule::ClientIp(req);
    ZMJSON out  = co_await m_node->Purge(ids, ctx, true);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTrashClean(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = body.contains("space") ? zm_file_row_int(body, "space", -1) : -1;
    // 立即执行保留期清理(等价于把每日清理项提前跑一次)
    ZMJSON out = co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space]() -> ZMJSON
        {
            int64_t before = ZmSqliteDb::Now() - zm_file::kTrashRetainDays * 86400;
            if (space >= 0)
            {
                ZMJSON rows =
                    m_db->QueryRowsSync("SELECT id FROM nodes WHERE deleted = 1 AND space = "
                                        "?1 AND delete_time > 0 "
                                        "AND delete_time < ?2 LIMIT 5000",
                                        {std::to_string(space), std::to_string(before)});
                std::vector<int64_t> ids;
                for (const auto& r : rows)
                    ids.push_back(zm_file_row_int(r, "id", 0));
                ZmOpCtx sys;
                sys.uid       = 0;
                sys.account   = "system";
                ZMJSON agg    = ZMJSON::object();
                agg["purged"] = 0;
                agg["bytes"]  = 0;
                if (!ids.empty())
                {
                    ZMJSON res    = m_node->PurgeIdsSync(ids, sys, true);
                    agg["purged"] = static_cast<int64_t>(res["success"].size());
                    agg["bytes"]  = zm_file_row_int(res, "bytes", 0);
                }
                return agg;
            }
            ZMJSON r    = m_node->PurgeExpiredSync(before);
            ZMJSON o    = ZMJSON::object();
            o["purged"] = zm_file_row_int(r, "purged", 0);
            o["bytes"]  = zm_file_row_int(r, "bytes", 0);
            return o;
        });
    co_return Respond(out);
}

// ============================================================================
// 任务 / 日志 / 空间
// ============================================================================
drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTrashClearAll(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON  body  = ParseBody(req);
    int64_t space = body.contains("space") ? zm_file_row_int(body, "space", -1) : -1;
    ZmOpCtx ctx;
    ctx.uid     = gate.ctx.uid;
    ctx.account = gate.ctx.account;
    ctx.ip      = ZmAuthGateModule::ClientIp(req);
    // 整体清空:不分保留期,回收站里的条目一次全部物理删除(管理端专属能力)
    ZMJSON out = co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, space, ctx]() -> ZMJSON
        {
            std::string where = " WHERE deleted = 1";
            std::vector<std::string> params;
            if (space >= 0)
            {
                where += " AND space = ?1";
                params.push_back(std::to_string(space));
            }
            ZMJSON rows = m_db->QueryRowsSync(
                "SELECT id FROM nodes" + where + " ORDER BY id ASC LIMIT 5000", params);
            std::vector<int64_t> ids;
            for (const auto& r : rows)
                ids.push_back(zm_file_row_int(r, "id", 0));
            ZMJSON o      = ZMJSON::object();
            o["purged"]   = 0;
            o["bytes"]    = 0;
            o["failed"]   = ZMJSON::array();
            if (ids.empty())
                return o;
            ZMJSON res  = m_node->PurgeIdsSync(ids, ctx, true);
            o["purged"] = static_cast<int64_t>(res["success"].size());
            o["bytes"]  = zm_file_row_int(res, "bytes", 0);
            o["failed"] = res.contains("failed") ? res["failed"] : ZMJSON::array();
            // 整体清空单独记一条审计(逐条的 purge 之外,便于回溯"谁清空了回收站")
            if (zm_file_row_int(o, "purged", 0) > 0)
            {
                ZMJSON detail  = ZMJSON::object();
                detail["purged"] = zm_file_row_int(o, "purged", 0);
                detail["bytes"]  = zm_file_row_int(o, "bytes", 0);
                detail["scope"]  = space < 0 ? "全部空间" : std::to_string(space);
                m_db->WithTxSync(
                    [&](ZmSqliteDb& db) -> bool
                    {
                        return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account,
                                                         zm_file::kActTrashClear, space, 0,
                                                         "回收站整体清空", detail.dump(),
                                                         ctx.ip, 1);
                    });
            }
            return o;
        });
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTasks(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t uid    = QueryI64(req, "uid", 0);
    int     type   = QueryInt(req, "type", 0, 0, 99);
    int     status = QueryInt(req, "status", 0, 0, 99);
    int     page   = QueryInt(req, "page", 1, 1, 1000000);
    int     size   = QueryInt(req, "size", 50, 1, 500);
    ZMJSON  out    = co_await m_task->AdminList(uid, type, status, page, size);
    if (!ZmFileHasError(out))
    {
        std::vector<int64_t> uids;
        for (const auto& it : out["list"])
        {
            int64_t u = zm_file_row_int(it, "uid", 0);
            if (u > 0)
                uids.push_back(u);
        }
        ZMJSON names = co_await m_user->GetNicknames(uids);
        for (auto& it : out["list"])
        {
            std::string key = std::to_string(zm_file_row_int(it, "uid", 0));
            it["uname"] =
                names.contains(key) ? names[key].get<std::string>() : ("用户 " + key);
        }
    }
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleTaskCancel(HttpRequestPtr req,
                                                                  std::string    taskNo)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZMJSON out = co_await m_task->Cancel(taskNo, gate.ctx.uid, true);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleLogs(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZmFileLogQuery q;
    q.uid       = QueryI64(req, "uid", 0);
    q.action    = req->getParameter("action");
    q.space     = req->getParameter("space").empty() ? -1 : QueryI64(req, "space", -1);
    q.from      = QueryI64(req, "from", 0);
    q.to        = QueryI64(req, "to", 0);
    q.keyword   = req->getParameter("keyword");
    int    page = QueryInt(req, "page", 1, 1, 1000000);
    int    size = QueryInt(req, "size", 20, 1, 500);
    ZMJSON out  = co_await m_audit->QueryFileLogs(q, page, size);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleShareLogs(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    ZmShareLogQuery q;
    q.token     = req->getParameter("token");
    q.from      = QueryI64(req, "from", 0);
    q.to        = QueryI64(req, "to", 0);
    int    page = QueryInt(req, "page", 1, 1, 1000000);
    int    size = QueryInt(req, "size", 20, 1, 500);
    ZMJSON out  = co_await m_audit->QueryShareLogs(q, page, size);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleSpaces(HttpRequestPtr req)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int                  page = QueryInt(req, "page", 1, 1, 1000000);
    int                  size = QueryInt(req, "size", 50, 1, 500);
    ZMJSON               rows = co_await m_db->ListSpaces();
    std::vector<int64_t> uids;
    for (const auto& r : rows)
    {
        int64_t sp = zm_file_row_int(r, "space", 0);
        if (sp > 0)
            uids.push_back(sp);
    }
    ZMJSON  names = co_await m_user->GetNicknames(uids);
    ZMJSON  list  = ZMJSON::array();
    int64_t total = static_cast<int64_t>(rows.size());
    int64_t from  = static_cast<int64_t>(page - 1) * size;
    for (int64_t i = from; i < total && static_cast<int>(list.size()) < size; ++i)
    {
        const ZMJSON& r    = rows[static_cast<size_t>(i)];
        int64_t       sp   = zm_file_row_int(r, "space", 0);
        ZMJSON        item = ZMJSON::object();
        item["space"]      = sp;
        std::string key    = std::to_string(sp);
        item["name"] =
            sp == 0 ? "公共空间"
                    : (names.contains(key) ? names[key].get<std::string>() : ("用户 " + key));
        item["quota"]      = zm_file_row_int(r, "quota", 0);
        item["used_size"]  = zm_file_row_int(r, "used_size", 0);
        item["used_items"] = zm_file_row_int(r, "used_items", 0);
        list.push_back(std::move(item));
    }
    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return Respond(out);
}

drogon::Task<HttpResponsePtr> ZmFileAdminModule::HandleSetQuota(HttpRequestPtr req,
                                                                std::string    spaceStr)
{
    auto gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);
    int64_t space = 0;
    try
    {
        space = std::stoll(spaceStr);
    }
    catch (...)
    {
        co_return ZmAuthGateModule::ApiError(400, zm_file_err::kBadRequest, "空间参数非法");
    }
    ZMJSON  body  = ParseBody(req);
    int64_t quota = zm_file_row_int(body, "quota", 0);
    if (quota < 0)
        quota = 0;
    bool ok = co_await m_db->SetQuota(space, quota);
    if (!ok)
        co_return ZmAuthGateModule::ApiError(500, zm_file_err::kInternal, "设置配额失败");
    co_return Respond(ZMJSON::object());
}
