#include "modules/module_file_task.h"

#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

using namespace drogon;

namespace
{
/// 进度落库的节流参数
constexpr int64_t kProgressMinIntervalMs = 500; ///< 距上次写库的最小间隔
constexpr int64_t kProgressMinPercent    = 5;   ///< 完成度跨多少个百分点必须写库

/// @return 当前毫秒时刻(单调)
int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// @return 32 位十六进制随机串(对外任务号)
std::string RandomToken()
{
    static const char*                 hex = "0123456789abcdef";
    std::random_device                 rd;
    std::mt19937_64                    gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string                        s;
    s.reserve(32);
    for (int i = 0; i < 32; ++i)
        s += hex[dist(gen)];
    return s;
}
} // namespace

// ============================================================================
// ZmTaskHandle
// ============================================================================
ZmTaskHandle::ZmTaskHandle(ZmFileTaskModule* owner, std::string taskNo, int64_t uid,
                           int64_t space)
    : m_owner(owner), m_taskNo(std::move(taskNo)), m_uid(uid), m_space(space)
{
    m_lastWriteMs = NowMs();
}

bool ZmTaskHandle::Cancelled() const
{
    return m_owner ? m_owner->IsCancelled(m_taskNo) : false;
}

void ZmTaskHandle::Progress(int64_t doneSize, int64_t doneItems)
{
    if (!m_owner)
        return;
    // 节流:避免高频 UPDATE 打满单写队列
    int64_t nowMs = NowMs();
    int64_t pct   = -1;
    if (doneItems > 0)
        pct = doneItems; // 条目型任务用条目数近似完成度,由调用方保证单调递增
    bool timeHit = (nowMs - m_lastWriteMs) >= kProgressMinIntervalMs;
    bool pctHit  = (pct >= 0 && m_lastPct >= 0 && (pct - m_lastPct) >= kProgressMinPercent);
    if (!timeHit && !pctHit)
        return;
    if (m_owner->UpdateProgressSync(m_taskNo, doneSize, doneItems))
    {
        m_lastWriteMs = nowMs;
        m_lastPct     = pct < 0 ? m_lastPct : pct;
    }
}

void ZmTaskHandle::Finish(int status, const std::string& error, const std::string& result)
{
    if (!m_owner)
        return;
    m_owner->SetStatusSync(m_taskNo, status, error, result);
    m_owner->ClearCancel(m_taskNo);
}

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileTaskModule::ZmFileTaskModule(ZmFileDbModule* db) : m_db(db) {}

ZmFileTaskModule::~ZmFileTaskModule() = default;

// ============================================================================
// 启动期僵尸任务清理
// ============================================================================
void ZmFileTaskModule::MarkZombieTasks()
{
    if (!m_db || !m_db->IsReady())
        return;
    // 服务上次退出时"进行中"的任务不可能还在跑:统一置已中断,供用户重试
    m_db->ExecSync("UPDATE transfer_tasks SET status = ?1, end_time = ?2, "
                   "error = '服务重启中断' WHERE status = ?3",
                   {std::to_string(zm_file::kTaskInterrupted),
                    std::to_string(ZmSqliteDb::Now()), std::to_string(zm_file::kTaskRunning)});
    DEFAULT_LOG_INFO("ZmFileTaskModule: 僵尸任务已置为已中断");
}

// ============================================================================
// 创建 / 启动
// ============================================================================
bool ZmFileTaskModule::CreateSync(int type, int64_t uid, int64_t space,
                                  const std::string& name, const std::string& target,
                                  int64_t size, int64_t totalItems, const std::string& refId,
                                  std::string& taskNoOut)
{
    taskNoOut   = RandomToken();
    int64_t now = ZmSqliteDb::Now();
    if (!m_db->ExecSync(
            "INSERT INTO transfer_tasks(task_no,type,uid,space,name,target,size,"
            "done_size,total_items,done_items,status,error,result,ref_id,create_time,"
            "start_time,end_time) VALUES(?1,?2,?3,?4,?5,?6,?7,0,?8,0,?9,'','',?10,?11,0,0)",
            {taskNoOut, std::to_string(type), std::to_string(uid), std::to_string(space), name,
             target, std::to_string(size), std::to_string(totalItems),
             std::to_string(zm_file::kTaskQueued), refId, std::to_string(now)}))
    {
        taskNoOut.clear();
        return false;
    }
    return true;
}

drogon::Task<ZMJSON> ZmFileTaskModule::Create(int type, int64_t uid, int64_t space,
                                              const std::string& name,
                                              const std::string& target, int64_t size,
                                              int64_t totalItems, const std::string& refId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, type, uid, space, name, target, size, totalItems, refId]() -> ZMJSON
        {
            ZMJSON      out = ZMJSON::object();
            std::string taskNo;
            if (!CreateSync(type, uid, space, name, target, size, totalItems, refId, taskNo))
                return out;
            out["task_no"] = taskNo;
            return out;
        });
}

drogon::Task<ZMJSON> ZmFileTaskModule::Start(int type, int64_t uid, int64_t space,
                                             const std::string& name,
                                             const std::string& target, int64_t size,
                                             int64_t totalItems, const std::string& refId,
                                             std::function<void(ZmTaskHandle&)> body)
{
    ZMJSON created = co_await Create(type, uid, space, name, target, size, totalItems, refId);
    std::string taskNo = zm_file_row_str(created, "task_no");
    if (taskNo.empty())
        co_return ZmFileError(zm_file_err::kInternal, 500, "任务创建失败");
    // 执行体投到工作池:内部只允许 *Sync 调用,不得 co_await
    ZmHttpServer::WorkPool().Submit(
        [this, taskNo, uid, space, body]()
        {
            SetStatusSync(taskNo, zm_file::kTaskRunning);
            ZmTaskHandle handle(this, taskNo, uid, space);
            try
            {
                body(handle);
            }
            catch (const std::exception& e)
            {
                DEFAULT_LOG_ERROR("任务 {} 执行异常: {}", taskNo, e.what());
                handle.Finish(zm_file::kTaskFailed, std::string("内部错误:") + e.what());
            }
            catch (...)
            {
                handle.Finish(zm_file::kTaskFailed, "内部错误");
            }
            // 执行体未收尾(忘记 Finish)时兜底置失败,避免任务永远"进行中"
            ZMJSON row = TaskRowSync(taskNo);
            if (zm_file_row_int(row, "status", 0) == zm_file::kTaskRunning)
                handle.Finish(zm_file::kTaskFailed, "任务未正常结束");
        });
    ZMJSON out     = ZMJSON::object();
    out["task_no"] = taskNo;
    co_return out;
}

// ============================================================================
// 查询
// ============================================================================
ZMJSON ZmFileTaskModule::TaskView(const ZMJSON& row)
{
    ZMJSON j         = ZMJSON::object();
    j["task_no"]     = zm_file_row_str(row, "task_no");
    j["type"]        = zm_file_row_int(row, "type", 0);
    j["uid"]         = zm_file_row_int(row, "uid", 0);
    j["space"]       = zm_file_row_int(row, "space", 0);
    j["name"]        = zm_file_row_str(row, "name");
    j["target"]      = zm_file_row_str(row, "target");
    j["size"]        = zm_file_row_int(row, "size", 0);
    j["done_size"]   = zm_file_row_int(row, "done_size", 0);
    j["total_items"] = zm_file_row_int(row, "total_items", 0);
    j["done_items"]  = zm_file_row_int(row, "done_items", 0);
    j["status"]      = zm_file_row_int(row, "status", 0);
    j["error"]       = zm_file_row_str(row, "error");
    j["result"]      = zm_file_row_str(row, "result");
    j["ref_id"]      = zm_file_row_str(row, "ref_id");
    j["create_time"] = zm_file_row_int(row, "create_time", 0);
    j["start_time"]  = zm_file_row_int(row, "start_time", 0);
    j["end_time"]    = zm_file_row_int(row, "end_time", 0);
    return j;
}

drogon::Task<ZMJSON> ZmFileTaskModule::List(int64_t uid, int type, int status, int page,
                                            int size)
{
    std::string              where = " WHERE uid = ?1";
    std::vector<std::string> p     = {std::to_string(uid)};
    if (type > 0)
    {
        where += " AND type = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(type));
    }
    if (status > 0)
    {
        where += " AND status = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(status));
    }
    ZMJSON totalRow =
        co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM transfer_tasks" + where, p);
    int64_t                  total = zm_file_row_int(totalRow, "n", 0);
    std::vector<std::string> lp    = p;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string((page - 1) * size));
    ZMJSON rows = co_await m_db->QueryRows(
        "SELECT * FROM transfer_tasks" + where + " ORDER BY id DESC LIMIT ?" +
            std::to_string(lp.size() - 1) + " OFFSET ?" + std::to_string(lp.size()),
        lp);
    ZMJSON list = ZMJSON::array();
    for (const auto& r : rows)
        list.push_back(TaskView(r));
    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return out;
}

drogon::Task<ZMJSON> ZmFileTaskModule::AdminList(int64_t uid, int type, int status, int page,
                                                 int size)
{
    std::string              where = " WHERE 1=1";
    std::vector<std::string> p;
    if (uid > 0)
    {
        where += " AND uid = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(uid));
    }
    if (type > 0)
    {
        where += " AND type = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(type));
    }
    if (status > 0)
    {
        where += " AND status = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(status));
    }
    ZMJSON totalRow =
        co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM transfer_tasks" + where, p);
    int64_t                  total = zm_file_row_int(totalRow, "n", 0);
    std::vector<std::string> lp    = p;
    lp.push_back(std::to_string(size));
    lp.push_back(std::to_string((page - 1) * size));
    ZMJSON rows = co_await m_db->QueryRows(
        "SELECT * FROM transfer_tasks" + where + " ORDER BY id DESC LIMIT ?" +
            std::to_string(lp.size() - 1) + " OFFSET ?" + std::to_string(lp.size()),
        lp);
    ZMJSON list = ZMJSON::array();
    for (const auto& r : rows)
        list.push_back(TaskView(r));
    ZMJSON out   = ZMJSON::object();
    out["total"] = total;
    out["page"]  = page;
    out["size"]  = size;
    out["list"]  = std::move(list);
    co_return out;
}

drogon::Task<ZMJSON> ZmFileTaskModule::Active(int64_t uid, bool adminAll)
{
    std::string sql =
        "SELECT task_no,type,name,status,done_size,size,done_items,total_items FROM "
        "transfer_tasks WHERE status IN (?1,?2)";
    std::vector<std::string> p = {std::to_string(zm_file::kTaskQueued),
                                  std::to_string(zm_file::kTaskRunning)};
    if (!adminAll)
    {
        sql += " AND uid = ?3";
        p.push_back(std::to_string(uid));
    }
    sql += " ORDER BY id ASC";
    co_return co_await m_db->QueryRows(sql, p);
}

drogon::Task<ZMJSON> ZmFileTaskModule::Detail(const std::string& taskNo, int64_t uid,
                                              bool adminAll)
{
    ZMJSON row =
        co_await m_db->QueryRow("SELECT * FROM transfer_tasks WHERE task_no = ?1", {taskNo});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    if (!adminAll && zm_file_row_int(row, "uid", 0) != uid)
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    co_return TaskView(row);
}

// ============================================================================
// 操作
// ============================================================================
drogon::Task<ZMJSON> ZmFileTaskModule::Cancel(const std::string& taskNo, int64_t uid,
                                              bool adminAll)
{
    ZMJSON row =
        co_await m_db->QueryRow("SELECT * FROM transfer_tasks WHERE task_no = ?1", {taskNo});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    if (!adminAll && zm_file_row_int(row, "uid", 0) != uid)
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    int status = static_cast<int>(zm_file_row_int(row, "status", 0));
    if (status != zm_file::kTaskQueued && status != zm_file::kTaskRunning)
        co_return ZMJSON::object(); // 终态任务取消是幂等的

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cancelFlags[taskNo] = true;
    }
    if (status == zm_file::kTaskQueued || adminAll)
    {
        // 排队中的任务没有执行体，可直接置位；管理端强制取消也直接置位
        co_await ZmHttpServer::RunOnPool<bool>(
            [this, taskNo]() -> bool
            { return SetStatusSync(taskNo, zm_file::kTaskCanceled, "已取消"); });
    }
    co_return ZMJSON::object();
}

drogon::Task<ZMJSON> ZmFileTaskModule::Clear(int64_t uid, int status, bool adminAll)
{
    std::string              where = " WHERE status >= ?1";
    std::vector<std::string> p     = {std::to_string(zm_file::kTaskDone)};
    if (status > 0)
    {
        where += " AND status = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(status));
    }
    if (!adminAll)
    {
        where += " AND uid = ?" + std::to_string(p.size() + 1);
        p.push_back(std::to_string(uid));
    }
    ZMJSON cntRow =
        co_await m_db->QueryRow("SELECT COUNT(*) AS n FROM transfer_tasks" + where, p);
    int64_t cleared = zm_file_row_int(cntRow, "n", 0);
    if (cleared > 0)
        co_await m_db->Exec("DELETE FROM transfer_tasks" + where, p);
    ZMJSON out     = ZMJSON::object();
    out["cleared"] = cleared;
    co_return out;
}

drogon::Task<ZMJSON> ZmFileTaskModule::Retry(const std::string& taskNo, int64_t uid)
{
    ZMJSON row =
        co_await m_db->QueryRow("SELECT * FROM transfer_tasks WHERE task_no = ?1", {taskNo});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    if (zm_file_row_int(row, "uid", 0) != uid)
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    int status = static_cast<int>(zm_file_row_int(row, "status", 0));
    if (status != zm_file::kTaskFailed && status != zm_file::kTaskInterrupted &&
        status != zm_file::kTaskCanceled)
        co_return ZmFileError(zm_file_err::kBadRequest, 400,
                              "仅失败/已中断/已取消的任务可重试");
    int type = static_cast<int>(zm_file_row_int(row, "type", 0));

    RetryExec exec;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto                        it = m_retryExecs.find(type);
        if (it != m_retryExecs.end())
            exec = it->second;
    }
    if (!exec)
        co_return ZmFileError(zm_file_err::kBadRequest, 400,
                              "该任务不支持重试,请在原位置重新发起");
    co_return co_await exec(row);
}

void ZmFileTaskModule::RegisterRetryExec(int type, RetryExec exec)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_retryExecs[type] = std::move(exec);
}

// ============================================================================
// 同步接口(执行体 / 句柄)
// ============================================================================
ZMJSON ZmFileTaskModule::TaskRowSync(const std::string& taskNo)
{
    return m_db->QueryRowSync("SELECT * FROM transfer_tasks WHERE task_no = ?1", {taskNo});
}

bool ZmFileTaskModule::UpdateProgressSync(const std::string& taskNo, int64_t doneSize,
                                          int64_t doneItems)
{
    return m_db->ExecSync("UPDATE transfer_tasks SET done_size = ?1, done_items = ?2 "
                          "WHERE task_no = ?3 AND status = ?4",
                          {std::to_string(doneSize), std::to_string(doneItems), taskNo,
                           std::to_string(zm_file::kTaskRunning)});
}

bool ZmFileTaskModule::SetStatusSync(const std::string& taskNo, int status,
                                     const std::string& error, const std::string& result)
{
    int64_t now = ZmSqliteDb::Now();
    // 进入"进行中"补记开始时间;进入终态补记结束时间
    return m_db->ExecSync(
        "UPDATE transfer_tasks SET status = ?1, error = ?2, result = ?3, "
        "start_time = CASE WHEN ?1 = ?4 AND start_time = 0 THEN ?6 ELSE start_time END, "
        "end_time = CASE WHEN ?1 >= ?5 THEN ?6 ELSE end_time END WHERE task_no = ?7",
        {std::to_string(status), error, result, std::to_string(zm_file::kTaskRunning),
         std::to_string(zm_file::kTaskDone), std::to_string(now), taskNo});
}

bool ZmFileTaskModule::IsCancelled(const std::string& taskNo) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto                        it = m_cancelFlags.find(taskNo);
    return it != m_cancelFlags.end() && it->second;
}

void ZmFileTaskModule::ClearCancel(const std::string& taskNo)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cancelFlags.erase(taskNo);
}

int64_t ZmFileTaskModule::CountRunningUploadsSync(int64_t uid)
{
    ZMJSON row = m_db->QueryRowSync(
        "SELECT COUNT(*) AS n FROM transfer_tasks WHERE uid = ?1 AND type = ?2 "
        "AND status IN (?3,?4)",
        {std::to_string(uid), std::to_string(zm_file::kTaskUpload),
         std::to_string(zm_file::kTaskQueued), std::to_string(zm_file::kTaskRunning)});
    return zm_file_row_int(row, "n", 0);
}

int64_t ZmFileTaskModule::CountRunningPacksSync(int64_t uid)
{
    ZMJSON row = m_db->QueryRowSync(
        "SELECT COUNT(*) AS n FROM transfer_tasks WHERE uid = ?1 AND type = ?2 "
        "AND status IN (?3,?4)",
        {std::to_string(uid), std::to_string(zm_file::kTaskPack),
         std::to_string(zm_file::kTaskQueued), std::to_string(zm_file::kTaskRunning)});
    return zm_file_row_int(row, "n", 0);
}

bool ZmFileTaskModule::TryAcquirePackSlot(const std::string& taskNo)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_packRunning.count(taskNo))
        return true;
    if (static_cast<int>(m_packRunning.size()) >= kPackSlots)
        return false;
    m_packRunning[taskNo] = NowMs();
    return true;
}

void ZmFileTaskModule::ReleasePackSlot(const std::string& taskNo)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_packRunning.erase(taskNo);
}

bool ZmFileTaskModule::WaitPackSlot(const std::string& taskNo, ZmTaskHandle* handle)
{
    // 打包吃 CPU 与磁盘 IO:全局同时 ≤2,超出排队(每 200ms 重试一次)
    for (int i = 0; i < 1500; ++i) // 最长等 5 分钟
    {
        if (TryAcquirePackSlot(taskNo))
            return true;
        if (handle && handle->Cancelled())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

void ZmFileTaskModule::SetRetryPayload(const std::string& taskNo, const ZMJSON& payload)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_retryPayload.size() > 2000)
        m_retryPayload.clear(); // 简单上限保护:进程内登记不值得做 LRU
    m_retryPayload[taskNo] = payload;
}

ZMJSON ZmFileTaskModule::GetRetryPayload(const std::string& taskNo)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto                        it = m_retryPayload.find(taskNo);
    return it == m_retryPayload.end() ? ZMJSON::object() : it->second;
}

void ZmFileTaskModule::DropRetryPayload(const std::string& taskNo)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_retryPayload.erase(taskNo);
}
