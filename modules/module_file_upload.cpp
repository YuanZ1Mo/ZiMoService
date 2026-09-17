#include "modules/module_file_upload.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <openssl/evp.h>

#include "modules/util/dir_lock.h"
#include "modules/module_file_audit.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"
#include "modules/module_file_store.h"
#include "modules/module_file_task.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <random>

using namespace drogon;

namespace
{
/// 单请求上传允许的最大体积(超过要求走分片;避免大 body 全量驻留内存)
constexpr int64_t kSimpleMax = 64LL * 1024 * 1024;

/// UTF-8 → UTF-16(Windows API 用)
std::wstring ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

/// 字节 → 十六进制小写
std::string ToHex(const unsigned char* data, unsigned int len)
{
    static const char* hex = "0123456789abcdef";
    std::string        s;
    s.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i)
    {
        s += hex[data[i] >> 4];
        s += hex[data[i] & 0x0F];
    }
    return s;
}

/// 路径分隔符统一为 '/' 供审计展示
std::string ToSlashPath(const std::string& winPath)
{
    std::string s = winPath;
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/// 随机十六进制串(上传标识)
std::string RandomToken(int bytes)
{
    static const char*                 hex = "0123456789abcdef";
    std::random_device                 rd;
    std::mt19937_64                    gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string                        s;
    s.reserve(static_cast<size_t>(bytes) * 2);
    for (int i = 0; i < bytes * 2; ++i)
        s += hex[dist(gen)];
    return s;
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileUploadModule::ZmFileUploadModule(ZmFileDbModule* db, ZmFileStoreModule* store,
                                       ZmFileNodeModule* node, ZmFileTaskModule* task,
                                       ZmFileAuditModule* audit, ZmDirLock* lock)
    : m_db(db), m_store(store), m_node(node), m_task(task), m_audit(audit), m_lock(lock)
{
}

ZmFileUploadModule::~ZmFileUploadModule() = default;

// ============================================================================
// 工具
// ============================================================================
std::string ZmFileUploadModule::HashBufSha256(const char* data, size_t len)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return "";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLen = 0;
    std::string   out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 && EVP_DigestFinal_ex(ctx, md, &mdLen) == 1)
        out = ToHex(md, mdLen);
    EVP_MD_CTX_free(ctx);
    return out;
}

std::string ZmFileUploadModule::HashFileSha256(const std::string& path)
{
    std::wstring w = ToWide(ZmFileStoreModule::ToExtended(path));
    if (w.empty())
        return "";
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        CloseHandle(h);
        return "";
    }
    bool              ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    std::vector<char> buf(1 << 20);
    while (ok)
    {
        DWORD done = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &done, nullptr) ||
            done == 0)
            break;
        ok = EVP_DigestUpdate(ctx, buf.data(), done) == 1;
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLen = 0;
    std::string   out;
    if (ok && EVP_DigestFinal_ex(ctx, md, &mdLen) == 1)
        out = ToHex(md, mdLen);
    EVP_MD_CTX_free(ctx);
    CloseHandle(h);
    return out;
}

std::string ZmFileUploadModule::TmpDir(int64_t space) const
{
    return m_store->CacheRoot(space) + "\\tmp";
}

std::string ZmFileUploadModule::NewTmpPath(int64_t space) const
{
    return TmpDir(space) + "\\" + RandomToken(8) + ".part";
}

std::string ZmFileUploadModule::FindInstantSource(int64_t space, const std::string& hash,
                                                  int64_t size)
{
    if (hash.empty() || size <= 0)
        return "";
    ZMJSON row = m_db->QueryRowSync(
        "SELECT id FROM nodes WHERE space = ?1 AND hash = ?2 AND size = ?3 AND deleted = 0 "
        "AND type = 2 LIMIT 1",
        {std::to_string(space), hash, std::to_string(size)});
    if (row.empty())
        return "";
    int64_t     id = zm_file_row_int(row, "id", 0);
    int64_t     sp = space;
    std::string path;
    if (!m_store->PhysicalPathSync(id, sp, path).ok)
        return "";
    return path;
}

// ============================================================================
// 入位收尾(单请求 / 分片合并 / 秒传共用)
// ============================================================================
ZMJSON ZmFileUploadModule::FinalizeSync(const ZmOpCtx& ctx, int64_t space, int64_t dirId,
                                        const std::string& name, const std::string& conflict,
                                        const std::string& tmpPath, int64_t size,
                                        const std::string& hash, const std::string& taskNo)
{
    std::string msg;
    if (!ZmFileNodeModule::ValidateName(name, msg))
    {
        m_store->RemoveFileSync(tmpPath);
        return ZmFileError(zm_file_err::kNameInvalid, 400, msg);
    }
    if (dirId != 0)
    {
        ZMJSON row;
        if (!m_node->VisibleSync(dirId, row) ||
            zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
            zm_file_row_int(row, "space", 0) != space)
        {
            m_store->RemoveFileSync(tmpPath);
            if (taskNo.empty())
                return ZmFileError(zm_file_err::kDirNotFound, 404, "目标目录不存在");
            m_task->SetStatusSync(taskNo, zm_file::kTaskFailed, "目标目录已被删除");
            return ZmFileError(zm_file_err::kDirNotFound, 404, "目标目录不存在");
        }
    }

    m_store->EnsureSpaceRoot(space); // 目标空间物理根(个人空间懒创建)
    ZmDirLock::Guard guard(*m_lock, space, dirId);
    int64_t          dupId     = 0;
    int              dupType   = 0;
    bool             hasDup    = m_node->ConflictSync(space, dirId, name, 0, dupId, dupType);
    std::string      finalName = name;
    bool             overwrite = false;
    if (hasDup)
    {
        if (conflict == zm_file_conflict::kAsk)
        {
            // ask:把冲突清单回给前端,由用户选策略后整批重试
            ZMJSON conflicts = ZMJSON::array();
            ZMJSON c         = ZMJSON::object();
            c["id"]          = dupId;
            c["name"]        = name;
            c["why"]         = std::string("目标已存在同名") +
                       (dupType == zm_file::kTypeDir ? "文件夹" : "文件");
            conflicts.push_back(std::move(c));
            if (!taskNo.empty())
                m_task->SetStatusSync(taskNo, zm_file::kTaskFailed,
                                      "同名冲突(未选择处理策略)");
            m_store->RemoveFileSync(tmpPath);
            ZMJSON extra       = ZMJSON::object();
            extra["conflicts"] = std::move(conflicts);
            return ZmFileErrorExtra(zm_file_err::kNameExists, 409,
                                    "同名文件已存在,请选择处理方式", extra);
        }
        if (conflict == zm_file_conflict::kSkip)
        {
            m_store->RemoveFileSync(tmpPath);
            if (!taskNo.empty())
                m_task->SetStatusSync(taskNo, zm_file::kTaskDone, "", "skipped");
            ZMJSON out     = ZMJSON::object();
            out["node_id"] = 0;
            out["task_no"] = taskNo;
            out["skipped"] = true;
            return out;
        }
        if (conflict == zm_file_conflict::kOverwrite && dupType == zm_file::kTypeFile)
        {
            overwrite = true;
        }
        else
        {
            // 目录或不支持覆盖的场景一律按 rename 处理
            finalName = m_node->FreeNameSync(space, dirId, name);
        }
    }

    std::string targetRel;
    {
        int64_t sp = space;
        m_db->PathPartsSync(dirId, sp, targetRel);
    }
    std::string rel = targetRel.empty() ? finalName : (targetRel + "\\" + finalName);
    if (!m_store->ValidateLength(space, rel, msg))
    {
        m_store->RemoveFileSync(tmpPath);
        if (!taskNo.empty())
            m_task->SetStatusSync(taskNo, zm_file::kTaskFailed, "路径过长");
        return ZmFileError(zm_file_err::kPathTooLong, 400, msg);
    }
    std::string dstPath = m_store->SpaceRoot(space) + "\\" + rel;

    int64_t dupSize = 0;
    if (overwrite)
    {
        ZMJSON dupRow = m_db->NodeRowSync(dupId);
        dupSize       = zm_file_row_int(dupRow, "size", 0);
    }

    // 原子入位:读方永远看不到半成品
    ZmStoreResult place = m_store->AtomicPlace(tmpPath, dstPath);
    if (!place.ok)
    {
        m_store->RemoveFileSync(tmpPath);
        if (!taskNo.empty())
            m_task->SetStatusSync(taskNo, zm_file::kTaskFailed, place.message);
        return ZmFileError(zm_file_err::kInternal, 500, place.message);
    }

    int64_t     newId = 0;
    std::string ext   = ZmFileNodeModule::ExtOf(finalName);
    bool        ok    = m_db->WithTxSync(
        [&](ZmSqliteDb& db) -> bool
        {
            if (overwrite)
            {
                // 覆盖:目标行与占用一并清除,由新文件顶替(原子替换语义)
                if (!db.ExecSync("DELETE FROM nodes WHERE id = ?1", {std::to_string(dupId)}))
                    return false;
                if (!ZmFileDbModule::ApplyUsageSync(db, space, -dupSize, -1))
                    return false;
            }
            if (!ZmFileNodeModule::InsertFileSync(db, space, dirId, finalName, size, ext, hash,
                                                            ctx.uid, newId))
                return false;
            ZMJSON detail     = ZMJSON::object();
            detail["path"]    = ToSlashPath(rel);
            detail["bytes"]   = size;
            detail["instant"] = false;
            return m_audit->RecordFileOpSync(db, ctx.uid, ctx.account, zm_file::kActUpload,
                                                       space, newId, finalName, detail.dump(), ctx.ip,
                                                       1);
        });
    if (!ok)
    {
        // 入位成功但落库失败:必须删掉刚落位的物理文件(否则会被一致性同步补建)
        ZmStoreResult back = m_store->RemoveFileSync(dstPath);
        if (!back.ok)
            DEFAULT_LOG_ERROR("上传回滚失败: {}", back.message);
        if (!taskNo.empty())
            m_task->SetStatusSync(taskNo, zm_file::kTaskFailed, "写入数据库失败");
        return ZmFileError(zm_file_err::kInternal, 500, "上传失败,请重试");
    }

    if (!taskNo.empty())
    {
        m_task->UpdateProgressSync(taskNo, size, 1);
        m_task->SetStatusSync(taskNo, zm_file::kTaskDone, "", finalName);
    }
    ZMJSON out     = ZMJSON::object();
    out["node_id"] = newId;
    out["task_no"] = taskNo;
    return out;
}

// ============================================================================
// 单请求上传
// ============================================================================
drogon::Task<ZMJSON> ZmFileUploadModule::Simple(const ZmOpCtx& ctx, int64_t space,
                                                int64_t dirId, const std::string& name,
                                                const std::string& conflict,
                                                const std::string& body)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, space, dirId, name, conflict, body]() -> ZMJSON
        {
            if (!ZmFileNodeModule::SpaceWritable(space, ctx.uid))
                return ZmFileError(zm_file_err::kPermDenied, 403, "无权写入该空间");
            if (static_cast<int64_t>(body.size()) > kSimpleMax)
                return ZmFileError(zm_file_err::kFileTooLarge, 413, "文件过大,请使用分片上传");
            // 个人空间懒创建(行 + 目录):缺行会让配额校验失去依据
            m_db->EnsureSpaceSync(space);
            // 配额预检(个人空间);与分片 init 同一口径
            {
                ZMJSON  spaceRow = m_db->SpaceRowSync(space);
                int64_t quota    = zm_file_row_int(spaceRow, "quota", 0);
                int64_t used     = zm_file_row_int(spaceRow, "used_size", 0);
                if (quota > 0 && used + static_cast<int64_t>(body.size()) > quota)
                    return ZmFileError(zm_file_err::kQuotaExceeded, 403, "空间配额不足");
            }
            m_store->EnsureSpaceRoot(space);
            std::string   tmp = NewTmpPath(space);
            ZmStoreResult mk  = m_store->EnsureDir(TmpDir(space));
            if (!mk.ok)
                return ZmFileError(zm_file_err::kInternal, 500, mk.message);
            ZmStoreResult wr = m_store->WriteFileSync(tmp, body.data(), body.size());
            if (!wr.ok)
                return ZmFileError(zm_file_err::kInternal, 500, wr.message);

            std::string hash = HashBufSha256(body.data(), body.size());
            // 并发闸门与建任务同临界区:否则并发请求会各自"先看后建"全部通过
            std::string taskNo;
            {
                std::lock_guard<std::mutex> lk(m_gateMtx);
                if (m_task->CountRunningUploadsSync(ctx.uid) >= zm_file::kUploadMaxRunning)
                {
                    m_store->RemoveFileSync(tmp);
                    return ZmFileError(zm_file_err::kTooManyUploads, 429,
                                       "进行中的上传任务过多,请稍后重试");
                }
                // 任务先建、入位收尾时置完成(单请求上传不进排队,直接是进行中)
                m_task->CreateSync(zm_file::kTaskUpload, ctx.uid, space, name, "",
                                   static_cast<int64_t>(body.size()), 1, "", taskNo);
                m_task->SetStatusSync(taskNo, zm_file::kTaskRunning);
            }
            ZMJSON out = FinalizeSync(ctx, space, dirId, name, conflict, tmp,
                                      static_cast<int64_t>(body.size()), hash, taskNo);
            return out;
        });
}

// ============================================================================
// 分片上传:初始化
// ============================================================================
drogon::Task<ZMJSON> ZmFileUploadModule::Init(const ZmOpCtx& ctx, int64_t space, int64_t dirId,
                                              const std::string& name, int64_t size,
                                              const std::string& hash,
                                              const std::string& conflict)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, space, dirId, name, hash, conflict, size]() -> ZMJSON
        {
            if (!ZmFileNodeModule::SpaceWritable(space, ctx.uid))
                return ZmFileError(zm_file_err::kPermDenied, 403, "无权写入该空间");
            std::string msg;
            if (!ZmFileNodeModule::ValidateName(name, msg))
                return ZmFileError(zm_file_err::kNameInvalid, 400, msg);
            if (size <= 0)
                return ZmFileError(zm_file_err::kBadRequest, 400, "文件大小非法");
            if (size > zm_file::kMaxFileSize)
                return ZmFileError(zm_file_err::kFileTooLarge, 413,
                                   "超出单文件大小上限(20GB)");
            if (dirId != 0)
            {
                ZMJSON row;
                if (!m_node->VisibleSync(dirId, row) ||
                    zm_file_row_int(row, "type", 0) != zm_file::kTypeDir ||
                    zm_file_row_int(row, "space", 0) != space)
                    return ZmFileError(zm_file_err::kDirNotFound, 404, "目标目录不存在");
            }
            // 个人空间懒创建(行 + 目录)
            m_db->EnsureSpaceSync(space);
            // 配额预检(个人空间)
            ZMJSON  spaceRow = m_db->SpaceRowSync(space);
            int64_t quota    = zm_file_row_int(spaceRow, "quota", 0);
            int64_t used     = zm_file_row_int(spaceRow, "used_size", 0);
            if (quota > 0 && used + size > quota)
                return ZmFileError(zm_file_err::kQuotaExceeded, 403, "空间配额不足");

            int64_t chunkTotal = (size + zm_file::kChunkSize - 1) / zm_file::kChunkSize;
            if (chunkTotal > zm_file::kMaxChunks)
                return ZmFileError(zm_file_err::kFileTooLarge, 413, "分片数超出上限");

            // ── 秒传:同空间内按 (hash,size) 命中已有文件 ──
            if (!hash.empty())
            {
                std::string src = FindInstantSource(space, hash, size);
                if (!src.empty())
                {
                    ZmStoreResult mk = m_store->EnsureDir(TmpDir(space));
                    if (!mk.ok)
                        return ZmFileError(zm_file_err::kInternal, 500, mk.message);
                    std::string   tmp = NewTmpPath(space);
                    ZmStoreResult cp  = m_store->CopyTreeSync(src, tmp);
                    if (cp.ok)
                    {
                        std::string taskNo;
                        m_task->CreateSync(zm_file::kTaskUpload, ctx.uid, space, name, "",
                                           size, 1, "", taskNo);
                        m_task->SetStatusSync(taskNo, zm_file::kTaskRunning);
                        ZMJSON fin = FinalizeSync(ctx, space, dirId, name, conflict, tmp, size,
                                                  hash, taskNo);
                        if (ZmFileHasError(fin))
                            return fin;
                        ZMJSON out         = ZMJSON::object();
                        out["upload_id"]   = "instant_" + RandomToken(8);
                        out["chunk_size"]  = zm_file::kChunkSize;
                        out["chunk_total"] = 0;
                        out["uploaded"]    = ZMJSON::array();
                        out["instant"]     = true;
                        out["node_id"]     = zm_file_row_int(fin, "node_id", 0);
                        out["task_no"]     = taskNo;
                        out["expire_time"] = ZmSqliteDb::Now() + zm_file::kUploadTtlSec;
                        return out;
                    }
                    m_store->RemoveFileSync(tmp);
                    // 复制失败退化为普通分片流程
                }
            }

            int64_t now = ZmSqliteDb::Now();
            // ── 断点续传与并发闸门同临界区 ──
            std::lock_guard<std::mutex> gateLock(m_gateMtx);
            // ── 断点续传:同用户同目录同名且进行中的会话直接复用 ──
            ZMJSON sess = m_db->QueryRowSync(
                "SELECT * FROM upload_sessions WHERE uid = ?1 AND space = ?2 AND parent_id = "
                "?3 "
                "AND name = ?4 COLLATE NOCASE AND status = 1 AND expire_time > ?5 LIMIT 1",
                {std::to_string(ctx.uid), std::to_string(space), std::to_string(dirId), name,
                 std::to_string(now)});
            std::string uploadId;
            std::string taskNo;
            int64_t     expire = 0;
            if (!sess.empty() && zm_file_row_int(sess, "size", 0) == size)
            {
                uploadId = zm_file_row_str(sess, "upload_id");
                expire   = zm_file_row_int(sess, "expire_time", 0);
                taskNo   = zm_file_row_str(sess, "task_no");
            }
            else
            {
                // 并发上传闸门(单用户进行中的上传任务 ≤3);秒传命中不占额度
                if (m_task->CountRunningUploadsSync(ctx.uid) >= zm_file::kUploadMaxRunning)
                    return ZmFileError(zm_file_err::kTooManyUploads, 429,
                                       "进行中的上传任务过多,请稍后重试");
                uploadId = "up_" + RandomToken(10);
                expire   = now + zm_file::kUploadTtlSec;
                if (!m_db->ExecSync(
                        "INSERT INTO upload_sessions(upload_id,uid,space,parent_id,name,size,"
                        "file_hash,chunk_size,chunk_total,chunk_done,conflict,status,create_"
                        "time,"
                        "expire_time) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,0,?10,1,?11,?12)",
                        {uploadId, std::to_string(ctx.uid), std::to_string(space),
                         std::to_string(dirId), name, std::to_string(size), hash,
                         std::to_string(zm_file::kChunkSize), std::to_string(chunkTotal),
                         conflict, std::to_string(now), std::to_string(expire)}))
                    return ZmFileError(zm_file_err::kInternal, 500, "创建上传会话失败");
                // 任务在 init 建、complete 收尾,两次响应的 task_no 相同
                m_task->CreateSync(zm_file::kTaskUpload, ctx.uid, space, name, "", size, 1,
                                   uploadId, taskNo);
                m_store->EnsureDir(m_store->ChunkDir(space, uploadId));
            }

            ZMJSON chunkRows = m_db->QueryRowsSync("SELECT chunk_index FROM upload_chunks "
                                                   "WHERE upload_id = ?1 ORDER BY chunk_index",
                                                   {uploadId});
            ZMJSON uploaded  = ZMJSON::array();
            for (const auto& r : chunkRows)
                uploaded.push_back(zm_file_row_int(r, "chunk_index", 0));

            ZMJSON out         = ZMJSON::object();
            out["upload_id"]   = uploadId;
            out["chunk_size"]  = zm_file::kChunkSize;
            out["chunk_total"] = chunkTotal;
            out["uploaded"]    = std::move(uploaded);
            out["instant"]     = false;
            out["expire_time"] = expire;
            out["task_no"]     = taskNo;
            return out;
        });
}

// ============================================================================
// 分片上传:接收分片
// ============================================================================
drogon::Task<ZMJSON> ZmFileUploadModule::PutChunk(const ZmOpCtx&     ctx,
                                                  const std::string& uploadId, int64_t index,
                                                  const std::string& data)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, uploadId, index, data]() -> ZMJSON
        {
            int64_t now  = ZmSqliteDb::Now();
            ZMJSON  sess = m_db->QueryRowSync(
                "SELECT * FROM upload_sessions WHERE upload_id = ?1", {uploadId});
            if (sess.empty() || zm_file_row_int(sess, "uid", 0) != ctx.uid ||
                zm_file_row_int(sess, "status", 0) != zm_file::kUploadRunning ||
                zm_file_row_int(sess, "expire_time", 0) < now)
                return ZmFileError(zm_file_err::kUploadNotFound, 404,
                                   "上传会话不存在或已过期");

            int64_t space = zm_file_row_int(sess, "space", 0);
            int64_t total = zm_file_row_int(sess, "chunk_total", 0);
            int64_t size  = zm_file_row_int(sess, "size", 0);
            if (index < 0 || index >= total)
                return ZmFileError(zm_file_err::kBadRequest, 400, "片号越界");
            int64_t expect = (index == total - 1) ? size - index * zm_file::kChunkSize
                                                  : zm_file::kChunkSize;
            if (static_cast<int64_t>(data.size()) != expect)
                return ZmFileError(zm_file_err::kChunkSizeMismatch, 400, "分片长度与声明不符");

            std::string   dir = m_store->ChunkDir(space, uploadId);
            ZmStoreResult mk  = m_store->EnsureDir(dir);
            if (!mk.ok)
                return ZmFileError(zm_file_err::kInternal, 500, mk.message);
            std::string   partPath = dir + "\\" + std::to_string(index) + ".part";
            ZmStoreResult wr = m_store->WriteFileSync(partPath, data.data(), data.size());
            if (!wr.ok)
                return ZmFileError(zm_file_err::kInternal, 500, wr.message);

            // 分片可重复上传:覆盖该片的登记行
            if (!m_db->ExecSync(
                    "INSERT INTO upload_chunks(upload_id,chunk_index,size,done_time) "
                    "VALUES(?1,?2,?3,?4) ON CONFLICT(upload_id,chunk_index) DO "
                    "UPDATE SET size = ?3, done_time = ?4",
                    {uploadId, std::to_string(index), std::to_string(data.size()),
                     std::to_string(now)}))
                return ZmFileError(zm_file_err::kInternal, 500, "登记分片失败");

            ZMJSON cnt = m_db->QueryRowSync(
                "SELECT COUNT(*) AS n FROM upload_chunks WHERE upload_id = ?1", {uploadId});
            int64_t done = zm_file_row_int(cnt, "n", 0);
            m_db->ExecSync("UPDATE upload_sessions SET chunk_done = ?1 WHERE upload_id = ?2",
                           {std::to_string(done), uploadId});
            // 任务进度:按已完成分片数折算字节(前端的进度以客户端视角为主)
            ZMJSON taskRow = m_db->QueryRowSync(
                "SELECT task_no FROM transfer_tasks WHERE ref_id = ?1 AND type = ?2 LIMIT 1",
                {uploadId, std::to_string(zm_file::kTaskUpload)});
            if (!taskRow.empty())
            {
                std::string taskNo    = zm_file_row_str(taskRow, "task_no");
                int64_t     doneBytes = std::min(size, done * zm_file::kChunkSize);
                m_task->UpdateProgressSync(taskNo, doneBytes, 0);
            }

            ZMJSON out         = ZMJSON::object();
            out["received"]    = true;
            out["chunk_done"]  = done;
            out["chunk_total"] = total;
            return out;
        });
}

// ============================================================================
// 分片上传:完成合并
// ============================================================================
drogon::Task<ZMJSON> ZmFileUploadModule::Complete(const ZmOpCtx&     ctx,
                                                  const std::string& uploadId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, uploadId]() -> ZMJSON
        {
            int64_t now  = ZmSqliteDb::Now();
            ZMJSON  sess = m_db->QueryRowSync(
                "SELECT * FROM upload_sessions WHERE upload_id = ?1", {uploadId});
            if (sess.empty() || zm_file_row_int(sess, "uid", 0) != ctx.uid ||
                zm_file_row_int(sess, "status", 0) != zm_file::kUploadRunning ||
                zm_file_row_int(sess, "expire_time", 0) < now)
                return ZmFileError(zm_file_err::kUploadNotFound, 404,
                                   "上传会话不存在或已过期");

            int64_t     space    = zm_file_row_int(sess, "space", 0);
            int64_t     dirId    = zm_file_row_int(sess, "parent_id", 0);
            std::string name     = zm_file_row_str(sess, "name");
            int64_t     size     = zm_file_row_int(sess, "size", 0);
            std::string hash     = zm_file_row_str(sess, "file_hash");
            std::string conflict = zm_file_row_str(sess, "conflict");
            int64_t     total    = zm_file_row_int(sess, "chunk_total", 0);
            ZMJSON      taskRow  = m_db->QueryRowSync(
                "SELECT task_no FROM transfer_tasks WHERE ref_id = ?1 AND type = ?2 LIMIT 1",
                {uploadId, std::to_string(zm_file::kTaskUpload)});
            std::string taskNo = zm_file_row_str(taskRow, "task_no");

            if (!ZmFileNodeModule::SpaceWritable(space, ctx.uid))
                return ZmFileError(zm_file_err::kPermDenied, 403, "无权写入该空间");

            // ① 校验片齐(登记行 + 物理文件)
            ZMJSON cnt = m_db->QueryRowSync(
                "SELECT COUNT(*) AS n FROM upload_chunks WHERE upload_id = ?1", {uploadId});
            if (zm_file_row_int(cnt, "n", 0) != total)
                return ZmFileError(zm_file_err::kBadRequest, 400, "分片未全部上传");
            std::string dir = m_store->ChunkDir(space, uploadId);
            for (int64_t i = 0; i < total; ++i)
            {
                if (!m_store->Exists(dir + "\\" + std::to_string(i) + ".part"))
                    return ZmFileError(zm_file_err::kBadRequest, 400, "分片缺失,请补齐后重试");
            }

            // ② 合并到临时文件(不占事件循环)
            ZmStoreResult mk = m_store->EnsureDir(TmpDir(space));
            if (!mk.ok)
                return ZmFileError(zm_file_err::kInternal, 500, mk.message);
            std::string tmp = NewTmpPath(space);
            m_store->RemoveFileSync(tmp);
            int64_t merged = 0;
            for (int64_t i = 0; i < total; ++i)
            {
                std::string   data;
                ZmStoreResult rd =
                    m_store->ReadFileSync(dir + "\\" + std::to_string(i) + ".part", data);
                if (!rd.ok)
                {
                    m_store->RemoveFileSync(tmp);
                    return ZmFileError(zm_file_err::kInternal, 500, rd.message);
                }
                ZmStoreResult ap = m_store->AppendFileSync(tmp, data.data(), data.size());
                if (!ap.ok)
                {
                    m_store->RemoveFileSync(tmp);
                    return ZmFileError(zm_file_err::kInternal, 500, ap.message);
                }
                merged += static_cast<int64_t>(data.size());
                if (!taskNo.empty())
                    m_task->UpdateProgressSync(taskNo, merged, 0);
            }
            if (merged != size)
            {
                m_store->RemoveFileSync(tmp);
                return ZmFileError(zm_file_err::kChunkSizeMismatch, 400,
                                   "合并结果与声明大小不符");
            }

            // ③ 校验整文件哈希(客户端提供时才校验)
            if (!hash.empty())
            {
                std::string actual = HashFileSha256(tmp);
                if (actual != hash)
                {
                    m_store->RemoveFileSync(tmp);
                    return ZmFileError(zm_file_err::kHashMismatch, 400,
                                       "文件校验失败,请重新上传");
                }
            }

            // ④ 锁内裁决 + 原子入位 + 落库
            ZMJSON out =
                FinalizeSync(ctx, space, dirId, name, conflict, tmp, size, hash, taskNo);
            if (ZmFileHasError(out))
                return out;

            // ⑤ 清理分片目录与会话
            m_store->RemoveTreeSync(dir);
            m_db->ExecSync("DELETE FROM upload_chunks WHERE upload_id = ?1", {uploadId});
            m_db->ExecSync("UPDATE upload_sessions SET status = ?1 WHERE upload_id = ?2",
                           {std::to_string(zm_file::kUploadDone), uploadId});
            return out;
        });
}

// ============================================================================
// 查询 / 取消 / 重试 / 清理
// ============================================================================
drogon::Task<ZMJSON> ZmFileUploadModule::Status(const ZmOpCtx&     ctx,
                                                const std::string& uploadId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, uploadId]() -> ZMJSON
        {
            ZMJSON sess = m_db->QueryRowSync(
                "SELECT * FROM upload_sessions WHERE upload_id = ?1", {uploadId});
            if (sess.empty() || zm_file_row_int(sess, "uid", 0) != ctx.uid)
                return ZmFileError(zm_file_err::kUploadNotFound, 404,
                                   "上传会话不存在或已过期");
            ZMJSON rows = m_db->QueryRowsSync("SELECT chunk_index FROM upload_chunks WHERE "
                                              "upload_id = ?1 ORDER BY chunk_index",
                                              {uploadId});
            ZMJSON uploaded = ZMJSON::array();
            for (const auto& r : rows)
                uploaded.push_back(zm_file_row_int(r, "chunk_index", 0));
            ZMJSON out         = ZMJSON::object();
            out["uploaded"]    = std::move(uploaded);
            out["chunk_total"] = zm_file_row_int(sess, "chunk_total", 0);
            out["expire_time"] = zm_file_row_int(sess, "expire_time", 0);
            return out;
        });
}

drogon::Task<ZMJSON> ZmFileUploadModule::Cancel(const ZmOpCtx&     ctx,
                                                const std::string& uploadId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, ctx, uploadId]() -> ZMJSON
        {
            ZMJSON sess = m_db->QueryRowSync(
                "SELECT * FROM upload_sessions WHERE upload_id = ?1", {uploadId});
            if (sess.empty() || zm_file_row_int(sess, "uid", 0) != ctx.uid)
                return ZmFileError(zm_file_err::kUploadNotFound, 404,
                                   "上传会话不存在或已过期");
            int64_t space = zm_file_row_int(sess, "space", 0);
            m_db->ExecSync("UPDATE upload_sessions SET status = ?1 WHERE upload_id = ?2",
                           {std::to_string(zm_file::kUploadCanceled), uploadId});
            m_db->ExecSync("DELETE FROM upload_chunks WHERE upload_id = ?1", {uploadId});
            m_store->RemoveTreeSync(m_store->ChunkDir(space, uploadId));
            ZMJSON taskRow = m_db->QueryRowSync(
                "SELECT task_no FROM transfer_tasks WHERE ref_id = ?1 AND type = ?2 LIMIT 1",
                {uploadId, std::to_string(zm_file::kTaskUpload)});
            if (!taskRow.empty())
                m_task->SetStatusSync(zm_file_row_str(taskRow, "task_no"),
                                      zm_file::kTaskCanceled, "已取消");
            return ZMJSON::object();
        });
}

drogon::Task<ZMJSON> ZmFileUploadModule::Retry(const ZMJSON& oldTask)
{
    std::string uploadId = zm_file_row_str(oldTask, "ref_id");
    int64_t     uid      = zm_file_row_int(oldTask, "uid", 0);
    std::string account  = "";
    if (uploadId.empty())
        co_return ZmFileError(zm_file_err::kBadRequest, 400,
                              "上传任务已失效,请在原位置重新上传");
    ZmOpCtx ctx;
    ctx.uid     = uid;
    ctx.account = account;
    ZMJSON out  = co_await Complete(ctx, uploadId);
    if (ZmFileHasError(out))
        co_return out;
    ZMJSON r     = ZMJSON::object();
    r["task_no"] = zm_file_row_str(out, "task_no");
    co_return r;
}

void ZmFileUploadModule::SweepStaleTasks(int64_t now)
{
    // 有 ref_id 的才是分片上传任务;Simple 的 ref_id 为空,不受影响
    m_db->ExecSync(
        "UPDATE transfer_tasks SET status = ?1, end_time = ?2, error = '上传已中断' "
        "WHERE type = ?3 AND status IN (?4,?5) AND ref_id <> '' AND ref_id NOT IN "
        "(SELECT upload_id FROM upload_sessions WHERE status = ?6 AND expire_time > ?7)",
        {std::to_string(zm_file::kTaskInterrupted), std::to_string(now),
         std::to_string(zm_file::kTaskUpload), std::to_string(zm_file::kTaskQueued),
         std::to_string(zm_file::kTaskRunning), std::to_string(zm_file::kUploadRunning),
         std::to_string(now)});
}

void ZmFileUploadModule::PurgeExpired(int64_t now)
{
    // 过期会话:置已过期 + 删分片目录与分片行(物理删除在缓存区)
    ZMJSON rows = m_db->QueryRowsSync(
        "SELECT upload_id, space FROM upload_sessions WHERE status = ?1 AND expire_time < ?2",
        {std::to_string(zm_file::kUploadRunning), std::to_string(now)});
    for (const auto& r : rows)
    {
        std::string uploadId = zm_file_row_str(r, "upload_id");
        int64_t     space    = zm_file_row_int(r, "space", 0);
        m_db->ExecSync("UPDATE upload_sessions SET status = ?1 WHERE upload_id = ?2",
                       {std::to_string(zm_file::kUploadExpired), uploadId});
        m_db->ExecSync("DELETE FROM upload_chunks WHERE upload_id = ?1", {uploadId});
        if (m_store)
            m_store->RemoveTreeSync(m_store->ChunkDir(space, uploadId));
        DEFAULT_LOG_INFO("ZmFileUploadModule: 上传会话已过期回收 {}", uploadId);
    }
}
