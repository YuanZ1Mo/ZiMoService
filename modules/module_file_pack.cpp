#include "modules/module_file_pack.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <openssl/evp.h>

#include <minizip-ng/mz.h>
#include <minizip-ng/mz_zip.h>
#include <minizip-ng/mz_os.h>
#include <minizip-ng/mz_strm.h>
#include <minizip-ng/mz_zip_rw.h>

#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"
#include "modules/module_file_token.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <ctime>
#include <random>
#include <set>

using namespace drogon;

namespace
{
/// 已压缩格式:只封装不再压缩(显著提速)
bool IsPrecompressed(const std::string& ext)
{
    static const std::set<std::string> kSet = {
        "zip", "gz",   "7z",   "rar",  "jpg",  "jpeg", "png", "gif", "webp", "mp4", "mov",
        "mp3", "flac", "docx", "xlsx", "pptx", "bz2",  "xz",  "zst", "mkv",  "avi", "webm"};
    return kSet.count(ext) > 0;
}

/// 打包上下文(工作池线程内传递)
struct PackCtx
{
    void*              zip        = nullptr;
    ZmFileStoreModule* store      = nullptr;
    ZmTaskHandle*      handle     = nullptr;
    int64_t            doneItems  = 0;
    int64_t            doneBytes  = 0;
    int64_t            totalItems = 0;
    bool               cancelled  = false;
    std::string        error;
};

/**
 * @brief 递归把一个物理条目写入 zip
 *
 * @param c 打包上下文
 * @param physPath 源物理路径
 * @param entryName zip 内条目名(以 / 分隔)
 * @param depth 已下钻层数(超过 kMaxTreeDepth 即中止,防目录联接成环)
 * @return true 成功;false 失败(原因写 c.error)
 */
bool AddPathToZip(PackCtx& c, const std::string& physPath, const std::string& entryName,
                  int depth)
{
    // 物理递归带深度上限:目录联接成环时打包会无限展开,必须到此为止并如实报错
    if (depth > zm_file::kMaxTreeDepth)
    {
        c.error = "目录层级过深(超过 64 层),可能存在目录联接成环,已中止打包";
        return false;
    }
    if (c.handle && c.handle->Cancelled())
    {
        c.cancelled = true;
        return false;
    }
    ZmDiskEntry   st;
    ZmStoreResult sr = c.store->StatSync(physPath, st);
    if (!sr.ok)
    {
        // 条目在打包过程中被删除:跳过并继续
        DEFAULT_LOG_WARN("打包跳过不可读条目: {}", physPath);
        return true;
    }
    if (!st.isDir)
    {
        std::string ext;
        size_t      dot = entryName.rfind('.');
        if (dot != std::string::npos && dot + 1 < entryName.size())
        {
            ext = entryName.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(::tolower(ch)); });
        }
        mz_zip_writer_set_compress_method(c.zip, IsPrecompressed(ext)
                                                     ? MZ_COMPRESS_METHOD_STORE
                                                     : MZ_COMPRESS_METHOD_DEFLATE);
        mz_zip_writer_set_compress_level(c.zip, 1);
        if (mz_zip_writer_add_file(c.zip, physPath.c_str(), entryName.c_str()) != MZ_OK)
        {
            c.error = "写入压缩包失败: " + entryName;
            return false;
        }
        ++c.doneItems;
        c.doneBytes += st.size;
        if (c.handle && (c.doneItems % 20) == 0)
            c.handle->Progress(c.doneBytes, c.doneItems);
        return true;
    }

    std::vector<ZmDiskEntry> children;
    ZmStoreResult            ls = c.store->ScanDirSync(physPath, children);
    if (!ls.ok)
    {
        DEFAULT_LOG_WARN("打包跳过不可读目录: {}", physPath);
        return true;
    }
    if (children.empty())
    {
        // 空目录也要保留层级:写一个目录条目
        mz_zip_file info{};
        std::string dirName     = entryName + "/";
        info.version_madeby     = MZ_VERSION_MADEBY;
        info.compression_method = MZ_COMPRESS_METHOD_STORE;
        info.flag               = MZ_ZIP_FLAG_UTF8;
        info.filename           = dirName.c_str();
        info.external_fa        = 0x41FF0010; // 目录属性(与 Windows 资源管理器一致)
        // 目录条目无数据体:必须走 add_info(stream=nullptr);add_buffer 在
        // minizip-ng 4.x 里对 nullptr buf 直接返回 MZ_PARAM_ERROR,会让整个打包失败
        if (mz_zip_writer_add_info(c.zip, nullptr, nullptr, &info) != MZ_OK)
        {
            c.error = "写入空目录失败: " + dirName;
            return false;
        }
        ++c.doneItems;
        return true;
    }
    for (const auto& ch : children)
    {
        if (!AddPathToZip(c, physPath + "\\" + ch.name, entryName + "/" + ch.name, depth + 1))
            return false;
    }
    return true;
}

/// 时间戳串 yyyyMMddHHmmss
std::string Stamp14()
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/// 随机小写字母数字串
std::string RandStr(int n)
{
    static const char*                 cs = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device                 rd;
    std::mt19937                       gen(rd());
    std::uniform_int_distribution<int> dist(0, 35);
    std::string                        s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        s += cs[dist(gen)];
    return s;
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFilePackModule::ZmFilePackModule(ZmFileDbModule* db, ZmFileStoreModule* store,
                                   ZmFileNodeModule* node, ZmFileTaskModule* task,
                                   ZmFileAuditModule* audit, ZmFileTokenModule* token)
    : m_db(db), m_store(store), m_node(node), m_task(task), m_audit(audit), m_token(token)
{
}

ZmFilePackModule::~ZmFilePackModule() = default;

// ============================================================================
// 命名与位置
// ============================================================================
std::string ZmFilePackModule::ZipPath(int64_t space, const std::string& zipName) const
{
    return m_store->ZipDir(space) + "\\" + zipName;
}

std::string ZmFilePackModule::CacheFileName(int64_t uid)
{
    return std::to_string(uid) + "_" + Stamp14() + "_" + RandStr(6) + ".zip";
}

std::string ZmFilePackModule::DisplayName(const ZMJSON& firstNode, size_t count)
{
    if (count == 1 && firstNode.is_object())
    {
        std::string n = zm_file_row_str(firstNode, "name");
        if (!n.empty())
            return n + ".zip";
    }
    return "打包下载_" + Stamp14() + ".zip";
}

// ============================================================================
// 创建打包任务
// ============================================================================
std::string ZmFilePackModule::DedupeHash(const std::string& key)
{
    if (key.empty())
        return "";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLen = 0;
    if (EVP_Digest(key.data(), key.size(), md, &mdLen, EVP_sha256(), nullptr) != 1)
        return "";
    static const char* hex = "0123456789abcdef";
    std::string        s;
    s.reserve(32);
    for (unsigned int i = 0; i < mdLen && s.size() < 32; ++i)
    {
        s += hex[md[i] >> 4];
        s += hex[md[i] & 0x0F];
    }
    return s;
}

drogon::Task<ZMJSON> ZmFilePackModule::Create(int64_t space, const std::vector<int64_t>& ids,
                                              const ZmOpCtx& ctx, const std::string& dedupeKey)
{
    // 上限前置:条目数 ≤5000、总字节 ≤20GB,超出直接拒,不进入排队
    int64_t              items = 0;
    int64_t              bytes = 0;
    ZMJSON               firstNode;
    std::vector<int64_t> uniq;
    for (int64_t id : ids)
    {
        if (id <= 0 || std::find(uniq.begin(), uniq.end(), id) != uniq.end())
            continue;
        uniq.push_back(id);
    }
    if (uniq.empty())
        co_return ZmFileError(zm_file_err::kBadRequest, 400, "未指定待打包条目");

    int64_t uid = ctx.uid;
    for (int64_t id : uniq)
    {
        // 逐条可见性 + 归属 + 子树规模(含目录在内的条目数,打包按条目推进)
        ZMJSON stat = co_await ZmHttpServer::RunOnPool<ZMJSON>(
            [this, id, uid]() -> ZMJSON
            {
                ZMJSON row;
                ZMJSON out = ZMJSON::object();
                if (!m_node->VisibleSync(id, row))
                {
                    out["ok"] = false;
                    return out;
                }
                // 逐条校验归属:入参 space 只是客户端的说法,条目真实空间才作数
                if (!ZmFileNodeModule::SpaceWritable(zm_file_row_int(row, "space", 0), uid))
                {
                    out["ok"] = false;
                    return out;
                }
                out["ok"]    = true;
                out["node"]  = row;
                int64_t by   = 0;
                int64_t it   = 0;
                ZMJSON  sub  = m_node->SubtreeIdsSync(id, &by, &it);
                out["items"] = static_cast<int64_t>(sub.size());
                out["bytes"] = by;
                return out;
            });
        if (!zm_json_get_bool(stat, "ok", false))
            co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "条目不存在");
        if (firstNode.empty())
            firstNode = stat["node"];
        items += zm_file_row_int(stat, "items", 0);
        bytes += zm_file_row_int(stat, "bytes", 0);
    }
    if (items > zm_file::kMaxPackItems || bytes > zm_file::kMaxPackBytes)
        co_return ZmFileError(zm_file_err::kPackTooLarge, 400,
                              "打包超出条目数或大小上限,请分批下载");

    // 单用户进行中打包任务 ≤2(§3.11):超出直接 429,不进入排队。
    // 与 RunPack 里"全局同时 ≤2"的闸门是两回事:那个是排队等待,这个是拒绝
    if (m_task->CountRunningPacksSync(ctx.uid) >= zm_file::kPackMaxRunning)
        co_return ZmFileError(zm_file_err::kTooManyPacks, 429,
                              "进行中的打包任务已达上限,请等前一个完成");

    std::string display = DisplayName(firstNode, uniq.size());
    // 幂等键记进 ref_id:分享页对同一批条目的重复请求据此复用任务(不落进程内表,重启也在)
    std::string refId   = DedupeHash(dedupeKey);
    ZMJSON      task = co_await m_task->Create(zm_file::kTaskPack, ctx.uid, space, display, "",
                                               bytes, items, refId);
    std::string taskNo = zm_file_row_str(task, "task_no");
    if (taskNo.empty())
        co_return ZmFileError(zm_file_err::kInternal, 500, "创建打包任务失败");
    // 重试输入(进程内;服务重启后重试提示重新发起)
    ZMJSON payload     = ZMJSON::object();
    payload["space"]   = space;
    payload["ids"]     = uniq;
    payload["uid"]     = ctx.uid;
    payload["account"] = ctx.account;
    payload["ip"]      = ctx.ip;
    m_task->SetRetryPayload(taskNo, payload);
    ZmHttpServer::WorkPool().Submit([this, taskNo]() { RunPack(taskNo); });
    ZMJSON out     = ZMJSON::object();
    out["task_no"] = taskNo;
    co_return out;
}

// ============================================================================
// 打包执行体(工作池线程)
// ============================================================================
void ZmFilePackModule::RunPack(const std::string& taskNo)
{
    ZMJSON row = m_task->TaskRowSync(taskNo);
    if (row.empty())
        return;
    int64_t     uid        = zm_file_row_int(row, "uid", 0);
    int64_t     space      = zm_file_row_int(row, "space", 0);
    std::string display    = zm_file_row_str(row, "name");
    int64_t     totalItems = zm_file_row_int(row, "total_items", 0);
    int64_t     totalBytes = zm_file_row_int(row, "size", 0);

    ZmTaskHandle handle(m_task, taskNo, uid, space);
    // 全局同时进行 ≤2:超出排队(打包吃 CPU 与磁盘 IO)
    if (!m_task->WaitPackSlot(taskNo, &handle))
    {
        handle.Finish(zm_file::kTaskCanceled, "已取消");
        return;
    }
    // 闸门拿到后再确认是否已被取消
    if (handle.Cancelled())
    {
        m_task->ReleasePackSlot(taskNo);
        handle.Finish(zm_file::kTaskCanceled, "已取消");
        return;
    }

    ZMJSON               payload = m_task->GetRetryPayload(taskNo);
    std::vector<int64_t> ids;
    if (payload.contains("ids") && payload["ids"].is_array())
    {
        for (const auto& v : payload["ids"])
            ids.push_back(v.get<int64_t>());
    }
    if (ids.empty())
    {
        m_task->ReleasePackSlot(taskNo);
        handle.Finish(zm_file::kTaskFailed, "打包任务输入已失效");
        return;
    }

    // 拿到闸门、开始真正打包时才置"进行中":排队等闸门期间应显示"排队中";
    // 同时"进行中"是进度写库的前置条件(UpdateProgressSync 只更新该状态的行)
    m_task->SetStatusSync(taskNo, zm_file::kTaskRunning);

    m_store->EnsureDir(m_store->ZipDir(space));
    std::string zipName = CacheFileName(uid);
    std::string zipPath = ZipPath(space, zipName);

    // 条目名:单条目用它自己的名字做 zip 根;多条目各自成一级
    struct Entry
    {
        int64_t     id;
        std::string phys;
        std::string entry;
    };
    std::vector<Entry> entries;
    for (int64_t id : ids)
    {
        int64_t     sp = space;
        std::string phys;
        if (!m_store->PhysicalPathSync(id, sp, phys).ok)
            continue;
        ZMJSON nrow = m_db->NodeRowSync(id);
        if (nrow.empty())
            continue;
        std::string name = zm_file_row_str(nrow, "name");
        if (phys.empty())
            continue;
        // phys 是完整路径,截到最后一个分隔符之后即为名字
        entries.push_back({id, phys, name});
    }
    if (entries.empty())
    {
        m_task->ReleasePackSlot(taskNo);
        handle.Finish(zm_file::kTaskFailed, "待打包条目已不存在");
        return;
    }

    void* zip = mz_zip_writer_create();
    if (zip == nullptr)
    {
        m_task->ReleasePackSlot(taskNo);
        handle.Finish(zm_file::kTaskFailed, "压缩组件初始化失败");
        return;
    }
    // 注意:zip64 由 minizip-ng 按条目大小自动启用(MZ_ZIP64_AUTO),
    // 不要调 set_zip_cd —— 那是"把中央目录也压成一个条目",会在包里多出一个
    // 名为 __cdcd__ 的占位条目,Windows 资源管理器会把它当成垃圾文件
    int32_t rc = mz_zip_writer_open_file(zip, zipPath.c_str(), 0, 0);
    if (rc != MZ_OK)
    {
        mz_zip_writer_delete(&zip);
        m_task->ReleasePackSlot(taskNo);
        handle.Finish(zm_file::kTaskFailed, "无法创建压缩包文件");
        return;
    }

    PackCtx c;
    c.zip        = zip;
    c.store      = m_store;
    c.handle     = &handle;
    c.totalItems = totalItems;
    bool ok      = true;
    // 单条目且是目录时:以目录名作为 zip 根的顶级目录,天然由 entry 名体现
    for (const auto& e : entries)
    {
        if (!AddPathToZip(c, e.phys, e.entry, 0))
        {
            ok = false;
            break;
        }
    }
    rc = mz_zip_writer_close(zip);
    mz_zip_writer_delete(&zip);
    m_task->ReleasePackSlot(taskNo);

    if (c.cancelled || handle.Cancelled())
    {
        m_store->RemoveFileSync(zipPath);
        handle.Finish(zm_file::kTaskCanceled, "已取消");
        return;
    }
    if (!ok || rc != MZ_OK)
    {
        m_store->RemoveFileSync(zipPath);
        handle.Finish(zm_file::kTaskFailed,
                      c.error.empty() ? std::string("打包失败") : c.error);
        return;
    }

    int64_t zipSize = m_store->FileSize(zipPath);
    TouchAccess(taskNo);
    // 任务 result 记压缩包名(相对 zip 目录),供后续换取下载令牌
    m_task->UpdateProgressSync(taskNo, totalBytes > 0 ? totalBytes : zipSize, c.doneItems);
    m_db->WithTxSync(
        [&](ZmSqliteDb& db) -> bool
        {
            ZMJSON detail          = ZMJSON::object();
            detail["zip"]          = zipName;
            detail["items"]        = c.doneItems;
            detail["bytes"]        = zipSize;
            detail["source_items"] = totalItems;
            return m_audit->RecordFileOpSync(db, uid, "", zm_file::kActPack, space, 0, display,
                                             detail.dump(), "", 1);
        });
    handle.Finish(zm_file::kTaskDone, "", zipName);
    DEFAULT_LOG_INFO("打包完成: {} 条目={} 大小={}", zipName, c.doneItems, zipSize);
}

// ============================================================================
// 重试
// ============================================================================
drogon::Task<ZMJSON> ZmFilePackModule::Retry(const ZMJSON& oldTask)
{
    std::string oldNo   = zm_file_row_str(oldTask, "task_no");
    ZMJSON      payload = m_task->GetRetryPayload(oldNo);
    if (payload.empty() || !payload.contains("ids"))
        co_return ZmFileError(zm_file_err::kBadRequest, 400,
                              "打包任务的输入已失效,请重新发起下载");
    ZmOpCtx ctx;
    ctx.uid                    = zm_file_row_int(payload, "uid", 0);
    ctx.account                = zm_file_row_str(payload, "account");
    ctx.ip                     = zm_file_row_str(payload, "ip");
    int64_t              space = zm_file_row_int(payload, "space", 0);
    std::vector<int64_t> ids;
    for (const auto& v : payload["ids"])
        ids.push_back(v.get<int64_t>());
    co_return co_await Create(space, ids, ctx);
}

// ============================================================================
// 清理
// ============================================================================
void ZmFilePackModule::TouchAccess(const std::string& taskNo)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_access[taskNo] = ZmSqliteDb::Now();
    }
    // 同时把压缩包的文件时间推到现在:空闲清理以文件时间为准,跨进程重启也成立
    ZMJSON row = m_db->QueryRowSync(
        "SELECT space, result FROM transfer_tasks WHERE task_no = ?1 AND type = ?2",
        {taskNo, std::to_string(zm_file::kTaskPack)});
    if (row.empty())
        return;
    std::string zipName = zm_file_row_str(row, "result");
    if (zipName.empty())
        return;
    m_store->Touch(ZipPath(zm_file_row_int(row, "space", 0), zipName));
}

void ZmFilePackModule::CleanIdle(int64_t now)
{
    if (!m_store)
        return;
    std::vector<ZmDiskEntry> spaces;
    if (!m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces).ok)
        return;
    for (const auto& sp : spaces)
    {
        if (!sp.isDir)
            continue;
        std::string zipDir = m_store->RootDir() + "\\space_cache\\" + sp.name + "\\zip";
        std::vector<ZmDiskEntry> zips;
        if (!m_store->ScanDirSync(zipDir, zips).ok)
            continue;
        for (const auto& z : zips)
        {
            if (z.isDir)
                continue;
            // 空闲阈值:最后一次被访问(下载会把文件时间推到现在)起算
            if (now - z.mtime > zm_file::kCacheIdleSec)
            {
                m_store->RemoveFileSync(zipDir + "\\" + z.name);
                DEFAULT_LOG_INFO("打包缓存空闲回收: {} (空闲 {}s)", z.name, now - z.mtime);
            }
        }
    }
}

drogon::Task<ZMJSON> ZmFilePackModule::CleanTask(const std::string& taskNo, int64_t uid,
                                                 bool adminAll)
{
    ZMJSON row =
        co_await m_db->QueryRow("SELECT * FROM transfer_tasks WHERE task_no = ?1", {taskNo});
    if (row.empty())
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    if (!adminAll && zm_file_row_int(row, "uid", 0) != uid)
        co_return ZmFileError(zm_file_err::kNodeNotFound, 404, "任务不存在");
    std::string zipName = zm_file_row_str(row, "result");
    int64_t     space   = zm_file_row_int(row, "space", 0);
    if (!zipName.empty() && zm_file_row_int(row, "type", 0) == zm_file::kTaskPack)
    {
        co_await ZmHttpServer::RunOnPool<bool>(
            [this, space, zipName]() -> bool
            { return m_store->RemoveFileSync(ZipPath(space, zipName)).ok; });
        if (m_token)
            m_token->RevokeByPack(taskNo);
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_access.erase(taskNo);
    }
    m_task->DropRetryPayload(taskNo);
    co_return ZMJSON::object();
}

void ZmFilePackModule::CleanCache(int64_t now)
{
    if (!m_store)
        return;
    m_store->EnsureDir(m_store->CacheRoot(0));
    // 遍历全部空间的缓存目录:zip(空闲 30 分钟 / 超 24 小时)+ chunk(超 24 小时)
    std::vector<ZmDiskEntry> spaces;
    ZmStoreResult sr = m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces);
    if (!sr.ok)
        return;
    auto scanSpace = [&](const std::string& spaceDir)
    {
        std::vector<ZmDiskEntry> zips;
        if (m_store->ScanDirSync(spaceDir + "\\zip", zips).ok)
        {
            for (const auto& z : zips)
            {
                if (z.isDir)
                    continue;
                int64_t idle = now - z.mtime;
                if (idle > zm_file::kCacheKeepSec)
                {
                    m_store->RemoveFileSync(spaceDir + "\\zip\\" + z.name);
                    DEFAULT_LOG_INFO("打包缓存超期清理: {}", z.name);
                }
            }
        }
        std::vector<ZmDiskEntry> chunks;
        if (m_store->ScanDirSync(spaceDir + "\\chunk", chunks).ok)
        {
            for (const auto& c : chunks)
            {
                if (!c.isDir)
                    continue;
                // 分片目录是上传临时区,与打包保留期无关,仍按天级回收
                if (now - c.mtime > zm_file::kCacheChunkKeepSec)
                    m_store->RemoveTreeSync(spaceDir + "\\chunk\\" + c.name);
            }
        }
    };
    for (const auto& s : spaces)
    {
        if (s.isDir)
            scanSpace(m_store->RootDir() + "\\space_cache\\" + s.name);
    }
    EnforceCacheQuota(now);
}

void ZmFilePackModule::EnforceCacheQuota(int64_t now)
{
    (void)now;
    // 容量保护:总占用超阈值时按"最久未访问优先"删压缩包,回落到阈值的 80%
    int64_t bytes  = 0;
    int64_t zips   = 0;
    int64_t chunks = 0;
    StatCache(bytes, zips, chunks);
    if (bytes <= zm_file::kCacheQuotaBytes)
        return;
    int64_t target = zm_file::kCacheQuotaBytes * 8 / 10;
    struct Item
    {
        std::string path;
        int64_t     size  = 0;
        int64_t     mtime = 0;
    };
    std::vector<Item>        items;
    std::vector<ZmDiskEntry> spaces;
    if (!m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces).ok)
        return;
    for (const auto& s : spaces)
    {
        if (!s.isDir)
            continue;
        std::string              zipDir = m_store->RootDir() + "\\space_cache\\" + s.name + "\\zip";
        std::vector<ZmDiskEntry> zs;
        if (!m_store->ScanDirSync(zipDir, zs).ok)
            continue;
        for (const auto& z : zs)
        {
            if (z.isDir)
                continue;
            items.push_back({zipDir + "\\" + z.name, z.size, z.mtime});
        }
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.mtime < b.mtime; });
    for (const auto& it : items)
    {
        if (bytes <= target)
            break;
        if (m_store->RemoveFileSync(it.path).ok)
        {
            bytes -= it.size;
            DEFAULT_LOG_INFO("打包缓存超容量回收: {}", it.path);
        }
    }
}

void ZmFilePackModule::RecoverOrphans()
{
    // 服务重启:进行中的打包任务置已中断(半成品由缓存清理兜底删除)
    m_db->ExecSync(
        "UPDATE transfer_tasks SET status = ?1, end_time = ?2, error = '服务重启中断' "
        "WHERE type = ?3 AND status IN (?4,?5)",
        {std::to_string(zm_file::kTaskInterrupted), std::to_string(ZmSqliteDb::Now()),
         std::to_string(zm_file::kTaskPack), std::to_string(zm_file::kTaskQueued),
         std::to_string(zm_file::kTaskRunning)});
}

void ZmFilePackModule::StatCache(int64_t& bytes, int64_t& zips, int64_t& chunks)
{
    bytes  = 0;
    zips   = 0;
    chunks = 0;
    std::vector<ZmDiskEntry> spaces;
    if (!m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces).ok)
        return;
    for (const auto& s : spaces)
    {
        if (!s.isDir)
            continue;
        std::string              base = m_store->RootDir() + "\\space_cache\\" + s.name;
        std::vector<ZmDiskEntry> zs;
        if (m_store->ScanDirSync(base + "\\zip", zs).ok)
        {
            for (const auto& z : zs)
            {
                if (z.isDir && !z.name.empty())
                {
                    // 隐藏的临时压缩包同样计入
                }
                if (!z.isDir)
                {
                    bytes += z.size;
                    ++zips;
                }
            }
        }
        std::vector<ZmDiskEntry> cs;
        if (m_store->ScanDirSync(base + "\\chunk", cs).ok)
        {
            for (const auto& c : cs)
            {
                if (!c.isDir)
                    continue;
                ++chunks;
                int64_t b  = 0;
                int64_t it = 0;
                m_store->TreeSizeSync(base + "\\chunk\\" + c.name, b, it);
                bytes += b;
            }
        }
    }
}

drogon::Task<ZMJSON> ZmFilePackModule::ListCache()
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this]() -> ZMJSON
        {
            ZMJSON                   list = ZMJSON::array();
            std::vector<ZmDiskEntry> spaces;
            if (!m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces).ok)
            {
                ZMJSON out   = ZMJSON::object();
                out["total"] = 0;
                out["list"]  = list;
                return out;
            }
            for (const auto& s : spaces)
            {
                if (!s.isDir)
                    continue;
                std::string base = m_store->RootDir() + "\\space_cache\\" + s.name;
                std::vector<ZmDiskEntry> zs;
                if (m_store->ScanDirSync(base + "\\zip", zs).ok)
                {
                    for (const auto& z : zs)
                    {
                        if (z.isDir)
                            continue;
                        ZMJSON item         = ZMJSON::object();
                        item["name"]        = z.name;
                        item["size"]        = z.size;
                        item["space"]       = s.name;
                        item["create_time"] = z.ctime;
                        item["access_time"] = z.mtime;
                        item["kind"]        = 1; // 1=压缩包
                        list.push_back(std::move(item));
                    }
                }
                std::vector<ZmDiskEntry> cs;
                if (m_store->ScanDirSync(base + "\\chunk", cs).ok)
                {
                    for (const auto& c : cs)
                    {
                        if (!c.isDir)
                            continue;
                        int64_t b  = 0;
                        int64_t it = 0;
                        m_store->TreeSizeSync(base + "\\chunk\\" + c.name, b, it);
                        ZMJSON item         = ZMJSON::object();
                        item["name"]        = c.name;
                        item["size"]        = b;
                        item["space"]       = s.name;
                        item["create_time"] = c.ctime;
                        item["access_time"] = c.mtime;
                        item["kind"]        = 2; // 2=分片目录
                        list.push_back(std::move(item));
                    }
                }
            }
            ZMJSON out   = ZMJSON::object();
            out["total"] = static_cast<int64_t>(list.size());
            out["list"]  = std::move(list);
            return out;
        });
}

drogon::Task<ZMJSON> ZmFilePackModule::CleanCacheFiles(const std::vector<std::string>& names)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, names]() -> ZMJSON
        {
            std::set<std::string>    want(names.begin(), names.end());
            int64_t                  bytes   = 0;
            int64_t                  deleted = 0;
            std::vector<ZmDiskEntry> spaces;
            if (!m_store->ScanDirSync(m_store->RootDir() + "\\space_cache", spaces).ok)
            {
                ZMJSON out     = ZMJSON::object();
                out["deleted"] = 0;
                out["bytes"]   = 0;
                return out;
            }
            for (const auto& s : spaces)
            {
                if (!s.isDir)
                    continue;
                std::string base = m_store->RootDir() + "\\space_cache\\" + s.name;
                for (const char* kind : {"zip", "chunk"})
                {
                    std::vector<ZmDiskEntry> items;
                    if (!m_store->ScanDirSync(base + "\\" + kind, items).ok)
                        continue;
                    for (const auto& it : items)
                    {
                        if (want.empty() || want.count(it.name))
                        {
                            int64_t sz = it.size;
                            if (it.isDir)
                            {
                                int64_t b = 0;
                                int64_t n = 0;
                                m_store->TreeSizeSync(base + "\\" + kind + "\\" + it.name, b,
                                                      n);
                                sz = b;
                            }
                            if (m_store->RemoveTreeSync(base + "\\" + kind + "\\" + it.name)
                                    .ok)
                            {
                                bytes += sz;
                                ++deleted;
                            }
                        }
                    }
                }
            }
            ZMJSON out     = ZMJSON::object();
            out["deleted"] = deleted;
            out["bytes"]   = bytes;
            return out;
        });
}
