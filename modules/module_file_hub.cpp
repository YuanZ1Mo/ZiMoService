#include "module_file_hub.h"

#include "module_user.h"
#include "service_define.h"
#include "http_frontend_manager.h"
#include "zm_net_runloop.h"

#include "zm_net_req_loop_protocol.h"   // ZmReqLoopRest
#include "zm_util_logger.h"
#include "zm_util_str.h"
#include "zm_util_sys.h"
#include "zm_util_zipfile.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <thread>
#include <chrono>
#include <functional>
#include <algorithm>

#include <sqlite3.h>
#include <openssl/sha.h>

#include <windows.h>

#include <event2/buffer.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <io.h>
#include <set>
#include <share.h>
#include <tuple>

namespace
{
// ============================================================================
// 内部小工具
// ============================================================================

// 公共库 sqlite 辅助(预处理语句/绑定):别名与 using 保持全部调用点不变
using Stmt = zm::ZmSqliteStmt;
using zm::BindText;
using zm::BindInt;

/** 一致性校验/分享下载日志清理:每日执行时刻(时) */
static constexpr int kVerifyCleanHour = 3;
/** 分享下载日志保留(秒,90 天,与审计一致) */
static constexpr int64_t kShareDownloadLogRetain = 90LL * 24 * 3600;

// ── 传输任务(transfer_tasks)常量 ─────────────────────────────────────────
/** 任务状态 */
static constexpr const char* kTaskStatusUploading = "uploading";  // 上传中
static constexpr const char* kTaskStatusPacking   = "packing";    // zip 打包中
static constexpr const char* kTaskStatusTriggered = "triggered";  // 已触发下载(直链已发出)
static constexpr const char* kTaskStatusDone      = "done";       // 完成
static constexpr const char* kTaskStatusFailed    = "failed";     // 失败/取消/中断

/** 加载历史时"上传中断"判定阈值(秒):前端上传 60s 即超时放弃,活任务不可能超过 90s,
 *  多标签页共存也不会误杀(活上传行 update_time 随状态推进刷新,且 <90s) */
static constexpr int64_t kTaskStaleSec = 90;
/** 清理线程兜底阈值(秒):非终态超 30 分钟强制标 failed(客户端中断) */
static constexpr int64_t kTaskStaleForceSec = 30LL * 60;
/** 历史保留(秒,30 天) */
static constexpr int64_t kTaskRetainDays = 30LL * 24 * 3600;
/** 历史分页页大小 */
static constexpr int kTaskPageSize = 50;

// ── 文件下载流式常量 ────────────────────────────────────────────────────────
/** 分块读-发块大小(1MB;与 zip 打包发送粒度一致) */
static constexpr size_t kFileStreamChunkSize = 1024 * 1024;
/** 混合发送策略阈值(filehub-zip-task-design §3.2):文件 <2GB → 分段零拷贝
 *  (vendored libevent SSL bev 发送 ≥2GB body 实测必败);≥2GB → 分块回退+水位节流 */
static constexpr int64_t kZeroCopyMaxFileBytes = (int64_t)2 * 1024 * 1024 * 1024;
/** ≥2GB 回退路径水位:排队未发字节超过该值暂停读盘等排水(防慢客户端内存线性涨) */
static constexpr size_t kSendWatermarkBytes = (size_t)8 * 1024 * 1024;
/** 对端停滞放弃阈值(毫秒):持续堵在水位以上的最长时间,超时放弃本次响应
 *  (Range 客户端可断点续传重试;不设上限会永久占住请求循环线程) */
static constexpr int64_t kSendStallAbortMs = 120000;
/** 打包进度刷库粒度(字节):任务行 total_size 刷新最小间隔,防小文件逐条目写库 */
static constexpr uint64_t kPackProgressFlushBytes = 64ull * 1024 * 1024;

/** @brief 安全读 int64(字段缺失/类型不匹配返回默认值) */
int64_t JsonInt(const ZMJSON& j, std::string_view key, int64_t def = 0)
{
    if (!j.contains(key))
        return def;
    try { return j[key].get<int64_t>(); }
    catch (...) { return def; }
}

/** @brief 名称合法性:非空、≤255 字节、不含非法字符;
 *          支持隐藏文件/目录(. 开头),但显式排除 . 与 ..(防路径穿越) */
bool IsValidName(const std::string& name)
{
    if (name.empty() || name.size() > 255)
        return false;
    if (name == "." || name == "..")
        return false;
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        return false;
    return true;
}

/** @brief task_id 合法性:非空且 ≤64 字符(前端 uuid,防御非法输入) */
bool ValidTaskId(const std::string& id)
{
    return !id.empty() && id.size() <= 64;
}

/** @brief JSON 字符串转义(日志 detail 内嵌 name,防引号/反斜杠破坏 JSON;名字经 IsValidName 约束) */
std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

/** @brief RFC 5987 百分号编码(Content-Disposition filename* 用) */
std::string PercentEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out.push_back((char)c);
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

/** @brief 32B 随机 → hex64(token);失败返回 false */
bool GenToken(std::string& hex64)
{
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        return false;
    static const char* hex = "0123456789abcdef";
    hex64.clear();
    hex64.reserve(64);
    for (unsigned char b : buf)
    {
        hex64.push_back(hex[b >> 4]);
        hex64.push_back(hex[b & 0x0F]);
    }
    return true;
}

/** @brief SHA-256 → hex(分享 token 哈希) */
std::string Sha256Hex(const std::string& in)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(), digest);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest)
    {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

/** @brief 物理目录递归删除(含所有文件和子目录) */
bool DeleteDirRecursive(const std::wstring& dirPath)
{
    std::wstring searchPattern = dirPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return RemoveDirectoryW(dirPath.c_str()) != 0;

    do
    {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..")
            continue;
        std::wstring fullPath = dirPath + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            DeleteDirRecursive(fullPath);
        else
            DeleteFileW(fullPath.c_str());
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return RemoveDirectoryW(dirPath.c_str()) != 0;
}

} // namespace

// ============================================================================
// 构造 / 析构 / 初始化
// ============================================================================

FileHubModule::FileHubModule(UserModule* userModule, zm::ZmSqliteConn& fileHubDb)
    : m_userModule(userModule), m_db(fileHubDb)
{
    // 文件仓库根:exe 同级 modules\filehub(与 db/ 三库分离;静态 www 同步不影响)
    char exePath[MAX_PATH];
    ZmSystem::GetModuleDir(exePath, MAX_PATH);
    m_hubRoot = std::string(exePath) + "\\" + ZM_FILE_HUB_ROOT;

    std::wstring hubRoot = ZmString::UTF8_To_Unicode(m_hubRoot);
    CreateDirectoryW(hubRoot.c_str(), nullptr);
}

FileHubModule::~FileHubModule()
{
    Shutdown();
}

bool FileHubModule::Open()
{
    if (m_openOk.load())
        return true;

    // 库由 DbInitializer 统一打开并建表/补列;此处仅检查可用性 + 文件系统根目录
    if (!m_db.IsOpen())
    {
        DEFAULT_LOG_ERROR("[FileHub] 数据库不可用,文件中心不可用");
        return false;
    }

    // 公共空间根目录(DB 根行种子由 DbInitializer 的 dirs 表声明负责)
    std::string pubRoot = m_hubRoot + "\\0";
    CreateDirectoryW(ZmString::UTF8_To_Unicode(pubRoot).c_str(), nullptr);

    // 打包中心临时/产物目录(含 .tmp;失败不致命,打包线程按需重试)
    CreateDirectoryW(ZmString::UTF8_To_Unicode(PackTempDir()).c_str(), nullptr);
    CreateDirectoryW(ZmString::UTF8_To_Unicode(PackTempDir() + "\\.tmp").c_str(), nullptr);

    m_openOk.store(true);
    // 崩溃/关停残留恢复:启动时刻不可能有在打线程,status='packing' 必为上次进程的死行,
    // 置 failed 让后续同指纹请求走"接管重打"(否则全部成为死行等待者,§4.4 等待永不收敛)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "UPDATE filehub_packs SET status='failed',err='服务中断' WHERE status='packing'");
        if (st.p && sqlite3_step(st.p) == SQLITE_DONE && m_db.Changes() > 0)
            DEFAULT_LOG_WARN("[FileHub] 启动恢复:{} 个残留 packing 产物行置 failed",
                             m_db.Changes());
    }
    PackCleanup();   // 启动孤儿清扫(.tmp 残留/无 pack 行产物)
    DEFAULT_LOG_INFO("[FileHub] 文件中心初始化完成,仓库 {}", m_hubRoot);

    // 后台周期维护:独立事件循环线程 + 60s 周期定时器(替代原 for+sleep 轮询线程;
    // 启动后首个周期完成首轮维护,之后每日 03:00 窗口;回调内 m_gone 短路)
    m_bgLoop = new ZmEvBaseRunLoop("FileHubBgLoop");
    if (!m_bgLoop->Loop())
    {
        DEFAULT_LOG_ERROR("[FileHub] 后台维护事件循环启动失败(维护功能不可用)");
        delete m_bgLoop;
        m_bgLoop = nullptr;
    }
    else
    {
        m_bgLoop->SetTimerCallback([this]() { MaintainTick(); });
        m_bgLoop->StartTimer(60);
    }
    return true;
}

void FileHubModule::Shutdown()
{
    m_gone.store(true);
    if (m_bgLoop)
    {
        m_bgLoop->Stop();   // 优雅退出:等待当前定时器回调完成(回调入口查 m_gone 短路)
        delete m_bgLoop;
        m_bgLoop = nullptr;
    }
    // 库连接由 DbInitializer 统一关闭
}

// ============================================================================
// 空间与路径
// ============================================================================

int FileHubModule::SpaceOf(const char* scope, uint64_t uid)
{
    if (!scope)
        return -1;
    if (std::strcmp(scope, "public") == 0)
        return 0;
    if (std::strcmp(scope, "personal") == 0)
        return (int)uid;   // 个人空间强制 uid=当前用户,不可指定他人
    return -1;
}

bool FileHubModule::EnsureUserRoot(uint64_t uid)
{
    if (uid == 0)
        return false;

    // 文件系统目录
    std::string spaceAbs = m_hubRoot + "\\" + std::to_string(uid);
    if (!CreateDirectoryW(ZmString::UTF8_To_Unicode(spaceAbs).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    // DB 根行(幂等)
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "INSERT OR IGNORE INTO dirs(space,parent_id,name,create_by,create_time) "
        "VALUES(?,0,'',?,?)");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)uid);
    BindInt(st.p, 2, (int64_t)uid);
    BindInt(st.p, 3, (int64_t)time(nullptr));
    sqlite3_step(st.p);
    return true;
}

bool FileHubModule::DirAbsPath(int space, uint64_t dirId, std::string& absPath)
{
    absPath = m_hubRoot + "\\" + std::to_string(space);
    if (dirId == 0)
        return true;   // 空间根

    // 沿 parent 链拼名(自底向上,最多 64 层防环;先记当前层再判根)
    std::vector<std::string> chain;
    uint64_t cur = dirId;
    for (int depth = 0; depth < 64; ++depth)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name,parent_id FROM dirs WHERE id=? AND space=?");
        if (!st.p)
            return false;
        BindInt(st.p, 1, (int64_t)cur);
        BindInt(st.p, 2, (int64_t)space);
        if (sqlite3_step(st.p) != SQLITE_ROW)
            return false;
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        int64_t parent = sqlite3_column_int64(st.p, 1);
        chain.push_back(name ? name : "");
        if (parent == 0)
            break;   // 到达空间根
        cur = (uint64_t)parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        absPath += "\\";
        absPath += *it;
    }
    return true;
}

std::string FileHubModule::EntryAbsPath(int space, uint64_t dirId, const std::string& name)
{
    std::string dir;
    if (!DirAbsPath(space, dirId, dir))
        return "";
    return dir + "\\" + name;
}

void FileHubModule::CollectSubtreeDirs(int space, uint64_t dirId, std::vector<uint64_t>& out)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    CollectSubtreeDirsLocked(space, dirId, out);
}

void FileHubModule::CollectSubtreeDirsLocked(int space, uint64_t dirId, std::vector<uint64_t>& out)
{
    out.push_back(dirId);
    for (size_t i = 0; i < out.size(); ++i)
    {
        Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=?");
        if (!st.p)
            return;
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)out[i]);
        while (sqlite3_step(st.p) == SQLITE_ROW)
            out.push_back((uint64_t)sqlite3_column_int64(st.p, 0));
    }
}

void FileHubModule::CollectSubtreeFiles(int space, uint64_t dirId, std::vector<uint64_t>& out)
{
    std::vector<uint64_t> dirs;
    CollectSubtreeDirs(space, dirId, dirs);
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    for (uint64_t d : dirs)
    {
        Stmt st(m_db, "SELECT id FROM files WHERE space=? AND dir_id=?");
        if (!st.p)
            return;
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)d);
        while (sqlite3_step(st.p) == SQLITE_ROW)
            out.push_back((uint64_t)sqlite3_column_int64(st.p, 0));
    }
}

void FileHubModule::CollectDirSizes(int space, const std::vector<uint64_t>& roots,
                                    std::map<uint64_t, int64_t>& out)
{
    out.clear();
    if (roots.empty())
        return;

    std::string ph;
    for (size_t i = 0; i < roots.size(); ++i)
        ph += i ? ",?" : "?";

    // 一次递归 CTE:从各根向下走子树,按根分组汇总文件大小(替代逐目录 CollectSubtreeDirs+SUM)
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        ("WITH RECURSIVE t(id, root) AS ("
         "  SELECT id, id FROM dirs WHERE space=? AND id IN (" + ph + ")"
         "  UNION ALL"
         "  SELECT d.id, t.root FROM dirs d JOIN t ON d.parent_id=t.id"
         ") SELECT t.root, COALESCE(SUM(f.size),0) FROM t"
         "  LEFT JOIN files f ON f.dir_id=t.id GROUP BY t.root").c_str());
    if (!st.p)
        return;
    BindInt(st.p, 1, (int64_t)space);
    for (size_t i = 0; i < roots.size(); ++i)
        BindInt(st.p, (int)i + 2, (int64_t)roots[i]);
    while (sqlite3_step(st.p) == SQLITE_ROW)
        out[(uint64_t)sqlite3_column_int64(st.p, 0)] = sqlite3_column_int64(st.p, 1);
}

void FileHubModule::BatchRelPaths(const std::vector<uint64_t>& dirIds,
                                  std::map<uint64_t, std::string>& out)
{
    out.clear();
    if (dirIds.empty())
        return;

    // 一次拉取所有涉及目录行(id→name,parent);父链缺失迭代补查(通常 1-2 轮收敛)
    std::map<uint64_t, std::pair<std::string, uint64_t>> rows;
    std::vector<uint64_t> need = dirIds;
    for (int round = 0; round < 8 && !need.empty(); ++round)
    {
        std::string ph;
        for (size_t i = 0; i < need.size(); ++i)
            ph += i ? ",?" : "?";
        std::vector<uint64_t> next;
        std::set<uint64_t> nextSeen;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db, ("SELECT id,name,parent_id FROM dirs WHERE id IN (" + ph + ")").c_str());
            if (!st.p)
                break;
            for (size_t i = 0; i < need.size(); ++i)
                BindInt(st.p, (int)i + 1, (int64_t)need[i]);
            while (sqlite3_step(st.p) == SQLITE_ROW)
            {
                uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
                const char* nm = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
                uint64_t parent = (uint64_t)sqlite3_column_int64(st.p, 2);
                rows[id] = {nm ? nm : "", parent};
                if (parent != 0 && !rows.count(parent) && !nextSeen.count(parent))
                {
                    next.push_back(parent);
                    nextSeen.insert(parent);
                }
            }
        }
        need = next;
    }

    // 由底向上拼链,再自根向下输出(与 DirAbsPath 同语义,内存版;最多 64 层防环)
    for (uint64_t id : dirIds)
    {
        std::vector<std::string> chain;
        uint64_t cur = id;
        int depth = 0;
        while (cur != 0 && depth++ < 64)
        {
            auto it = rows.find(cur);
            if (it == rows.end())
                break;   // 目录已删:输出已收集部分
            chain.push_back(it->second.first);
            if (it->second.second == 0)
                break;
            cur = it->second.second;
        }
        std::string rel;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            if (!rel.empty())
                rel += "/";
            rel += *it;
        }
        out[id] = rel;
    }
}

// ============================================================================
// 鉴权辅助
// ============================================================================

uint64_t FileHubModule::AuthAndSpace(ZmHttpdTask* task, const char* scope, int& outSpace,
                                     std::string& outRole, std::string& outAccount)
{
    outSpace = -1;
    if (!m_userModule)
        return 0;

    UserModule::UserInfo ui;
    uint64_t uid = m_userModule->AuthAndTouch(task, &ui);
    if (!uid)
        return 0;
    outAccount = ui.account;

    outSpace = SpaceOf(scope, uid);
    if (outSpace < 0)
        return 0;

    m_userModule->GetUserRole(uid, outRole);
    return uid;
}

bool FileHubModule::CanModify(const std::string& role, uint64_t opUid,
                              int space, uint64_t uploaderId, std::string& errText)
{
    if (space == 0)
    {
        // 公共空间:仅上传者本人或 developer/admin
        if (uploaderId == opUid)
            return true;
        if (role == "developer" || role == "admin")
            return true;
        errText = "公共空间文件仅创建者可修改,或由管理员处理";
        return false;
    }
    // 个人空间:仅本人
    if (opUid != (uint64_t)space)
    {
        errText = "个人空间仅本人可操作";
        return false;
    }
    return true;
}

void FileHubModule::Log(uint64_t opId, const std::string& opAccount,
                        const std::string& action, const std::string& targetType,
                        uint64_t targetId, const std::string& detail)
{
    if (m_userModule)
        m_userModule->WriteBusinessLog(UserModule::BizLogTable::FileHubLogs, opId, opAccount, action,
                                       targetType, targetId, detail);
}

// ============================================================================
// 页面端口路由:分享链接 /share/<token> → 302 转发 REST 端口 39441
// ============================================================================

void FileHubModule::RegisterHttpRoutes(HttpFrontendManager* httpMgr)
{
    if (!httpMgr)
        return;

    auto& router = httpMgr->GetRouter();

    // 分享链接入口:页面端口(80/443)统一 302 到 REST 端口(公共分享免登录可达;
    // 个人分享在 REST 侧 302 回登录页)
    router.Any("/share/*", [](ZmHttpdTask* task, const BYTE*, size_t) {
        std::string uri = task->Uri() ? task->Uri() : "/";
        if (uri.rfind("/share/", 0) != 0)
            return 404;
        std::string token = uri.substr(7);
        const char* host = task->GetRequestHeader("Host");
        std::string location = std::string("https://") + (host ? host : "localhost") +
                               ":39441/zimo/api/share/" + token;
        task->SetReply(302, "Found");
        task->PutReplyHeader("Location", location.c_str());
        return 302;
    });
}

// ============================================================================
// REST 分发
// ============================================================================

bool FileHubModule::DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                                 ZmHttpdTask* task, const BYTE* body, size_t bodyLen)
{
    // /share/<token>:分享访问(独立顶层路径,免登录可达)
    if (path.rfind("/share/", 0) == 0)
    {
        if (verb != EVHTTP_REQ_GET)
        {
            ZmReqLoopRest::ResponseError(loop, 405, "Method Not Allowed");
            return true;
        }
        HandleShareAccess(loop, task, path.substr(7));
        return true;
    }

    // /portal/filehubAdmin/sync:文件中心管理(管理模块)。
    // 注意须先于下方 "/portal/filehub" 前缀分支判定——"/portal/filehubAdmin"
    // 同样以 "/portal/filehub" 开头,后置会被普通文件中心分支吞入。
    if (path == "/portal/filehubAdmin/sync")
    {
        if (verb != EVHTTP_REQ_POST)
        {
            ZmReqLoopRest::ResponseError(loop, 405, "Method Not Allowed");
            return true;
        }
        if (!m_openOk.load())
        {
            ZmReqLoopRest::ResponseError(loop, 503, "文件中心未初始化");
            return true;
        }
        // 鉴权 + 权限:可见模块须含 filehubAdmin(admin 提升自动授权;未授权 403,防接口直调绕过前端目录隐藏)
        if (m_userModule)
        {
            UserModule::UserInfo ui;
            uint64_t uid = m_userModule->AuthAndTouch(task, &ui);
            if (!uid)
            {
                ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
                return true;
            }
            std::string role;
            m_userModule->GetUserRole(uid, role);
            std::vector<ZMJSON> mods;
            if (!m_userModule->GetUserModules(uid, role, mods))
            {
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return true;
            }
            bool has = false;
            for (auto& m : mods)
            {
                if (zm_json_get_str(m, "code") == "filehubAdmin")
                {
                    has = true;
                    break;
                }
            }
            if (!has)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "无文件中心管理模块权限");
                return true;
            }
        }
        HandleAdminSync(loop);
        return true;
    }

    // /portal/filehub/*:门户文件中心
    if (path.rfind("/portal/filehub", 0) != 0)
        return false;

    if (!m_openOk.load())
    {
        ZmReqLoopRest::ResponseError(loop, 503, "文件中心未初始化");
        return true;
    }

    // 模块权限兜底:可见模块须含 filehub(未授权 403,防接口直调绕过前端目录隐藏)
    if (m_userModule)
    {
        UserModule::UserInfo ui;
        uint64_t uid = m_userModule->AuthAndTouch(task, &ui);
        if (uid)
        {
            std::string role;
            m_userModule->GetUserRole(uid, role);
            std::vector<ZMJSON> mods;
            if (m_userModule->GetUserModules(uid, role, mods))
            {
                bool has = false;
                for (auto& m : mods)
                {
                    if (zm_json_get_str(m, "code") == "filehub")
                    {
                        has = true;
                        break;
                    }
                }
                if (!has)
                {
                    ZmReqLoopRest::ResponseError(loop, 403, "无文件中心模块权限");
                    return true;
                }
            }
        }
    }

    // GET 端点
    if (verb == EVHTTP_REQ_GET)
    {
        if (path == "/portal/filehub/list")
        {
            const char* scope = task->GetQueryValue("space", "");
            std::string role, account;
            int space = -1;
            uint64_t uid = AuthAndSpace(task, scope, space, role, account);
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            if (space < 0) { ZmReqLoopRest::ResponseError(loop, 400, "space 参数非法"); return true; }
            HandleList(loop, task, space, uid);
            return true;
        }
        if (path == "/portal/filehub/search")
        {
            const char* scope = task->GetQueryValue("space", "");
            std::string role, account;
            int space = -1;
            uint64_t uid = AuthAndSpace(task, scope, space, role, account);
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            if (space < 0) { ZmReqLoopRest::ResponseError(loop, 400, "space 参数非法"); return true; }
            HandleSearch(loop, task, space);
            return true;
        }
        if (path == "/portal/filehub/download")
        {
            const char* scope = task->GetQueryValue("space", "");
            std::string role, account;
            int space = -1;
            uint64_t uid = AuthAndSpace(task, scope, space, role, account);
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            if (space < 0) { ZmReqLoopRest::ResponseError(loop, 400, "space 参数非法"); return true; }
            HandleDownload(loop, task, uid, account);
            return true;
        }
        // 传输任务:历史分页 / zip 打包状态轮询 / zip 直链流式打包下载
        if (path == "/portal/filehub/tasks")
        {
            // 历史查询:无需 space 参数,直接鉴权
            UserModule::UserInfo ui;
            uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            HandleTasks(loop, task, uid);
            return true;
        }
        if (path == "/portal/filehub/task_status")
        {
            UserModule::UserInfo ui;
            uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            HandleTaskStatus(loop, task, uid);
            return true;
        }
        // 打包任务下发(分块/零拷贝,Range 续传):GET 直链
        if (path == "/portal/filehub/zip_task_download")
        {
            UserModule::UserInfo ui;
            uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            HandleZipTaskDownload(loop, task, uid);
            return true;
        }
        if (path == "/portal/filehub/shares")
        {
            // 我的分享:无需 space 参数,直接鉴权
            UserModule::UserInfo ui;
            uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
            if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
            HandleShareList(loop, uid);
            return true;
        }
        ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
        return true;
    }

    if (verb != EVHTTP_REQ_POST)
    {
        ZmReqLoopRest::ResponseError(loop, 405, "Method Not Allowed");
        return true;
    }

    // POST 端点(解析 body;upload 除外,其 body 为文件流)
    ZMJSON req;
    if (path != "/portal/filehub/upload")
    {
        std::string bodyStr(body ? reinterpret_cast<const char*>(body) : "", bodyLen);
        if (!bodyStr.empty())
        {
            std::string err;
            req = zm_json_parse(bodyStr, err);
            if (!err.empty())
            {
                ZmReqLoopRest::ResponseError(loop, 400, "请求体不是合法 JSON");
                return true;
            }
        }
    }

    // zip 打包发起(POST;query 带 task_id/ids,与 GET 下载窗口无关)
    if (path == "/portal/filehub/zip_download")
    {
        UserModule::UserInfo ui;
        uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
        if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
        HandleZipStart(loop, task, uid, ui.account);
        return true;
    }

    // share/unshare/share_commit 目标自带空间,不要求 space 参数(仅需登录)
    if (path == "/portal/filehub/share" || path == "/portal/filehub/unshare" ||
        path == "/portal/filehub/share_commit")
    {
        std::string role, account;
        int space = -1;
        uint64_t uid = AuthAndSpace(task, "public", space, role, account);
        if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
        if (path == "/portal/filehub/share")
            HandleShareCreate(loop, req, uid, account);
        else if (path == "/portal/filehub/share_commit")
            HandleShareCommit(loop, req, uid, account);
        else
            HandleShareCancel(loop, req, uid, account);
        return true;
    }

    // 传输任务管理(task_create/cancel/delete)不带空间语义,仅需登录(独立鉴权)
    if (path == "/portal/filehub/task_create" || path == "/portal/filehub/task_cancel" ||
        path == "/portal/filehub/task_delete")
    {
        UserModule::UserInfo ui;
        uint64_t uid = m_userModule ? m_userModule->AuthAndTouch(task, &ui) : 0;
        if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
        if (path == "/portal/filehub/task_create")
            HandleTaskCreate(loop, req, uid);
        else if (path == "/portal/filehub/task_cancel")
            HandleTaskCancel(loop, req, uid);
        else
            HandleTaskDelete(loop, req, uid);
        return true;
    }

    const char* scope = task->GetQueryValue("space", "");
    std::string role, account;
    int space = -1;
    uint64_t uid = AuthAndSpace(task, scope, space, role, account);
    if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
    if (space < 0) { ZmReqLoopRest::ResponseError(loop, 400, "space 参数非法"); return true; }

    if (path == "/portal/filehub/upload")
    {
        HandleUpload(loop, task, space, uid, account);
        return true;
    }
    if (path == "/portal/filehub/mkdir")
    {
        HandleMkdir(loop, req, space, uid, account);
        return true;
    }
    if (path == "/portal/filehub/rename")
    {
        HandleRename(loop, req, uid, role, account);
        return true;
    }
    if (path == "/portal/filehub/move")
    {
        HandleMove(loop, req, uid, role, account);
        return true;
    }
    if (path == "/portal/filehub/copy")
    {
        HandleCopy(loop, req, uid, role, account);
        return true;
    }
    if (path == "/portal/filehub/delete")
    {
        HandleDelete(loop, req, uid, role, account);
        return true;
    }
    ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
    return true;
}

// ============================================================================
// 列表 / 搜索
// ============================================================================

void FileHubModule::BuildPathChain(ZMJSON& pathArr, int space, uint64_t dirId)
{
    std::vector<uint64_t> stack;
    uint64_t cur = dirId;
    while (cur != 0)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name,parent_id FROM dirs WHERE id=? AND space=?");
        if (!st.p)
            break;
        BindInt(st.p, 1, (int64_t)cur);
        BindInt(st.p, 2, (int64_t)space);
        if (sqlite3_step(st.p) != SQLITE_ROW)
            break;
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        int64_t parent = sqlite3_column_int64(st.p, 1);
        stack.push_back(cur);
        if (parent == 0)
            break;
        cur = (uint64_t)parent;
    }
    // 根段
    ZMJSON root;
    root["id"] = (int64_t)0;
    root["name"] = space == 0 ? "公共文件夹" : "个人文件夹";
    pathArr.push_back(root);
    // 目录链(自上而下)
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name FROM dirs WHERE id=?");
        if (!st.p)
            break;
        BindInt(st.p, 1, (int64_t)*it);
        if (sqlite3_step(st.p) == SQLITE_ROW)
        {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
            ZMJSON seg;
            seg["id"] = (int64_t)*it;
            seg["name"] = name ? name : "";
            pathArr.push_back(seg);
        }
    }
}

void FileHubModule::HandleList(ZmReqLoop* loop, ZmHttpdTask* task, int space, uint64_t opUid)
{
    if (space != 0)
        EnsureUserRoot((uint64_t)space);   // 个人空间懒创建

    uint64_t dirId = (uint64_t)strtoull(task->GetQueryValue("dir_id", "0"), nullptr, 10);

    // 复合排序:sort=key1,key2 & order=asc,desc(白名单 name/size/mtime/type,最多 4 级)
    std::vector<std::pair<std::string, std::string>> sorts;
    {
        std::string s = task->GetQueryValue("sort", "name");
        std::string o = task->GetQueryValue("order", "asc");
        std::vector<std::string> ks, os;
        auto split = [](const std::string& in, std::vector<std::string>& out) {
            std::string cur;
            for (char c : in)
            {
                if (c == ',') { out.push_back(cur); cur.clear(); }
                else cur.push_back(c);
            }
            out.push_back(cur);
        };
        split(s, ks);
        split(o, os);
        for (size_t i = 0; i < ks.size() && i < 4; ++i)
        {
            std::string k = ks[i];
            bool desc = i < os.size() && os[i] == "desc";
            if (k == "name" || k == "size" || k == "mtime" || k == "type")
                sorts.push_back({k, desc ? "DESC" : "ASC"});
        }
        if (sorts.empty())
            sorts.push_back({"name", "ASC"});
    }

    if (!DirExists(space, dirId))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
        return;
    }

    ZMJSON rsp;
    rsp["result"]["space"] = space == 0 ? "public" : "personal";
    BuildPathChain(rsp["result"]["path"], space, dirId);

    // 目录:按第一个排序键的方向排 name;文件:完整复合排序
    std::string dirOrder = std::string(" ORDER BY name ") +
                           (sorts[0].second == "DESC" ? "DESC" : "ASC");
    std::string fileOrder = " ORDER BY ";
    for (size_t i = 0; i < sorts.size(); ++i)
    {
        const auto& [k, dir] = sorts[i];
        if (i) fileOrder += ", ";
        if (k == "size")
            fileOrder += "size " + dir;
        else if (k == "mtime")
            fileOrder += "mtime " + dir;
        else if (k == "type")
            fileOrder += "CASE WHEN instr(name,'.')=0 THEN '' "
                         "ELSE lower(substr(name, instr(name,'.')+1)) END " + dir;
        else
            fileOrder += "name " + dir;
    }

    // 目录行(先查后算:批量大小/账号另取锁,不能在 m_db.Mutex() 内调用)
    std::vector<std::tuple<uint64_t, std::string, uint64_t, int64_t>> dirRows;   // id,name,create_by,create_time
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, (std::string(
            "SELECT id,name,create_by,create_time FROM dirs WHERE space=? AND parent_id=? "
            "AND name != ''") + dirOrder).c_str());
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)dirId);
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            dirRows.emplace_back(id, name ? name : "",
                                 (uint64_t)sqlite3_column_int64(st.p, 2),   // create_by
                                 sqlite3_column_int64(st.p, 3));            // create_time
        }
    }
    // 批量:一次递归 CTE 取全部子目录子树大小 + 一次跨库 IN 查创建者账号(替代 N+1)
    std::vector<uint64_t> childIds, accountIds;
    for (const auto& row : dirRows)
    {
        childIds.push_back(std::get<0>(row));
        if (std::get<2>(row) != 0)
            accountIds.push_back(std::get<2>(row));
    }
    std::map<uint64_t, int64_t> dirSizes;
    CollectDirSizes(space, childIds, dirSizes);
    std::map<uint64_t, std::string> accounts;
    m_userModule->BatchGetUserAccounts(accountIds, accounts);

    for (const auto& row : dirRows)
    {
        ZMJSON d;
        d["id"] = (int64_t)std::get<0>(row);
        d["name"] = std::get<1>(row);
        auto sit = dirSizes.find(std::get<0>(row));
        d["dirSize"] = sit != dirSizes.end() ? sit->second : 0;
        d["mtime"] = std::get<3>(row);
        d["createBy"] = (int64_t)std::get<2>(row);
        d["createTime"] = std::get<3>(row);
        uint64_t cb = std::get<2>(row);
        auto ait = accounts.find(cb);
        if (cb != 0 && ait != accounts.end())
            d["creator"] = ait->second;
        else
            d["creator"] = "—";
        rsp["result"]["dirs"].push_back(d);
    }

    // 文件行:锁内收集(含 uploader_id),锁外批量取上传者账号
    std::vector<std::tuple<uint64_t, std::string, int64_t, int64_t, uint64_t, int64_t>> fileRows;
    // id,name,size,mtime,uploader_id,upload_time
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, (std::string(
            "SELECT id,name,size,mtime,uploader_id,upload_time FROM files "
            "WHERE space=? AND dir_id=? ") + fileOrder).c_str());
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)dirId);
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            uint64_t upId = (uint64_t)sqlite3_column_int64(st.p, 4);
            fileRows.emplace_back(id, name ? name : "",
                                  sqlite3_column_int64(st.p, 2), sqlite3_column_int64(st.p, 3),
                                  upId, sqlite3_column_int64(st.p, 5));
        }
    }
    accountIds.clear();
    for (const auto& row : fileRows)
        if (std::get<4>(row) != 0)
            accountIds.push_back(std::get<4>(row));
    accounts.clear();
    m_userModule->BatchGetUserAccounts(accountIds, accounts);

    for (const auto& row : fileRows)
    {
        ZMJSON f;
        f["id"] = (int64_t)std::get<0>(row);
        f["name"] = std::get<1>(row);
        f["size"] = std::get<2>(row);
        f["mtime"] = std::get<3>(row);
        uint64_t uploaderId = std::get<4>(row);
        f["uploaderId"] = (int64_t)uploaderId;
        f["uploadTime"] = (int64_t)std::get<5>(row);
        auto ait = accounts.find(uploaderId);
        if (uploaderId != 0 && ait != accounts.end())
            f["uploader"] = ait->second;
        else
            f["uploader"] = "—";
        std::string ext;
        size_t dot = std::get<1>(row).rfind('.');
        if (dot != std::string::npos && dot + 1 < std::get<1>(row).size())
        {
            ext = std::get<1>(row).substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }
        f["type"] = ext;
        rsp["result"]["files"].push_back(f);
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

void FileHubModule::HandleSearch(ZmReqLoop* loop, ZmHttpdTask* task, int space)
{
    const char* kw = task->GetQueryValue("keyword", "");
    if (!kw || !kw[0])
    {
        ZmReqLoopRest::ResponseError(loop, 400, "keyword 不能为空");
        return;
    }
    std::string like = "%" + std::string(kw) + "%";

    ZMJSON rsp;
    rsp["result"]["space"] = space == 0 ? "public" : "personal";

    // 文件名匹配(先查后算:批量路径/大小/账号另取锁)
    std::vector<std::tuple<uint64_t, std::string, uint64_t, int64_t, int64_t, uint64_t>> fileRows;
    // id,name,dir_id,size,mtime,uploader_id
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT id,name,dir_id,size,mtime,uploader_id FROM files "
                      "WHERE space=? AND name LIKE ? ORDER BY name");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)space);
        BindText(st.p, 2, like);
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            uint64_t dirId = (uint64_t)sqlite3_column_int64(st.p, 2);
            fileRows.emplace_back(id, name ? name : "", dirId,
                                  sqlite3_column_int64(st.p, 3), sqlite3_column_int64(st.p, 4),
                                  (uint64_t)sqlite3_column_int64(st.p, 5));
        }
    }
    // 目录名匹配(先查后算:批量大小/路径/账号另取锁)
    std::vector<std::tuple<uint64_t, std::string, uint64_t, uint64_t, int64_t>> dirRows;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT id,name,parent_id,create_by,create_time FROM dirs "
                      "WHERE space=? AND name LIKE ?  ORDER BY name");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)space);
        BindText(st.p, 2, like);
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            uint64_t parentId = (uint64_t)sqlite3_column_int64(st.p, 2);
            dirRows.emplace_back(id, name ? name : "", parentId,
                                 (uint64_t)sqlite3_column_int64(st.p, 3),
                                 sqlite3_column_int64(st.p, 4));
        }
    }
    // 批量:匹配目录的子树大小(一次 CTE)+ 全部相对路径(一次拉行)+ 账号(一次跨库 IN)
    std::vector<uint64_t> sizeRoots, relIds, accountIds;
    for (const auto& row : fileRows)
    {
        relIds.push_back(std::get<2>(row));
        if (std::get<5>(row) != 0)
            accountIds.push_back(std::get<5>(row));
    }
    for (const auto& row : dirRows)
    {
        sizeRoots.push_back(std::get<0>(row));
        relIds.push_back(std::get<2>(row));
        if (std::get<3>(row) != 0)
            accountIds.push_back(std::get<3>(row));
    }
    std::map<uint64_t, int64_t> dirSizes;
    CollectDirSizes(space, sizeRoots, dirSizes);
    std::map<uint64_t, std::string> relPaths;
    BatchRelPaths(relIds, relPaths);
    std::map<uint64_t, std::string> accounts;
    m_userModule->BatchGetUserAccounts(accountIds, accounts);

    // relPath 组装:dirId 链 + "/" + 名称(根目录链为空时直接返回名称)
    auto relPathOf = [&relPaths](uint64_t dirId, const std::string& name) -> std::string {
        auto it = relPaths.find(dirId);
        if (it == relPaths.end() || it->second.empty())
            return name;
        return it->second + "/" + name;
    };

    for (const auto& row : fileRows)
    {
        ZMJSON f;
        f["id"] = (int64_t)std::get<0>(row);
        f["type"] = "file";
        f["name"] = std::get<1>(row);
        f["size"] = std::get<3>(row);
        f["mtime"] = std::get<4>(row);
        f["relPath"] = relPathOf(std::get<2>(row), std::get<1>(row));
        uint64_t upId = std::get<5>(row);
        auto ait = accounts.find(upId);
        if (upId != 0 && ait != accounts.end())
            f["uploader"] = ait->second;
        else
            f["uploader"] = "—";
        rsp["result"]["items"].push_back(f);
    }
    for (const auto& row : dirRows)
    {
        ZMJSON d;
        d["id"] = (int64_t)std::get<0>(row);
        d["type"] = "dir";
        d["name"] = std::get<1>(row);
        auto sit = dirSizes.find(std::get<0>(row));
        d["dirSize"] = sit != dirSizes.end() ? sit->second : 0;
        d["relPath"] = relPathOf(std::get<2>(row), std::get<1>(row));
        d["createBy"] = (int64_t)std::get<3>(row);
        d["createTime"] = std::get<4>(row);
        uint64_t cb = std::get<3>(row);
        auto ait = accounts.find(cb);
        if (cb != 0 && ait != accounts.end())
            d["creator"] = ait->second;
        else
            d["creator"] = "—";
        rsp["result"]["items"].push_back(d);
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

// ============================================================================
// 写操作
// ============================================================================

bool FileHubModule::ParseIdList(const ZMJSON& body,
                                std::vector<std::pair<std::string, uint64_t>>& out)
{
    out.clear();
    if (!body.contains("ids") || !body["ids"].is_array())
        return false;
    for (auto& it : body["ids"])
    {
        std::string type = zm_json_get_str(it, "type");
        uint64_t id = (uint64_t)JsonInt(it, "id", 0);
        if ((type != "file" && type != "dir") || id == 0)
            return false;
        out.push_back({type, id});
    }
    return !out.empty();
}

bool FileHubModule::DirExists(int space, uint64_t dirId)
{
    if (dirId == 0)
        return true;   // 空间根恒存在
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db, "SELECT 1 FROM dirs WHERE id=? AND space=?");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)dirId);
    BindInt(st.p, 2, (int64_t)space);
    return sqlite3_step(st.p) == SQLITE_ROW;
}

void FileHubModule::HandleMkdir(ZmReqLoop* loop, const ZMJSON& body, int space, uint64_t opUid,
                                const std::string& opAccount)
{
    uint64_t parentId = (uint64_t)JsonInt(body, "parent_id", 0);
    std::string name = zm_json_get_str(body, "name");
    if (!IsValidName(name))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "目录名称不合法");
        return;
    }
    if (!DirExists(space, parentId))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "父目录不存在");
        return;
    }

    std::string parentAbs;
    if (!DirAbsPath(space, parentId, parentAbs))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    // 同名冲突检查(幂等 mkdir:目录已存在 → 200 + 已有 dirId;同名文件 → 409)
    uint64_t existedId = 0;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)parentId);
        BindText(st.p, 3, name);
        if (sqlite3_step(st.p) == SQLITE_ROW)
        {
            existedId = (uint64_t)sqlite3_column_int64(st.p, 0);
            ZmReqLoopRest::ResponseJson(loop, 200,
                {{"result", {{"ok", true}, {"dirId", (int64_t)existedId}, {"existed", true}}}});
            return;
        }
        Stmt st2(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
        if (!st2.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st2.p, 1, (int64_t)space);
        BindInt(st2.p, 2, (int64_t)parentId);
        BindText(st2.p, 3, name);
        if (sqlite3_step(st2.p) == SQLITE_ROW)
        {
            ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
            return;
        }
    }

    // 文件系统创建(已存在 → 幂等复用;其他失败 → 500)
    std::string newAbs = parentAbs + "\\" + name;
    if (!CreateDirectoryW(ZmString::UTF8_To_Unicode(newAbs).c_str(), nullptr))
    {
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            // 物理目录已存在但 DB 无行(漂移/并发窗口):补查 dirs 行;查不到则补行自愈,
            // 幂等返回(不再误报 409)
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
            if (st.p)
            {
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)parentId);
                BindText(st.p, 3, name);
                if (sqlite3_step(st.p) == SQLITE_ROW)
                {
                    existedId = (uint64_t)sqlite3_column_int64(st.p, 0);
                    ZmReqLoopRest::ResponseJson(loop, 200,
                        {{"result", {{"ok", true}, {"dirId", (int64_t)existedId}, {"existed", true}}}});
                    return;
                }
            }
            Stmt ins(m_db,
                "INSERT INTO dirs(space,parent_id,name,create_by,create_time) VALUES(?,?,?,?,?)");
            if (ins.p)
            {
                BindInt(ins.p, 1, (int64_t)space);
                BindInt(ins.p, 2, (int64_t)parentId);
                BindText(ins.p, 3, name);
                BindInt(ins.p, 4, (int64_t)opUid);
                BindInt(ins.p, 5, (int64_t)time(nullptr));
                if (sqlite3_step(ins.p) == SQLITE_DONE)
                {
                    existedId = (uint64_t)m_db.LastInsertRowId();
                    DEFAULT_LOG_INFO("[FileHub] mkdir 漂移自愈:space={} 补行 {}", space, name);
                    ZmReqLoopRest::ResponseJson(loop, 200,
                        {{"result", {{"ok", true}, {"dirId", (int64_t)existedId}, {"existed", true}}}});
                    return;
                }
            }
            ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
            return;
        }
        ZmReqLoopRest::ResponseError(loop, 500, "创建目录失败");
        return;
    }

    // DB 写入;失败回滚文件系统
    uint64_t newDirId = 0;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "INSERT INTO dirs(space,parent_id,name,create_by,create_time) VALUES(?,?,?,?,?)");
        if (!st.p)
        {
            DeleteDirRecursive(ZmString::UTF8_To_Unicode(newAbs).c_str());
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)parentId);
        BindText(st.p, 3, name);
        BindInt(st.p, 4, (int64_t)opUid);
        BindInt(st.p, 5, (int64_t)time(nullptr));
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            DeleteDirRecursive(ZmString::UTF8_To_Unicode(newAbs).c_str());
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        newDirId = (uint64_t)m_db.LastInsertRowId();
    }

    Log(opUid, opAccount, "mkdir", "dir", newDirId, "{\"name\":\"" + name + "\"}");
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"ok", true}, {"dirId", (int64_t)newDirId}}}});
}

void FileHubModule::HandleRename(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                 const std::string& opRole, const std::string& opAccount)
{
    std::string type = zm_json_get_str(body, "type");
    uint64_t id = (uint64_t)JsonInt(body, "id", 0);
    std::string newName = zm_json_get_str(body, "new_name");
    if ((type != "file" && type != "dir") || id == 0 || !IsValidName(newName))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    // 查目标行
    int space = -1;
    uint64_t dirId = 0;
    uint64_t uploaderId = 0;
    std::string oldName;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (type == "file")
        {
            Stmt st(m_db, "SELECT space,dir_id,name,uploader_id FROM files WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                return;
            }
            space = (int)sqlite3_column_int64(st.p, 0);
            dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
            oldName = n ? n : "";
            uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
        }
        else
        {
            Stmt st(m_db, "SELECT space,parent_id,name,create_by FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                return;
            }
            space = (int)sqlite3_column_int64(st.p, 0);
            dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
            oldName = n ? n : "";
            uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
        }
    }

    // 权限
    std::string permErr;
    if (!CanModify(opRole, opUid, space, uploaderId, permErr))
    {
        ZmReqLoopRest::ResponseError(loop, 403, permErr);
        return;
    }
    if (newName == oldName)
    {
        ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
        return;
    }

    // 同名冲突(同空间同目录)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (type == "file")
        {
            Stmt st(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)space);
            BindInt(st.p, 2, (int64_t)dirId);
            BindText(st.p, 3, newName);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                return;
            }
        }
        else
        {
            Stmt st(m_db, "SELECT 1 FROM dirs WHERE space=? AND parent_id=? AND name=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)space);
            BindInt(st.p, 2, (int64_t)dirId);
            BindText(st.p, 3, newName);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                return;
            }
        }
    }

    // 文件系统改名
    std::string oldAbs = EntryAbsPath(space, dirId, oldName);
    std::string newAbs = EntryAbsPath(space, dirId, newName);
    if (oldAbs.empty() || newAbs.empty() ||
        !MoveFileW(ZmString::UTF8_To_Unicode(oldAbs).c_str(),
                   ZmString::UTF8_To_Unicode(newAbs).c_str()))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "重命名失败");
        return;
    }

    // DB 更新;失败回滚(先 bind 再 step,防未绑定参数静默改 0 行)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (type == "file")
        {
            Stmt st(m_db, "UPDATE files SET name=? WHERE id=?");
            if (!st.p)
            {
                MoveFileW(ZmString::UTF8_To_Unicode(newAbs).c_str(),
                          ZmString::UTF8_To_Unicode(oldAbs).c_str());
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return;
            }
            BindText(st.p, 1, newName);
            BindInt(st.p, 2, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_DONE || m_db.Changes() == 0)
            {
                MoveFileW(ZmString::UTF8_To_Unicode(newAbs).c_str(),
                          ZmString::UTF8_To_Unicode(oldAbs).c_str());
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return;
            }
        }
        else
        {
            Stmt st(m_db, "UPDATE dirs SET name=? WHERE id=?");
            if (!st.p)
            {
                MoveFileW(ZmString::UTF8_To_Unicode(newAbs).c_str(),
                          ZmString::UTF8_To_Unicode(oldAbs).c_str());
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return;
            }
            BindText(st.p, 1, newName);
            BindInt(st.p, 2, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_DONE)
            {
                MoveFileW(ZmString::UTF8_To_Unicode(newAbs).c_str(),
                          ZmString::UTF8_To_Unicode(oldAbs).c_str());
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return;
            }
        }
    }

    Log(opUid, opAccount, "rename", type, id,
        "{\"from\":\"" + oldName + "\",\"to\":\"" + newName + "\"}");
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void FileHubModule::HandleMove(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                               const std::string& opRole, const std::string& opAccount)
{
    std::vector<std::pair<std::string, uint64_t>> ids;
    if (!ParseIdList(body, ids))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    uint64_t targetDir = (uint64_t)JsonInt(body, "target_dir_id", 0);

    // 目标目录存在性 + 空间(targetDir==0 = 各条目自身空间根,空间校验跳过)
    int tgtSpace = -1;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (targetDir == 0)
        {
            // 移动到本空间根:不设 tgtSpace,条目循环内跳过空间校验
        }
        else
        {
            Stmt st(m_db, "SELECT space FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)targetDir);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目标目录不存在");
                return;
            }
            tgtSpace = (int)sqlite3_column_int64(st.p, 0);
        }
    }

    // 逐项校验与执行
    for (auto& it : ids)
    {
        const std::string& type = it.first;
        uint64_t id = it.second;
        int space = -1;
        uint64_t dirId = 0, uploaderId = 0;
        std::string name;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            if (type == "file")
            {
                Stmt st(m_db, "SELECT space,dir_id,name,uploader_id FROM files WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
            else
            {
                Stmt st(m_db, "SELECT space,parent_id,name,create_by FROM dirs WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
        }

        // 移动仅同空间(targetDir==0 时目标为条目自身空间根,天然同空间)
        if (tgtSpace >= 0 && space != tgtSpace)
        {
            ZmReqLoopRest::ResponseError(loop, 403, "移动仅限同一空间");
            return;
        }
        // 权限
        std::string permErr;
    if (!CanModify(opRole, opUid, space, uploaderId, permErr))
        {
            ZmReqLoopRest::ResponseError(loop, 403, permErr);
            return;
        }
        // 目录:目标不得为自身或其子树
        if (type == "dir")
        {
            std::vector<uint64_t> subs;
            CollectSubtreeDirs(space, id, subs);
            if (std::find(subs.begin(), subs.end(), targetDir) != subs.end())
            {
                ZmReqLoopRest::ResponseError(loop, 403, "不能移动到自身或其子目录");
                return;
            }
        }
        // 目标同名冲突
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            if (type == "file")
            {
                Stmt st(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)targetDir);
                BindText(st.p, 3, name);
                if (sqlite3_step(st.p) == SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                    return;
                }
            }
            else
            {
                Stmt st(m_db, "SELECT 1 FROM dirs WHERE space=? AND parent_id=? AND name=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)targetDir);
                BindText(st.p, 3, name);
                if (sqlite3_step(st.p) == SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                    return;
                }
            }
        }

        // 文件系统移动
        std::string srcAbs = EntryAbsPath(space, dirId, name);
        std::string tgtAbs;
        if (!DirAbsPath(space, targetDir, tgtAbs))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        std::string dstAbs = tgtAbs + "\\" + name;
        if (!MoveFileW(ZmString::UTF8_To_Unicode(srcAbs).c_str(),
                       ZmString::UTF8_To_Unicode(dstAbs).c_str()))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "移动失败");
            return;
        }

        // DB 更新(文件改 dir_id;目录改 parent_id);失败回滚
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            if (type == "file")
            {
                Stmt st(m_db, "UPDATE files SET dir_id=? WHERE id=?");
                if (!st.p)
                {
                    MoveFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str(),
                              ZmString::UTF8_To_Unicode(srcAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
                BindInt(st.p, 1, (int64_t)targetDir);
                BindInt(st.p, 2, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_DONE)
                {
                    MoveFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str(),
                              ZmString::UTF8_To_Unicode(srcAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
            }
            else
            {
                Stmt st(m_db, "UPDATE dirs SET parent_id=? WHERE id=?");
                if (!st.p)
                {
                    MoveFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str(),
                              ZmString::UTF8_To_Unicode(srcAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
                BindInt(st.p, 1, (int64_t)targetDir);
                BindInt(st.p, 2, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_DONE)
                {
                    MoveFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str(),
                              ZmString::UTF8_To_Unicode(srcAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
            }
        }
        Log(opUid, opAccount, "move", type, id,
            "{\"to_dir\":" + std::to_string(targetDir) + "}");
    }
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void FileHubModule::HandleCopy(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                               const std::string& opRole, const std::string& opAccount)
{
    std::vector<std::pair<std::string, uint64_t>> ids;
    if (!ParseIdList(body, ids))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    uint64_t targetDir = (uint64_t)JsonInt(body, "target_dir_id", 0);

    // 目标目录存在性 + 空间(targetDir==0 = 空间根,需 target_space 指定目标空间)
    int tgtSpace = -1;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (targetDir == 0)
        {
            // target_space:数字 uid / "public" / "personal"(当前用户个人空间)
            std::string ts = zm_json_get_str(body, "target_space");
            if (ts == "public") tgtSpace = 0;
            else if (ts == "personal") tgtSpace = (int)opUid;
            else tgtSpace = (int)JsonInt(body, "target_space", -1);
            if (tgtSpace < 0)
            {
                ZmReqLoopRest::ResponseError(loop, 400, "请指定目标空间");
                return;
            }
            // 个人空间根仅本人
            if (tgtSpace != 0 && opUid != (uint64_t)tgtSpace)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "不能复制到他人个人空间");
                return;
            }
        }
        else
        {
            Stmt st(m_db, "SELECT space FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)targetDir);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目标目录不存在");
                return;
            }
            tgtSpace = (int)sqlite3_column_int64(st.p, 0);
        }
    }
    // 个人目标空间仅本人
    if (tgtSpace != 0 && opUid != (uint64_t)tgtSpace)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "不能复制到他人个人空间");
        return;
    }

    // 个人目标空间懒创建
    if (tgtSpace != 0)
        EnsureUserRoot((uint64_t)tgtSpace);

    for (auto& it : ids)
    {
        const std::string& type = it.first;
        uint64_t id = it.second;
        int space = -1;
        uint64_t dirId = 0, uploaderId = 0;
        std::string name;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            if (type == "file")
            {
                Stmt st(m_db, "SELECT space,dir_id,name,uploader_id FROM files WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
            else
            {
                Stmt st(m_db, "SELECT space,parent_id,name,create_by FROM dirs WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
        }

        // 复制权限:公共文件任何登录用户;个人仅本人;个人源空间他人 403
        if (space != 0 && opUid != (uint64_t)space)
        {
            ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可操作");
            return;
        }
        // 目录复制:目标不得为自身或其子树(同空间时)
        if (type == "dir" && space == tgtSpace)
        {
            std::vector<uint64_t> subs;
            CollectSubtreeDirs(space, id, subs);
            if (std::find(subs.begin(), subs.end(), targetDir) != subs.end())
            {
                ZmReqLoopRest::ResponseError(loop, 403, "不能复制到自身或其子目录");
                return;
            }
        }
        std::string tgtAbs;
        if (!DirAbsPath(tgtSpace, targetDir, tgtAbs))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }

        if (type == "file")
        {
            // 同名冲突 + 复制 + 落库整体持锁(与上传同款:并发同名复制由锁串行化,
            // 检查即最终裁决,后到者 409 且不触碰先到者的物理文件)
            std::string srcAbs = EntryAbsPath(space, dirId, name);
            std::string dstAbs = tgtAbs + "\\" + name;
            {
                std::lock_guard<std::mutex> lk(m_db.Mutex());
                {
                    Stmt st(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
                    if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                    BindInt(st.p, 1, (int64_t)tgtSpace);
                    BindInt(st.p, 2, (int64_t)targetDir);
                    BindText(st.p, 3, name);
                    if (sqlite3_step(st.p) == SQLITE_ROW)
                    {
                        ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                        return;
                    }
                    Stmt st2(m_db, "SELECT 1 FROM dirs WHERE space=? AND parent_id=? AND name=?");
                    if (!st2.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                    BindInt(st2.p, 1, (int64_t)tgtSpace);
                    BindInt(st2.p, 2, (int64_t)targetDir);
                    BindText(st2.p, 3, name);
                    if (sqlite3_step(st2.p) == SQLITE_ROW)
                    {
                        ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                        return;
                    }
                }
                if (!CopyFileW(ZmString::UTF8_To_Unicode(srcAbs).c_str(),
                               ZmString::UTF8_To_Unicode(dstAbs).c_str(), FALSE))
                {
                    ZmReqLoopRest::ResponseError(loop, 500, "复制失败");
                    return;
                }
                Stmt st(m_db,
                    "INSERT INTO files(space,dir_id,name,size,mtime,uploader_id,upload_time) "
                    "VALUES(?,?,?,?,?,?,?)");
                if (!st.p)
                {
                    DeleteFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
                BindInt(st.p, 1, (int64_t)tgtSpace);
                BindInt(st.p, 2, (int64_t)targetDir);
                BindText(st.p, 3, name);
                WIN32_FILE_ATTRIBUTE_DATA fad;
                int64_t fsz = 0, fmt = (int64_t)time(nullptr);
                if (GetFileAttributesExW(ZmString::UTF8_To_Unicode(dstAbs).c_str(),
                        GetFileExInfoStandard, &fad))
                {
                    ULARGE_INTEGER s;
                    s.HighPart = fad.nFileSizeHigh;
                    s.LowPart = fad.nFileSizeLow;
                    fsz = (int64_t)s.QuadPart;
                    ULARGE_INTEGER m;
                    m.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                    m.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                    fmt = (int64_t)(m.QuadPart / 10000000ULL - 11644473600ULL);   // FILETIME → unix
                }
                BindInt(st.p, 4, fsz);
                BindInt(st.p, 5, fmt);
                BindInt(st.p, 6, (int64_t)opUid);
                BindInt(st.p, 7, (int64_t)time(nullptr));
                if (sqlite3_step(st.p) != SQLITE_DONE)
                {
                    DeleteFileW(ZmString::UTF8_To_Unicode(dstAbs).c_str());
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
            }
            Log(opUid, opAccount, "copy", "file", id,
                "{\"to_dir\":" + std::to_string(targetDir) + "}");
        }
        else
        {
            // 目录深拷贝:DFS 复制物理目录 + 建 dirs/files 行
            // 栈元素:源物理路径 + 目标物理路径 + 源 dir id + 目标 dir id
            struct DirPair { std::string src; std::string dst; uint64_t srcId; uint64_t dstId; };
            std::vector<DirPair> stack;
            std::string srcRoot = EntryAbsPath(space, dirId, name);
            std::string dstRoot = tgtAbs + "\\" + name;
            bool failed = false;

            // 根目录:同名冲突 + 建目录 + dirs 行整体持锁 —— 并发同名目录复制由锁串行化,
            // 输者在此 409 退出,不进入 DFS 也不触发回滚,杜绝"输者回滚删除赢者已复制内容"
            {
                std::lock_guard<std::mutex> lk(m_db.Mutex());
                {
                    Stmt st(m_db, "SELECT 1 FROM dirs WHERE space=? AND parent_id=? AND name=?");
                    if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                    BindInt(st.p, 1, (int64_t)tgtSpace);
                    BindInt(st.p, 2, (int64_t)targetDir);
                    BindText(st.p, 3, name);
                    if (sqlite3_step(st.p) == SQLITE_ROW)
                    {
                        ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                        return;
                    }
                    Stmt st2(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
                    if (!st2.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                    BindInt(st2.p, 1, (int64_t)tgtSpace);
                    BindInt(st2.p, 2, (int64_t)targetDir);
                    BindText(st2.p, 3, name);
                    if (sqlite3_step(st2.p) == SQLITE_ROW)
                    {
                        ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
                        return;
                    }
                }
                if (!CreateDirectoryW(ZmString::UTF8_To_Unicode(dstRoot).c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS)
                {
                    failed = true;
                }
                else
                {
                    Stmt st(m_db,
                        "INSERT INTO dirs(space,parent_id,name,create_by,create_time) VALUES(?,?,?,?,?)");
                    if (!st.p)
                        failed = true;
                    else
                    {
                        BindInt(st.p, 1, (int64_t)tgtSpace);
                        BindInt(st.p, 2, (int64_t)targetDir);
                        BindText(st.p, 3, name);
                        BindInt(st.p, 4, (int64_t)opUid);
                        BindInt(st.p, 5, (int64_t)time(nullptr));
                        if (sqlite3_step(st.p) != SQLITE_DONE)
                            failed = true;
                        else
                            stack.push_back({srcRoot, dstRoot, id,
                                             (uint64_t)m_db.LastInsertRowId()});
                    }
                }
            }

            while (!stack.empty() && !failed)
            {
                DirPair p = stack.back();
                stack.pop_back();

                std::wstring pattern = ZmString::UTF8_To_Unicode(p.src) + L"\\*";
                WIN32_FIND_DATAW fd;
                HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
                if (hFind == INVALID_HANDLE_VALUE)
                    continue;
                do
                {
                    std::string nm = ZmString::Unicode_To_UTF8(fd.cFileName);
                    if (nm == "." || nm == ".." || nm[0] == '.')
                        continue;
                    std::string childSrc = p.src + "\\" + nm;
                    std::string childDst = p.dst + "\\" + nm;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        if (!CreateDirectoryW(ZmString::UTF8_To_Unicode(childDst).c_str(), nullptr) &&
                            GetLastError() != ERROR_ALREADY_EXISTS)
                        {
                            failed = true;
                            break;
                        }
                        uint64_t newDirId = 0;
                        {
                            std::lock_guard<std::mutex> lk(m_db.Mutex());
                            Stmt st(m_db,
                                "INSERT INTO dirs(space,parent_id,name,create_by,create_time) "
                                "VALUES(?,?,?,?,?)");
                            if (!st.p) { failed = true; break; }
                            BindInt(st.p, 1, (int64_t)tgtSpace);
                            BindInt(st.p, 2, (int64_t)p.dstId);
                            BindText(st.p, 3, nm);
                            BindInt(st.p, 4, (int64_t)opUid);
                            BindInt(st.p, 5, (int64_t)time(nullptr));
                            if (sqlite3_step(st.p) != SQLITE_DONE) { failed = true; break; }
                            newDirId = (uint64_t)m_db.LastInsertRowId();
                        }
                        // 源子目录 id(按名查源空间)
                        uint64_t childSrcId = 0;
                        {
                            std::lock_guard<std::mutex> lk(m_db.Mutex());
                            Stmt st(m_db,
                                "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
                            if (st.p)
                            {
                                BindInt(st.p, 1, (int64_t)space);
                                BindInt(st.p, 2, (int64_t)p.srcId);
                                BindText(st.p, 3, nm);
                                if (sqlite3_step(st.p) == SQLITE_ROW)
                                    childSrcId = (uint64_t)sqlite3_column_int64(st.p, 0);
                            }
                        }
                        stack.push_back({childSrc, childDst, childSrcId, newDirId});
                    }
                    else
                    {
                        if (!CopyFileW(ZmString::UTF8_To_Unicode(childSrc).c_str(),
                                       ZmString::UTF8_To_Unicode(childDst).c_str(), FALSE))
                        {
                            failed = true;
                            break;
                        }
                        int64_t sz = 0, mt = (int64_t)time(nullptr);
                        WIN32_FILE_ATTRIBUTE_DATA fad;
                        if (GetFileAttributesExW(ZmString::UTF8_To_Unicode(childDst).c_str(),
                                                 GetFileExInfoStandard, &fad))
                        {
                            ULARGE_INTEGER s;
                            s.HighPart = fad.nFileSizeHigh;
                            s.LowPart = fad.nFileSizeLow;
                            sz = (int64_t)s.QuadPart;
                            ULARGE_INTEGER m;
                            m.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                            m.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                            mt = (int64_t)(m.QuadPart / 10000000ULL - 11644473600ULL);
                        }
                        std::lock_guard<std::mutex> lk(m_db.Mutex());
                        Stmt st(m_db,
                            "INSERT INTO files(space,dir_id,name,size,mtime,uploader_id,upload_time) "
                            "VALUES(?,?,?,?,?,?,?)");
                        if (!st.p) { failed = true; break; }
                        BindInt(st.p, 1, (int64_t)tgtSpace);
                        BindInt(st.p, 2, (int64_t)p.dstId);
                        BindText(st.p, 3, nm);
                        BindInt(st.p, 4, sz);
                        BindInt(st.p, 5, mt);
                        BindInt(st.p, 6, (int64_t)opUid);
                        BindInt(st.p, 7, (int64_t)time(nullptr));
                        if (sqlite3_step(st.p) != SQLITE_DONE) { failed = true; break; }
                    }
                } while (FindNextFileW(hFind, &fd) && !failed);
                FindClose(hFind);
            }

            if (failed)
            {
                // 回滚:物理删目标根 + DB 删目标子树行
                DeleteDirRecursive(ZmString::UTF8_To_Unicode(dstRoot).c_str());
                std::vector<uint64_t> dirtyDirs;
                {
                    std::lock_guard<std::mutex> lk(m_db.Mutex());
                    Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
                    if (st.p)
                    {
                        BindInt(st.p, 1, (int64_t)tgtSpace);
                        BindInt(st.p, 2, (int64_t)targetDir);
                        BindText(st.p, 3, name);
                        if (sqlite3_step(st.p) == SQLITE_ROW)
                            dirtyDirs.push_back((uint64_t)sqlite3_column_int64(st.p, 0));
                    }
                }
                for (size_t i = 0; i < dirtyDirs.size(); ++i)
                {
                    std::vector<uint64_t> subs;
                    CollectSubtreeDirs(tgtSpace, dirtyDirs[i], subs);
                    for (uint64_t d : subs)
                        dirtyDirs.push_back(d);
                }
                {
                    std::lock_guard<std::mutex> lk(m_db.Mutex());
                    // 事务:回滚清理原子化(清理失败仅记日志,物理目录已删,尽力而为)
                    std::string ph;
                    for (size_t i = 0; i < dirtyDirs.size(); ++i)
                        ph += (i ? "," : "") + std::to_string(dirtyDirs[i]);
                    if (!dirtyDirs.empty())
                    {
                        bool ok = m_db.Begin();
                        if (ok)
                            ok = m_db.Exec(("DELETE FROM files WHERE dir_id IN (" + ph + ")").c_str());
                        if (ok)
                            ok = m_db.Exec(("DELETE FROM dirs WHERE id IN (" + ph + ")").c_str());
                        if (!ok || !m_db.Commit())
                        {
                            m_db.Rollback();
                            DEFAULT_LOG_ERROR("[FileHub] 复制回滚清理 DB 失败");
                        }
                    }
                }
                ZmReqLoopRest::ResponseError(loop, 500, "复制失败");
                return;
            }
            Log(opUid, opAccount, "copy", "dir", id,
                "{\"to_dir\":" + std::to_string(targetDir) + "}");
        }
    }
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void FileHubModule::HandleDelete(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                 const std::string& opRole, const std::string& opAccount)
{
    std::vector<std::pair<std::string, uint64_t>> ids;
    if (!ParseIdList(body, ids))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    for (auto& it : ids)
    {
        const std::string& type = it.first;
        uint64_t id = it.second;
        int space = -1;
        uint64_t dirId = 0, uploaderId = 0;
        std::string name;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            if (type == "file")
            {
                Stmt st(m_db, "SELECT space,dir_id,name,uploader_id FROM files WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
            else
            {
                Stmt st(m_db, "SELECT space,parent_id,name,create_by FROM dirs WHERE id=?");
                if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
                BindInt(st.p, 1, (int64_t)id);
                if (sqlite3_step(st.p) != SQLITE_ROW)
                {
                    ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                    return;
                }
                space = (int)sqlite3_column_int64(st.p, 0);
                dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                name = n ? n : "";
                uploaderId = (uint64_t)sqlite3_column_int64(st.p, 3);
            }
        }

        std::string permErr;
    if (!CanModify(opRole, opUid, space, uploaderId, permErr))
        {
            ZmReqLoopRest::ResponseError(loop, 403, permErr);
            return;
        }

        if (type == "file")
        {
            std::string absPath = EntryAbsPath(space, dirId, name);
            if (!DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str()))
            {
                // 物理文件已不存在(被手动删/漂移):视为已删,DB 行照删(幂等)
                if (GetLastError() != ERROR_FILE_NOT_FOUND)
                {
                    ZmReqLoopRest::ResponseError(loop, 500, "删除失败");
                    return;
                }
            }
            {
                std::lock_guard<std::mutex> lk(m_db.Mutex());
                Stmt st(m_db, "DELETE FROM files WHERE id=?");
                if (!st.p)
                {
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
                BindInt(st.p, 1, (int64_t)id);
                sqlite3_step(st.p);
            }
            RemoveSharesOf("file", id);
            Log(opUid, opAccount, "delete", "file", id, "{\"name\":\"" + name + "\"}");
        }
        else
        {
            // 整棵子树:先收集 id(文件系统删除前)
            std::vector<uint64_t> dirs, fileIds;
            CollectSubtreeDirs(space, id, dirs);
            CollectSubtreeFiles(space, id, fileIds);
            std::string absPath = EntryAbsPath(space, dirId, name);
            if (!DeleteDirRecursive(ZmString::UTF8_To_Unicode(absPath).c_str()))
            {
                // 物理目录已不存在(被手动删/漂移):视为已删,子树行照删(幂等)
                DWORD derr = GetLastError();
                if (derr != ERROR_FILE_NOT_FOUND && derr != ERROR_PATH_NOT_FOUND)
                {
                    ZmReqLoopRest::ResponseError(loop, 500, "删除失败");
                    return;
                }
            }
            {
                std::lock_guard<std::mutex> lk(m_db.Mutex());
                // 事务:两表删除原子化,崩溃不留 files.dir_id 指向已删目录的孤儿行
                std::string placeholders;
                for (size_t i = 0; i < dirs.size(); ++i)
                    placeholders += (i ? "," : "") + std::to_string(dirs[i]);
                bool ok = m_db.Begin();
                if (ok && !dirs.empty())
                    ok = m_db.Exec(("DELETE FROM files WHERE dir_id IN (" + placeholders + ")").c_str());
                if (ok)
                    ok = m_db.Exec(("DELETE FROM dirs WHERE id IN (" + placeholders + ")").c_str());
                if (!ok || !m_db.Commit())
                {
                    m_db.Rollback();
                    ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                    return;
                }
            }
            // 级联清理分享:子树全部 dirs + files 的 shares 行(dirs 含顶层自身)
            for (uint64_t d : dirs)
                RemoveSharesOf("dir", d);
            for (uint64_t fid : fileIds)
                RemoveSharesOf("file", fid);
            Log(opUid, opAccount, "delete", "dir", id, "{\"name\":\"" + name + "\"}");
        }
    }
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

// ============================================================================
// 上传 / 下载
// ============================================================================

int FileHubModule::ReceiveFileStream(ZmHttpdTask* task, const std::string& physicalPath,
                                     uint64_t& outSize)
{
    outSize = 0;
    std::wstring wPath = ZmString::UTF8_To_Unicode(physicalPath);
    HANDLE hFile = CreateFileW(wPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;

    struct evbuffer* in = task->GetInputBuffer();
    std::vector<unsigned char> buf(64 * 1024);   // 堆分配:ZmReqLoop 线程栈受限,防栈溢出
    uint64_t total = 0;
    DWORD written = 0;
    while (in && evbuffer_get_length(in) > 0)
    {
        size_t avail = evbuffer_get_length(in);
        size_t chunk = avail > buf.size() ? buf.size() : avail;
        evbuffer_copyout(in, buf.data(), chunk);
        if (!WriteFile(hFile, buf.data(), (DWORD)chunk, &written, nullptr) || written != chunk)
        {
            CloseHandle(hFile);
            DeleteFileW(wPath.c_str());
            return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR;
        }
        evbuffer_drain(in, chunk);
        total += chunk;
    }
    CloseHandle(hFile);
    outSize = total;
    return 0;   // 成功
}

void FileHubModule::HandleUpload(ZmReqLoop* loop, ZmHttpdTask* task, int space, uint64_t opUid,
                                 const std::string& opAccount)
{
    // 上传为长任务(body 已完整到达,写盘+校验可能超过默认 5s deadline;
    // 不取消则超时收尾关闭连接 → 浏览器 XHR onerror「网络连接失败」)
    loop->CancelDeadline();

    // 可选:传输任务 task_id(前端 task_create 预建行后携带;缺失时保持旧行为不记任务)
    const char* tid = task->GetQueryValue("task_id", "");
    std::string taskId = (tid && ValidTaskId(tid)) ? tid : "";
    // 任务行收尾(幂等;终态由 UpdateTransferTask 内 NOT IN 守卫防覆盖)
    auto markFail = [&](const char* msg) {
        if (!taskId.empty())
            UpdateTransferTask(taskId, opUid, kTaskStatusFailed, msg, -1);
    };
    auto markDone = [&](int64_t size) {
        if (!taskId.empty())
            UpdateTransferTask(taskId, opUid, kTaskStatusDone, "", size);
    };

    uint64_t dirId = (uint64_t)strtoull(task->GetQueryValue("dir_id", "0"), nullptr, 10);
    const char* name = task->GetQueryValue("name", "");
    if (!IsValidName(name ? name : ""))
    {
        markFail("文件名不合法");
        ZmReqLoopRest::ResponseError(loop, 400, "文件名不合法");
        return;
    }
    if (!DirExists(space, dirId))
    {
        markFail("目录不存在");
        ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
        return;
    }
    // 行兜底:task_create 未先行(旧版前端/直传)时按 uploading 建行
    if (!taskId.empty())
        CreateTransferTask(taskId, opUid, "upload", kTaskStatusUploading, name, 0);
    // 同名冲突 + 写盘 + 落库整体持锁:并发同名上传由该锁串行化,冲突检查即最终裁决,
    // 后到者 409 且不触碰先到者的物理文件。原分锁实现存在窗口:两请求都通过检查 → 互覆写
    // 同一路径 → INSERT 撞 UNIQUE 回滚误删对方文件,造成数据丢失。
    // ★ 任务行更新(markFail/markDone)只能在锁外调用:UpdateTransferTask 内部加
    //    m_db.Mutex(),锁内调用 = 非递归锁嵌套加锁,MSVC 抛异常(实测响应变空)。
    std::string absPath = EntryAbsPath(space, dirId, name);   // 路径解析自持锁,须在整体锁外
    uint64_t size = 0;
    uint64_t newId = 0;
    int failCode = 0;
    std::string failMsg;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());

        // 同名冲突
        Stmt stChk(m_db, "SELECT 1 FROM files WHERE space=? AND dir_id=? AND name=?");
        if (!stChk.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(stChk.p, 1, (int64_t)space);
        BindInt(stChk.p, 2, (int64_t)dirId);
        BindText(stChk.p, 3, name);
        if (sqlite3_step(stChk.p) == SQLITE_ROW)
        {
            DEFAULT_LOG_INFO("[FileHub] upload 409: space={} dir_id={} name={}",
                             space, dirId, name ? name : "");
            failCode = 409;
            failMsg = "同名文件已存在";
        }
        else if (ReceiveFileStream(task, absPath, size) != 0)
        {
            DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
            failCode = 500;
            failMsg = "上传失败";
        }
        else
        {
            // X-File-Size 声明校验(有则比对,不符删半成品)
            const char* declared = task->GetRequestHeader("X-File-Size");
            if (declared && declared[0])
            {
                uint64_t expected = (uint64_t)strtoull(declared, nullptr, 10);
                if (expected != size)
                {
                    DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
                    failCode = 400;
                    failMsg = "文件大小不一致";
                }
            }
            // 允许 0 字节文件上传(空配置文件等合法场景;ReceiveFileStream 对空 body 创建空文件)

            if (failCode == 0)
            {
                // 落盘 stat
                WIN32_FILE_ATTRIBUTE_DATA fad;
                int64_t mtime = time(nullptr);
                if (GetFileAttributesExW(ZmString::UTF8_To_Unicode(absPath).c_str(),
                                         GetFileExInfoStandard, &fad))
                {
                    ULARGE_INTEGER mt;
                    mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                    mt.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                    mtime = (int64_t)(mt.QuadPart / 10000000ULL - 11644473600ULL);   // FILETIME → unix
                }

                Stmt st(m_db,
                    "INSERT INTO files(space,dir_id,name,size,mtime,uploader_id,upload_time) "
                    "VALUES(?,?,?,?,?,?,?)");
                if (!st.p)
                {
                    DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
                    failCode = 500;
                    failMsg = "服务器内部错误";
                }
                else
                {
                    BindInt(st.p, 1, (int64_t)space);
                    BindInt(st.p, 2, (int64_t)dirId);
                    BindText(st.p, 3, name);
                    BindInt(st.p, 4, (int64_t)size);
                    BindInt(st.p, 5, mtime);
                    BindInt(st.p, 6, (int64_t)opUid);
                    BindInt(st.p, 7, (int64_t)time(nullptr));
                    if (sqlite3_step(st.p) != SQLITE_DONE)
                    {
                        DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
                        failCode = 500;
                        failMsg = "服务器内部错误";
                    }
                    else
                    {
                        newId = (uint64_t)m_db.LastInsertRowId();
                    }
                }
            }
        }
    }

    if (failCode != 0)
    {
        markFail(failMsg.c_str());   // 锁外:UpdateTransferTask 自加锁,不嵌套
        ZmReqLoopRest::ResponseError(loop, failCode, failMsg);
        return;
    }
    markDone((int64_t)size);
    Log(opUid, opAccount, "upload", "file", newId,
        "{\"name\":\"" + std::string(name) + "\",\"size\":" + std::to_string(size) + "}");
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"file", {{"id", (int64_t)newId}, {"name", name},
                             {"size", (int64_t)size}}}}}});
}

// ============================================================================
// 通用单文件下载(带 Range 断点续传)
// ============================================================================

namespace
{
std::string ExtractFilename(const std::string& uri)
{
    std::string path = uri;
    size_t qpos = path.find('?');
    if (qpos != std::string::npos) path = path.substr(0, qpos);
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(slash + 1);
    return path;
}
} // namespace

// ============================================================================
// Range 解析(单段 bytes=;严格 strtoll 校验)
// 返回:1 = 合法单段(start/end 输出);0 = 按全文件处理(未知前缀/多段/无 Range);
//       -1 = 非法(→ 416)。与旧实现语义对齐:无法识别的 Range 头回全文件,bytes= 非法回 416
// ============================================================================

static int ParseFileRange(const char* rangeHeader, int64_t fileSize,
                          int64_t& start, int64_t& end)
{
    if (!rangeHeader || fileSize <= 0)
        return 0;   // 无/空文件不解析 Range,按全文件处理
    if (strlen(rangeHeader) < 7 || _strnicmp(rangeHeader, "bytes=", 6) != 0)
        return 0;   // 非 bytes 单位:按全文件(兼容旧实现)
    std::string rangeVal(rangeHeader + 6);
    if (rangeVal.find(',') != std::string::npos)
        return 0;   // 多段 range:回全文件(兼容旧实现)

    size_t dashPos = rangeVal.find('-');
    if (dashPos == std::string::npos)
        return -1;

    // stoll 无防护(非法/溢出输入抛异常穿透请求线程):strtoll + 严格校验
    auto parseNum = [](const std::string& s, int64_t* out) -> bool {
        if (s.empty())
            return false;
        const char* p = s.c_str();
        char* end = nullptr;
        errno = 0;
        long long v = strtoll(p, &end, 10);
        if (errno != 0 || !end || *end != '\0')
            return false;
        *out = (int64_t)v;
        return true;
    };

    std::string startStr = rangeVal.substr(0, dashPos);
    std::string endStr = rangeVal.substr(dashPos + 1);
    if (startStr.empty() && !endStr.empty()) {
        // 后缀范围:bytes=-N(最后 N 字节)
        int64_t suffixLen = 0;
        if (!parseNum(endStr, &suffixLen))
            return -1;
        start = (suffixLen >= fileSize) ? 0 : fileSize - suffixLen;
        end = fileSize - 1;
    } else if (!startStr.empty() && endStr.empty()) {
        if (!parseNum(startStr, &start))
            return -1;
        end = fileSize - 1;
    } else {
        if (!parseNum(startStr, &start) || !parseNum(endStr, &end))
            return -1;
    }

    if (start < 0 || end >= fileSize || start > end)
        return -1;
    return 1;
}

// ============================================================================
// 通用文件下载:分块流式(混合策略 ≥2GB 回退路径,§3.2)
// 背景:vendored libevent SSL bev 发送 ≥2GB body 实测必败;<2GB 走分段零拷贝
// (SendFileHybrid),本路径仅接手 ≥2GB 文件;水位节流(PendingReplyBytes)
// 防慢客户端用户态内存随下载时长线性涨。
// ============================================================================

int FileHubModule::SendFileStream(ZmReqLoop* loop, ZmHttpdTask* task,
                                  const std::string& physicalPath, const char* rangeHeader)
{
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;
    int64_t fileSize = _filelengthi64(fd);
    if (fileSize < 0) { _close(fd); return ZM_HTTP_STATUS_CODE_NOT_FOUND; }

    // Range 解析:合法单段 → 206;未知前缀/多段 → 全文件 200;bytes= 非法 → 416
    int64_t start = 0, end = fileSize - 1;
    bool isRange = false;
    if (rangeHeader && rangeHeader[0])
    {
        int pr = ParseFileRange(rangeHeader, fileSize, start, end);
        if (pr < 0)
        {
            _close(fd);
            ZmReqLoopRest::ResponseError(loop, ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE, "Range Not Satisfiable");
            return ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE;
        }
        isRange = (pr == 1);
    }
    int64_t chunkTotal = end - start + 1;

    std::string mime = ZmHttpUtil::GetMimeType(physicalPath);
    std::string disp = "attachment; filename=\"" + ExtractFilename(physicalPath) + "\"";
    std::string clStr = std::to_string(chunkTotal);

    // 显式 Content-Length → evhttp 固定长度传输(非 chunked),浏览器/IDM 校验与进度正常;
    // 206 另带 Content-Range。Accept-Ranges 恒带,便于续传探测。
    if (isRange)
    {
        std::string crStr = "bytes " + std::to_string(start) + "-" + std::to_string(end) +
                            "/" + std::to_string(fileSize);
        ZmReqLoopRest::ResponseStreamStart(loop, ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT, {
            {"Content-Type", mime.c_str()},
            {"Content-Disposition", disp.c_str()},
            {"Accept-Ranges", "bytes"},
            {"Content-Length", clStr.c_str()},
            {"Content-Range", crStr.c_str()},
        });
    }
    else
    {
        ZmReqLoopRest::ResponseStreamStart(loop, ZM_HTTP_STATUS_CODE_OK, {
            {"Content-Type", mime.c_str()},
            {"Content-Disposition", disp.c_str()},
            {"Accept-Ranges", "bytes"},
            {"Content-Length", clStr.c_str()},
        });
    }

    // 分块流式发送:1MB 块读-发;连接关闭即中止;水位节流防慢客户端内存线性涨;
    // 结束无条件 EndStreamReply
    loop->PostToLoop([task, fd, start, chunkTotal, isRange](ZmReqLoop* l) {
        if (l->IsClosing())
        {
            _close(fd);
            return;   // 头也可能未发出(关闭窗口),后续收尾由默认路径处理
        }
        std::vector<unsigned char> buf(kFileStreamChunkSize);
        _lseeki64(fd, start, SEEK_SET);
        int64_t remaining = chunkTotal;
        bool stalledOut = false;
        int64_t stallMs = 0;
        while (remaining > 0 && !task->IsConnClosed())
        {
            // ★ 水位节流(§3.2):排队未发字节超阈值 → 暂停读盘等排水(evbuffer 锁
            //   保护跨线程查询);持续堵塞超阈值视为对端停滞,放弃本次响应(Range
            //   客户端可断点续传重试)。快客户端 pending≈单块,不触发。
            while (!task->IsConnClosed() &&
                   task->PendingReplyBytes() > kSendWatermarkBytes)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                stallMs += 10;
                if (stallMs >= kSendStallAbortMs) { stalledOut = true; break; }
            }
            if (stalledOut || task->IsConnClosed())
                break;
            stallMs = 0;
            size_t toRead = remaining > (int64_t)buf.size() ? buf.size() : (size_t)remaining;
            int n = _read(fd, buf.data(), (unsigned int)toRead);
            if (n <= 0)
                break;
            task->SendReplyChunk(buf.data(), (size_t)n);
            remaining -= n;
        }
        _close(fd);
        if (stalledOut)
            DEFAULT_LOG_WARN("[FileHub] 下载对端停滞超时,放弃本次响应(客户端可 Range 续传)");
        // ★ 收尾遵循公共库流式约定(参照音频模块):先取回复门 → EndStreamReply 驱动
        //   doer 回收 → 投 DONE 回池。缺任一步:成功路径 ZmReqLoop(独立线程)永不回池,
        //   池耗尽后请求排队超时;close 竞态下 ProcessClose 会对已回收 doer TriggerReply
        l->TryReply();
        task->EndStreamReply();
        l->PostToLoop(ZmReqLoop::REQ_LOOP_SIG_DONE, task);
    });
    return isRange ? ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT : ZM_HTTP_STATUS_CODE_OK;
}

int FileHubModule::SendFileHybrid(ZmReqLoop* loop, ZmHttpdTask* task,
                                  const std::string& physicalPath,
                                  const std::string& dispName, const char* rangeHeader)
{
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;
    int64_t fileSize = _filelengthi64(fd);
    if (fileSize < 0) { _close(fd); return ZM_HTTP_STATUS_CODE_NOT_FOUND; }

    // Range 解析:合法单段 → 206;未知前缀/多段 → 全文件 200;bytes= 非法 → 416
    int64_t start = 0, end = fileSize - 1;
    bool isRange = false;
    if (rangeHeader && rangeHeader[0])
    {
        int pr = ParseFileRange(rangeHeader, fileSize, start, end);
        if (pr < 0)
        {
            _close(fd);
            ZmReqLoopRest::ResponseError(loop, ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE, "Range Not Satisfiable");
            return ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE;
        }
        isRange = (pr == 1);
    }
    int64_t chunkTotal = end - start + 1;

    // ── <2GB:分段零拷贝(SetReplyFile 内部按 2GB 分段文件映射;Range 206 续传)──
    //   Content-Length 由 evhttp 按回复体长度自动补,206 时恰为分片长度
    if (fileSize < kZeroCopyMaxFileBytes)
    {
        std::string mime = ZmHttpUtil::GetMimeType(physicalPath);
        task->PutReplyHeader("Content-Type", mime.c_str());
        task->PutReplyHeader("Accept-Ranges", "bytes");
        if (isRange)
        {
            task->PutReplyHeader("Content-Range",
                ("bytes " + std::to_string(start) + "-" + std::to_string(end) +
                 "/" + std::to_string(fileSize)).c_str());
        }
        loop->CancelDeadline();
        ZmReqLoopRest::ResponseFile(loop, fd, start, chunkTotal,
                                    ExtractFilename(dispName).c_str(), 0,
                                    isRange ? ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT
                                            : ZM_HTTP_STATUS_CODE_OK);
        return isRange ? ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT : ZM_HTTP_STATUS_CODE_OK;
    }

    // ── ≥2GB:分块流式回退(SSL bev ≥2GB 实测限制)+ 水位节流(见 SendFileStream)──
    //   预置本次下载名(下载名与产物名解耦):SendFileStream 会再按物理文件名补一条,
    //   同名头重复时浏览器/IDM 取首条,故以先写入的 dispName 为准
    _close(fd);   // SendFileStream 自行重开(Range/头/收尾全托管)
    std::string disp = "attachment; filename=\"" + ExtractFilename(dispName) + "\"";
    task->PutReplyHeader("Content-Disposition", disp.c_str());
    return SendFileStream(loop, task, physicalPath, rangeHeader);
}

void FileHubModule::HandleDownload(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid,
                                   const std::string& opAccount)
{
    // ★ 根治:手动 TriggerReply 不占 TryReply 门——不取消 deadline 的话,5s 后
    //   超时收尾(onTimeout)会再触发一次 REPLY,同一请求产生双信号,doer 回收后
    //   残留信号访问已释放 m_request(实测崩溃)。下载响应同步发送,无需 deadline。
    loop->CancelDeadline();

    uint64_t fileId = (uint64_t)strtoull(task->GetQueryValue("file_id", "0"), nullptr, 10);
    if (fileId == 0)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    // 可选:传输任务 task_id(前端直链下载携带;缺失时保持旧行为不记任务)
    const char* tid = task->GetQueryValue("task_id", "");
    std::string taskId = (tid && ValidTaskId(tid)) ? tid : "";
    int space = -1;
    uint64_t dirId = 0;
    std::string name;
    int64_t fileSize = 0;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT space,dir_id,name,size FROM files WHERE id=?");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)fileId);
        if (sqlite3_step(st.p) != SQLITE_ROW)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
            return;
        }
        space = (int)sqlite3_column_int64(st.p, 0);
        dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
        name = n ? n : "";
        fileSize = sqlite3_column_int64(st.p, 3);
    }
    // 权限:公共空间任何登录用户;个人空间仅本人
    if (space != 0 && opUid != (uint64_t)space)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可下载");
        return;
    }
    // 建行(triggered;幂等,前端重试携带新 task_id 各记一条)
    if (!taskId.empty())
        CreateTransferTask(taskId, opUid, "file", kTaskStatusTriggered, name, fileSize);

    std::string absPath = EntryAbsPath(space, dirId, name);
    // 混合发送(§3.2):<2GB 分段零拷贝 / ≥2GB 分块回退;两路径均支持 Range 续传
    int rc = SendFileHybrid(loop, task, absPath, name, task->GetRequestHeader("Range"));
    if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
    {
        if (!taskId.empty())
            UpdateTransferTask(taskId, opUid, kTaskStatusFailed, "文件不存在", -1);
        ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
        return;
    }
    if (rc == ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE)
    {
        if (!taskId.empty())
            UpdateTransferTask(taskId, opUid, kTaskStatusFailed, "Range 请求不合法", -1);
        return;   // 416 已回复
    }
    // 流式响应已接管回复(头/分块/收尾在 SendFileStream 内完成),不再 TriggerReply
    if (!taskId.empty())
        UpdateTransferTask(taskId, opUid, kTaskStatusDone, "", -1);
    Log(opUid, opAccount, "download", "file", fileId, "{\"name\":\"" + name + "\"}");
}

// ============================================================================
// 打包中心(后台预打包:下载打包/分享打包统一,内容指纹复用)
// ============================================================================

static constexpr const char* kPackStatusPacking = "packing";
static constexpr const char* kPackStatusDone    = "done";
static constexpr const char* kPackStatusFailed  = "failed";

static void HexEncode(const unsigned char* d, size_t n, std::string& out)
{
    static const char kHex[] = "0123456789abcdef";
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        out[i * 2]     = kHex[d[i] >> 4];
        out[i * 2 + 1] = kHex[d[i] & 0xF];
    }
}

std::string FileHubModule::HashHex(const std::string& in)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr);
    std::string out;
    HexEncode(md, len, out);
    return out;
}

/** 流式 SHA-256 文件内容哈希(64KB 块;取消/读失败返回 false) */
static bool HashFileStream(const std::string& absPath, std::string& hexOut,
                           const std::function<bool()>& cancelled)
{
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(absPath).c_str(),
                  _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { _close(fd); return false; }
    bool ok = (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1);
    std::vector<unsigned char> buf(64 * 1024);
    while (ok)
    {
        if (cancelled && !cancelled()) { ok = false; break; }
        int n = (int)_read(fd, buf.data(), (unsigned int)buf.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (EVP_DigestUpdate(ctx, buf.data(), (size_t)n) != 1) { ok = false; break; }
    }
    if (ok)
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        ok = (EVP_DigestFinal_ex(ctx, md, &len) == 1);
        if (ok) HexEncode(md, len, hexOut);
    }
    EVP_MD_CTX_free(ctx);
    _close(fd);
    return ok;
}

std::string FileHubModule::PackTempDir() const
{
    return m_hubRoot + "\\pack_temp";
}

std::string FileHubModule::PackTmpPath(const std::string& taskId) const
{
    return PackTempDir() + "\\.tmp\\" + taskId + ".zip";
}

std::string FileHubModule::PackPathFromFp(int ownerUid, const std::string& fp32) const
{
    return PackTempDir() + "\\" + std::to_string((long long)ownerUid) + "\\" + fp32 + ".zip";
}

std::string FileHubModule::EntryDiskPath(int space, const PackEntry& e, std::string& absPath)
{
    if (e.type == "dir")
    {
        return DirAbsPath(space, e.id, absPath) ? "" : "目录不存在";
    }
    uint64_t dirId = 0;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT dir_id,name FROM files WHERE id=? AND space=?");
        if (!st.p)
            return "数据库查询失败";
        BindInt(st.p, 1, (int64_t)e.id);
        BindInt(st.p, 2, (int64_t)space);
        if (sqlite3_step(st.p) != SQLITE_ROW)
            return "文件不存在";
        dirId = (uint64_t)sqlite3_column_int64(st.p, 0);
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        name = n ? n : "";
    }
    // ★ 锁外组装路径:EntryAbsPath→DirAbsPath 内部会再取 m_db.Mutex(),
    //   持锁调用 => 同线程嵌套锁(MSVC EDEADLK "resource deadlock would occur")
    absPath = EntryAbsPath(space, dirId, name);
    return "";
}

bool FileHubModule::ExpandPackItems(int space, const std::vector<std::pair<std::string, uint64_t>>& ids,
                                    std::vector<PackEntry>& out, const std::function<bool()>& cancelled)
{
    // 顶层显式文件先登记(文件优先去重规则:目录展开遇同 id 跳过)
    std::set<uint64_t> topFiles;
    for (const auto& it : ids)
        if (it.first == "file")
            topFiles.insert(it.second);

    std::function<bool(const std::string&, uint64_t, const std::string&)> recurseDir =
        [&](const std::string& dirName, uint64_t dirId, const std::string& prefix) -> bool {
        if (cancelled && !cancelled())
            return false;
        std::string rel = prefix.empty() ? dirName : prefix + "/" + dirName;

        std::vector<std::tuple<std::string, uint64_t, int64_t>> childFiles;   // (name,id,size)
        std::vector<std::pair<std::string, uint64_t>> childDirs;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt stD(m_db, "SELECT name FROM dirs WHERE id=? AND space=?");
            if (!stD.p)
                return false;
            BindInt(stD.p, 1, (int64_t)dirId);
            BindInt(stD.p, 2, (int64_t)space);
            if (sqlite3_step(stD.p) != SQLITE_ROW)
                return false;
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stD.p, 0));
            Stmt stF(m_db, "SELECT id,name,size FROM files WHERE space=? AND dir_id=? ORDER BY name");
            if (!stF.p)
                return false;
            BindInt(stF.p, 1, (int64_t)space);
            BindInt(stF.p, 2, (int64_t)dirId);
            while (sqlite3_step(stF.p) == SQLITE_ROW)
            {
                uint64_t fid = (uint64_t)sqlite3_column_int64(stF.p, 0);
                const char* fn = reinterpret_cast<const char*>(sqlite3_column_text(stF.p, 1));
                childFiles.emplace_back(fn ? fn : "", fid,
                                        (int64_t)sqlite3_column_int64(stF.p, 2));
            }
            Stmt stC(m_db, "SELECT id,name FROM dirs WHERE space=? AND parent_id=? ORDER BY name");
            if (!stC.p)
                return false;
            BindInt(stC.p, 1, (int64_t)space);
            BindInt(stC.p, 2, (int64_t)dirId);
            while (sqlite3_step(stC.p) == SQLITE_ROW)
            {
                uint64_t cid = (uint64_t)sqlite3_column_int64(stC.p, 0);
                const char* cn = reinterpret_cast<const char*>(sqlite3_column_text(stC.p, 1));
                childDirs.emplace_back(cn ? cn : "", cid);
            }
        }
        if (childDirs.empty() && childFiles.empty())
        {
            // 空目录 → zip 目录条目
            if (!rel.empty())
                out.push_back({"dir", dirId, rel + "/", 0, ""});
            return true;
        }
        for (const auto& [name, fid, fsize] : childFiles)
        {
            if (topFiles.count(fid))
                continue;   // 显式选中文件优先,目录展开跳过(防重复打包)
            out.push_back({"file", fid, rel + "/" + name, fsize, ""});
        }
        for (const auto& [name, cid] : childDirs)
            if (!recurseDir(name, cid, rel))
                return false;
        return true;
    };

    // 顶层按请求顺序:文件 → 直接条目;目录 → 递归展开(顶层名保留)
    for (const auto& it : ids)
    {
        if (m_gone.load())
            return false;
        if (cancelled && !cancelled())
            return false;
        std::string name;
        int64_t topSize = 0;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            const char* q = (it.first == "file")
                ? "SELECT name,size FROM files WHERE id=? AND space=?"
                : "SELECT name FROM dirs WHERE id=? AND space=?";
            Stmt st(m_db, q);
            if (!st.p)
                return false;
            BindInt(st.p, 1, (int64_t)it.second);
            BindInt(st.p, 2, (int64_t)space);
            if (sqlite3_step(st.p) != SQLITE_ROW)
                return false;
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
            name = n ? n : "";
            if (it.first == "file")
                topSize = sqlite3_column_int64(st.p, 1);
        }
        if (name.empty())
            return false;
        if (it.first == "file")
            out.push_back({"file", it.second, name, topSize, ""});
        else if (!recurseDir(name, it.second, ""))
            return false;
    }
    return true;
}

bool FileHubModule::ComputeFingerprint(int space, const std::vector<std::pair<std::string, uint64_t>>& ids,
                                       std::vector<PackEntry>& entries, std::string& outFp,
                                       std::string& outItemInfo, int64_t& outItemCount,
                                       const std::function<bool()>& cancelled)
{
    DEFAULT_LOG_INFO("[FileHub] CF 开始: space={}", space);
    if (!ExpandPackItems(space, ids, entries, cancelled))
        return false;
    DEFAULT_LOG_INFO("[FileHub] CF 展开完成: 条目={}", (int)entries.size());
    // 逐文件内容哈希(SHA-256)
    for (auto& e : entries)
    {
        if (cancelled && !cancelled())
            return false;
        if (e.type == "file")
        {
            std::string abs, h;
            std::string err = EntryDiskPath(space, e, abs);
            if (!err.empty())
                return false;
            if (!HashFileStream(abs, h, cancelled))
                return false;
            e.fileHash = h;
        }
    }
    DEFAULT_LOG_INFO("[FileHub] CF 哈希完成");
    // 指纹 = SHA256(按 relPath 排序的 "relPath\0size\0hash\0" 序列)
    std::vector<PackEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const PackEntry& a, const PackEntry& b) {
        return a.relPath < b.relPath;
    });
    std::string agg;
    for (auto& e : sorted)
    {
        agg += e.relPath;
        agg.push_back('\0');
        agg += std::to_string((long long)e.size);
        agg.push_back('\0');
        agg += (e.type == "file") ? e.fileHash : "";
        agg.push_back('\0');
    }
    outFp = HashHex(agg);
    // item_info JSON(清单:type/id/name(relPath)/size/hash;审计与复用校验数据源)
    outItemInfo = "{\"count\":" + std::to_string((long long)sorted.size()) + ",\"items\":[";
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        if (i)
            outItemInfo += ',';
        outItemInfo += "{\"type\":\"" + sorted[i].type + "\",\"id\":" +
                       std::to_string((long long)sorted[i].id) +
                       ",\"name\":\"" + JsonEscape(sorted[i].relPath) +
                       "\",\"size\":" + std::to_string((long long)sorted[i].size) +
                       ",\"hash\":\"" + sorted[i].fileHash + "\"}";
    }
    outItemInfo += "]}";
    outItemCount = (int64_t)sorted.size();
    return true;
}

bool FileHubModule::PackEntriesToFd(int fd, int space, std::vector<PackEntry>& entries,
                                    const std::function<bool()>& cancelled, uint64_t& outWritten,
                                    const std::function<void(uint64_t)>& onProgress)
{
    ZipFileWriter zip(fd);
    zip.SetCancelHook(cancelled);
    for (auto& e : entries)
    {
        if (cancelled && !cancelled())
            return false;
        if (e.type == "dir")
        {
            if (!zip.BeginEntry(e.relPath, true) || !zip.EndEntry())
                return false;
            if (onProgress)
                onProgress(zip.Written());
            continue;
        }
        std::string abs;
        std::string err = EntryDiskPath(space, e, abs);
        if (!err.empty())
            return false;
        int ffd = -1;
        if (_wsopen_s(&ffd, ZmString::UTF8_To_Unicode(abs).c_str(),
                      _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || ffd == -1)
            return false;
        if (!zip.BeginEntry(e.relPath, false))
        {
            _close(ffd);
            return false;
        }
        // 读-压同轮:内容哈希与 deflate 共用读流(打包期指纹;两次读取间变化以此为准)
        EVP_MD_CTX* hctx = EVP_MD_CTX_new();
        bool ok = (hctx && EVP_DigestInit_ex(hctx, EVP_sha256(), nullptr) == 1);
        std::vector<unsigned char> buf(64 * 1024);
        while (ok)
        {
            if (cancelled && !cancelled())
            {
                ok = false;
                break;
            }
            int n = (int)_read(ffd, buf.data(), (unsigned int)buf.size());
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            if (EVP_DigestUpdate(hctx, buf.data(), (size_t)n) != 1 ||
                !zip.Write(buf.data(), (size_t)n))
            {
                ok = false;
                break;
            }
        }
        if (ok)
        {
            unsigned char md[EVP_MAX_MD_SIZE];
            unsigned int len = 0;
            ok = (EVP_DigestFinal_ex(hctx, md, &len) == 1);
            if (ok)
            {
                std::string h;
                HexEncode(md, len, h);
                e.fileHash = h;   // 打包期指纹回填(§4.3 一致性)
            }
        }
        if (hctx)
            EVP_MD_CTX_free(hctx);
        _close(ffd);
        if (!ok || !zip.EndEntry())
            return false;
        if (onProgress)
            onProgress(zip.Written());   // 每条目进度(打包中任务行 total_size)
    }
    if (!zip.Finish())
        return false;
    outWritten = zip.Written();
    return true;
}

// ---- 打包中心产物行 filehub_packs 存取 ----

int FileHubModule::PacksInsert(const std::string& fp, int ownerUid, const std::string& taskId,
                               const std::string& origin, const std::string& packPath,
                               const std::string& itemInfo, int64_t itemCount)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "INSERT OR IGNORE INTO filehub_packs(fingerprint,user_id,task_id,origin,item_info,"
        "name,item_count,size,pack_path,status,create_time,access_time,err) "
        "VALUES(?,?,?,?,?,'',?,0,?,?,?,?,?)");
    if (!st.p)
        return -1;
    int64_t now = time(nullptr);
    BindText(st.p, 1, fp);
    BindInt(st.p, 2, (int64_t)ownerUid);
    BindText(st.p, 3, taskId);
    BindText(st.p, 4, origin);
    BindText(st.p, 5, itemInfo);          // 打包前清单+每文件哈希(§4.3 增量校验数据源)
    BindInt(st.p, 6, itemCount);
    BindText(st.p, 7, packPath);
    BindText(st.p, 8, kPackStatusPacking);
    BindInt(st.p, 9, now);
    BindInt(st.p, 10, now);               // access_time 与 create 同步起算(清理基准)
    BindText(st.p, 11, "");               // err(未绑定 → NULL 触发 NOT NULL 约束,
    if (sqlite3_step(st.p) != SQLITE_DONE)
        return -1;
    if (m_db.Changes() > 0)
        return 1;   // 主动作者
    return 0;       // 冲突:已有行(转入等待/复用判定)
}

bool FileHubModule::PacksLoad(const std::string& fp, int ownerUid, std::string& status,
                              std::string& packPath, std::string& name, int64_t& size,
                              int64_t& packIdOut)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "SELECT status,pack_path,name,size,id FROM filehub_packs WHERE fingerprint=? AND user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, fp);
    BindInt(st.p, 2, (int64_t)ownerUid);
    if (sqlite3_step(st.p) != SQLITE_ROW)
        return false;
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
    status = s ? s : "";
    packPath = p ? p : "";
    name = n ? n : "";
    size = sqlite3_column_int64(st.p, 3);
    packIdOut = sqlite3_column_int64(st.p, 4);
    return true;
}

bool FileHubModule::PacksUpdate(const std::string& fp, int ownerUid, const std::string& status,
                                int64_t size, const std::string& packPath, const std::string& err,
                                const std::string& name)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "UPDATE filehub_packs SET status=?,size=?,pack_path=?,err=?,access_time=?,"
        "name=CASE WHEN ?<>'' THEN ? ELSE name END "
        "WHERE fingerprint=? AND user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, status);
    BindInt(st.p, 2, size);
    BindText(st.p, 3, packPath);
    BindText(st.p, 4, err);
    BindInt(st.p, 5, (int64_t)time(nullptr));
    BindText(st.p, 6, name);
    BindText(st.p, 7, name);
    BindText(st.p, 8, fp);
    BindInt(st.p, 9, (int64_t)ownerUid);
    return sqlite3_step(st.p) == SQLITE_DONE;
}

bool FileHubModule::PacksRebindTask(const std::string& fp, int ownerUid,
                                    const std::string& taskId)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    // task_id 可被覆盖(§4.1):复用命中时把行指到本次触发任务,新任务的
    // zip_task_download/share_commit 才能按 task_id 找到产物;顺带续命
    Stmt st(m_db,
        "UPDATE filehub_packs SET task_id=?,access_time=? WHERE fingerprint=? AND user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, taskId);
    BindInt(st.p, 2, (int64_t)time(nullptr));
    BindText(st.p, 3, fp);
    BindInt(st.p, 4, (int64_t)ownerUid);
    return sqlite3_step(st.p) == SQLITE_DONE && m_db.Changes() > 0;
}

bool FileHubModule::PacksTryTakeOver(const std::string& fp, int ownerUid,
                                     const std::string& packPath, const std::string& taskId)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    // 原子接管 failed 行:仅 status='failed' 时生效(Changes() 判定),
    // 多个等待者并发重试时只有一个成为主动作者(§4.4 互斥语义的接管侧延伸);
    // task_id 同步改写为接管任务(§4.1 单槽归属跟随当前作者,防完成后行仍指向旧任务)
    Stmt st(m_db,
        "UPDATE filehub_packs SET status='packing',size=0,pack_path=?,err='',access_time=?,"
        "task_id=? "
        "WHERE fingerprint=? AND user_id=? AND status='failed'");
    if (!st.p)
        return false;
    BindText(st.p, 1, packPath);
    BindInt(st.p, 2, (int64_t)time(nullptr));
    BindText(st.p, 3, taskId);
    BindText(st.p, 4, fp);
    BindInt(st.p, 5, (int64_t)ownerUid);
    return sqlite3_step(st.p) == SQLITE_DONE && m_db.Changes() > 0;
}

bool FileHubModule::PacksFinish(const std::string& fpClaimed, const std::string& fpFinal,
                                int ownerUid, int64_t size, const std::string& packPath,
                                const std::string& name, int64_t& packIdOut)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    // 打包期指纹回填(§4.3):行以抢占期指纹定位,指纹漂移时重键为打包期指纹;
    // 以真实影响行数判定成功(SQLITE 下 UPDATE 0 行匹配同样返回 DONE)
    Stmt st(m_db,
        "UPDATE filehub_packs SET fingerprint=?,status='done',size=?,pack_path=?,err='',"
        "access_time=?,name=CASE WHEN ?<>'' THEN ? ELSE name END "
        "WHERE fingerprint=? AND user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, fpFinal);
    BindInt(st.p, 2, size);
    BindText(st.p, 3, packPath);
    BindInt(st.p, 4, (int64_t)time(nullptr));
    BindText(st.p, 5, name);
    BindText(st.p, 6, name);
    BindText(st.p, 7, fpClaimed);
    BindInt(st.p, 8, (int64_t)ownerUid);
    if (sqlite3_step(st.p) != SQLITE_DONE || m_db.Changes() <= 0)
        return false;
    // 回读完成行 id(重键后按终点指纹唯一匹配;供任务行 pack_id 回填)
    Stmt stId(m_db, "SELECT id FROM filehub_packs WHERE fingerprint=? AND user_id=?");
    if (!stId.p)
        return false;
    BindText(stId.p, 1, fpFinal);
    BindInt(stId.p, 2, (int64_t)ownerUid);
    if (sqlite3_step(stId.p) != SQLITE_ROW)
        return false;
    packIdOut = sqlite3_column_int64(stId.p, 0);
    return true;
}

bool FileHubModule::PacksPathForTask(const std::string& taskId, uint64_t userUid,
                                     std::string& packPath, int64_t& size,
                                     std::string& fpOut, int64_t& packIdOut)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    // ① 主路径:经任务行 pack_id(打包完成/复用命中时回填)。产物行 task_id 是单槽,
    //    复用/接管会覆盖(§4.1),多个任务引用同一产物时旧任务按 task_id 必 miss;
    //    每任务独立 pack_id 绑定后任意历史任务都能定位自己那份产物
    Stmt st1(m_db,
        "SELECT p.pack_path,p.size,p.status,p.fingerprint,p.id FROM filehub_packs p "
        "JOIN transfer_tasks t ON t.pack_id=p.id "
        "WHERE t.task_id=? AND t.user_id=?");
    if (st1.p)
    {
        BindText(st1.p, 1, taskId);
        BindInt(st1.p, 2, (int64_t)userUid);
        if (sqlite3_step(st1.p) == SQLITE_ROW)
        {
            const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st1.p, 2));
            if (!s || std::string(s) != kPackStatusDone)
                return false;   // 已绑定未就绪:按旧 task_id 兜底会误取其他任务产物,不作回退
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st1.p, 0));
            const char* f = reinterpret_cast<const char*>(sqlite3_column_text(st1.p, 3));
            packPath = p ? p : "";
            size = sqlite3_column_int64(st1.p, 1);
            fpOut = f ? f : "";
            packIdOut = sqlite3_column_int64(st1.p, 4);
            return !packPath.empty();
        }
    }
    // ② 存量回退:pack_id=0 的老行,仅按 task_id 定位(归属已由 transfer_tasks
    //    (task_id+user_id)校验;公共包行 user_id=0 与发起者 uid 不同,不得按 user_id 过滤)。
    //    同 task_id 多行仅见于复用重绑后的指纹漂移边角,取最新一行
    Stmt st(m_db,
        "SELECT pack_path,size,status,fingerprint,id FROM filehub_packs WHERE task_id=? "
        "ORDER BY id DESC LIMIT 1");
    if (!st.p)
        return false;
    BindText(st.p, 1, taskId);
    if (sqlite3_step(st.p) != SQLITE_ROW)
        return false;
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
    if (!s || std::string(s) != kPackStatusDone)
        return false;
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
    const char* f = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
    packPath = p ? p : "";
    size = sqlite3_column_int64(st.p, 1);
    fpOut = f ? f : "";
    packIdOut = sqlite3_column_int64(st.p, 4);
    return !packPath.empty();
}
// ---- 后台打包线程入口 ----

void FileHubModule::RunPackThread(std::shared_ptr<PackJob> job)
{
    DEFAULT_LOG_INFO("[FileHub] RunPackThread 启动: task={} space={}", job->taskId, job->space);
    auto failTask = [this, job](const std::string& err) {
        UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed, err, -1);
    };
    // 取消检查 = 服务关停 or 任务行已 failed(前端取消/清理线程兜底)
    auto cancelled = [this, job]() {
        if (m_gone.load())
            return false;
        std::string t, s, n, e;
        int64_t sz = 0;
        bool found = LoadTransferTask(job->taskId, job->opUid, t, s, n, sz, e);
        return found && s != kTaskStatusFailed;
    };

    // 1) 展开 + 指纹(读一遍;取消检测贯穿)
    std::vector<PackEntry> entries;
    std::string fp, itemInfo;
    int64_t cnt = 0;
    if (!ComputeFingerprint(job->space, job->ids, entries, fp, itemInfo, cnt, cancelled))
    {
        failTask(m_gone.load() ? "服务中断" : "无法访问打包内容");
        return;
    }
    DEFAULT_LOG_INFO("[FileHub] RunPackThread 指纹完成: task={} fp={} 条目数={}",
                     job->taskId, fp.substr(0, 16), (long long)cnt);
    std::string fp32 = fp.substr(0, 32);
    int owner = job->space;                      // 公共 0 / 个人 uid(§4.4 归属键)
    std::string packPath = PackPathFromFp(owner, fp32);

    // 2) 复用判定(原子抢占/等待;产物丢失/failed 可重打)
    bool becameAuthor = false;   // 抢占循环耗尽时不得裸落为作者(会覆盖真作者的行)
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (!cancelled())
        {
            failTask(m_gone.load() ? "服务中断" : "已取消");
            return;
        }
        std::string st, pp, nm;
        int64_t sz = 0;
        int64_t pid = 0;
        if (PacksLoad(fp, owner, st, pp, nm, sz, pid))
        {
            if (st == kPackStatusDone)
            {
                if (!pp.empty() && _access(pp.c_str(), 0) == 0)
                {
                    // 复用:不重打,产物行 task_id 重绑到本任务("可被覆盖")→ 任务行
                    // 回填本行 id(独立绑定,该任务永远可定位本产物)+ 行续命
                    PacksRebindTask(fp, owner, job->taskId);
                    SetTransferTaskPackId(job->taskId, job->opUid, pid);
                    UpdateTransferTask(job->taskId, job->opUid, kTaskStatusDone, "", sz);
                    Log(job->opUid, job->opAccount, "download", "zip", 0,
                        "{\"reuse\":\"" + fp32 + "\",\"size\":" + std::to_string((long long)sz) + "}");
                    return;
                }
                PacksUpdate(fp, owner, kPackStatusFailed, 0, pp, "产物丢失");
                continue;   // 重打:取 failed 行
            }
            if (st == kPackStatusPacking)
            {
                // 等待主动作者完成(1s 轮询;等待者取消不干扰主动作者)
                for (int w = 0; w < 3600; ++w)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                    if (!cancelled())
                        break;
                    std::string s2, p2, n2;
                    int64_t z2 = 0;
                    int64_t id2 = 0;
                    if (!PacksLoad(fp, owner, s2, p2, n2, z2, id2))
                        break;
                    if (s2 == kPackStatusDone && !p2.empty() && _access(p2.c_str(), 0) == 0)
                    {
                        // 复用命中:同上,task_id 重绑到本任务 + 行 id 回填(防本任务
                        // 与原作者任务联动时旧任务定位丢失)+ 续命
                        PacksRebindTask(fp, owner, job->taskId);
                        SetTransferTaskPackId(job->taskId, job->opUid, id2);
                        UpdateTransferTask(job->taskId, job->opUid, kTaskStatusDone, "", z2);
                        Log(job->opUid, job->opAccount, "download", "zip", 0,
                            "{\"reuse\":\"" + fp32 + "\",\"size\":" + std::to_string((long long)z2) + "}");
                        return;
                    }
                    if (s2 == kPackStatusFailed)
                        break;   // 主动作者失败:本线程接管重打
                }
                continue;
            }
            // failed → 原子接管该行(仅 failed 时生效;并发等待者只有一个成功)
            if (st == kPackStatusFailed)
            {
                // 接管即把行归属(单槽 task_id)改为本任务:任务完成之行仍指向本任务,
                // 不依赖老任务时 404(本次 404 的根因——直接按 task_id 查产物 miss)
                if (PacksTryTakeOver(fp, owner, packPath, job->taskId))
                {
                    becameAuthor = true;
                    break;   // 本线程成为主动作者
                }
                continue;    // 已被他人接管 → 下轮按 packing 转等待者
            }
        }
        else
        {
            int rc = PacksInsert(fp, owner, job->taskId, job->origin, packPath,
                                 itemInfo, cnt);
            if (rc == 1)
            {
                becameAuthor = true;
                break;   // 主动作者
            }
            if (rc < 0)
            {
                failTask("数据库写入失败");
                return;
            }
            continue;   // INSERT 冲突(他人在同时起跑)→ 循环重查(转等待/复用)
        }
    }

    // 3) 主动作者:打包 → .tmp → 原子改名 → 产物行 done
    if (!becameAuthor)
    {
        failTask("打包等待超时");   // 抢占/等待循环耗尽:放弃而非覆盖他人行
        return;
    }
    // 作者期失败收尾:任务行与 pack 行一并置 failed——pack 行停留 packing 会让
    // 等待者永久空转(指纹捕获值为抢占期键,行内 fingerprint 列在 PacksFinish 前不变)
    auto failAuthor = [this, job, fp, owner](const std::string& err) {
        UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed, err, -1);
        PacksUpdate(fp, owner, kPackStatusFailed, 0, "", err);
    };
    if (m_gone.load())
    {
        failAuthor("服务中断");
        return;
    }
    std::string ownerDir = PackTempDir() + "\\" + std::to_string((long long)owner);
    if (_mkdir(ownerDir.c_str()) != 0 && _access(ownerDir.c_str(), 0) != 0)
    {
        failAuthor("无法创建产物目录");
        return;
    }
    std::string tmpDir = PackTempDir() + "\\.tmp";
    if (_mkdir(tmpDir.c_str()) != 0 && _access(tmpDir.c_str(), 0) != 0)
    {
        failAuthor("无法创建临时目录");
        return;
    }
    std::string tmp = PackTmpPath(job->taskId);
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(tmp).c_str(),
                  _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE) != 0 || fd == -1)
    {
        failAuthor("无法创建打包文件");
        return;
    }
    uint64_t written = 0;
    bool ok = PackEntriesToFd(fd, job->space, entries, cancelled, written,
        [this, job, last = (uint64_t)0](uint64_t w) mutable {   // 进度:≥64MB 粒度刷 total_size
            if (w - last < kPackProgressFlushBytes)
                return;   // 小文件逐条目写库会放大 DB 锁竞争
            last = w;
            UpdateTransferTask(job->taskId, job->opUid, kTaskStatusPacking, "", (int64_t)w);
        });
    _close(fd);
    if (!ok)
    {
        _unlink(tmp.c_str());
        failAuthor(m_gone.load() ? "服务中断" : "打包中内容变化或已取消");
        return;
    }
    // 打包期指纹回填(两次读取间内容变化 → 以打包期为准,包内容自洽)
    std::vector<PackEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const PackEntry& a, const PackEntry& b) {
        return a.relPath < b.relPath;
    });
    std::string agg;
    for (auto& e : sorted)
    {
        agg += e.relPath;
        agg.push_back('\0');
        agg += std::to_string((long long)e.size);
        agg.push_back('\0');
        agg += (e.type == "file") ? e.fileHash : "";
        agg.push_back('\0');
    }
    std::string fpNow = HashHex(agg);
    std::string fpClaimed = fp;   // 抢占期指纹(pack 行定位键)
    if (fpNow != fp)
    {
        fp = fpNow;
        fp32 = fp.substr(0, 32);
        packPath = PackPathFromFp(owner, fp32);
    }
    if (!MoveFileExW(ZmString::UTF8_To_Unicode(tmp).c_str(),
                     ZmString::UTF8_To_Unicode(packPath).c_str(), 0))
    {
        _unlink(tmp.c_str());
        failAuthor("产物落位失败");
        return;
    }
    // 落库:以抢占期指纹定位行、重键为打包期指纹(真实影响行数判定,
    // 防 0 行匹配假成功 → 产物孤儿 + 行永久卡 packing)
    int64_t packIdDone = 0;
    if (!PacksFinish(fpClaimed, fp, owner, (int64_t)written, packPath, job->zipName,
                     packIdDone))
    {
        _unlink(packPath.c_str());
        failAuthor("产物入库失败");
        return;
    }
    // 任务行 pack_id 回填:本任务独立绑定此产物(§4.1 单槽可被后续复用/接管覆盖,
    // 但任务行绑定是每任务私有,历史任务不再因归属转移而 404)
    SetTransferTaskPackId(job->taskId, job->opUid, packIdDone);
    UpdateTransferTask(job->taskId, job->opUid, kTaskStatusDone, "", (int64_t)written);
    DEFAULT_LOG_INFO("[FileHub] RunPackThread 完成: task={} size={} fp={}", job->taskId,
                     (long long)written, fp32);
    Log(job->opUid, job->opAccount, "download", "zip", 0,
        "{\"name\":\"" + JsonEscape(job->zipName) + "\",\"fp\":\"" + fp32 +
        "\",\"size\":" + std::to_string((long long)written) +
        ",\"count\":" + std::to_string((long long)cnt) + "}");
}

// ---- 打包中心清理(周期 + 启动) ----

void FileHubModule::PackCleanup()
{
    int64_t now = time(nullptr);
    int64_t retainSec = 30LL * 24 * 3600;
    // 1) 过期产物行(被活跃分享引用 target_type='pack' 的不删)+ 删产物文件
    std::vector<std::pair<std::string, int>> expired;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "SELECT p.fingerprint,p.user_id,p.pack_path FROM filehub_packs p "
            "WHERE p.status='done' AND p.access_time<? AND NOT EXISTS("
            "  SELECT 1 FROM shares s WHERE s.target_type='pack' AND s.target_id=p.id)");
        if (!st.p)
            return;
        BindInt(st.p, 1, (int64_t)(now - retainSec));
        while (sqlite3_step(st.p) == SQLITE_ROW)
        {
            const char* fp = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
            int owner = (int)sqlite3_column_int64(st.p, 1);
            const char* pp = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
            expired.emplace_back(fp ? fp : "", owner);
            if (pp && pp[0])
                _unlink(pp);
        }
    }
    for (auto& [fp, owner] : expired)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "DELETE FROM filehub_packs WHERE fingerprint=? AND user_id=?");
        if (st.p)
        {
            BindText(st.p, 1, fp);
            BindInt(st.p, 2, (int64_t)owner);
            sqlite3_step(st.p);
        }
    }
    // 2) .tmp 残留与孤儿产物(无 pack 行)清扫:遍历 pack_temp(含 .tmp 子目录)
    std::vector<std::string> cleanFiles;
    std::set<std::string> tracked;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT pack_path FROM filehub_packs WHERE status IN ('done','failed')");
        if (st.p)
            while (sqlite3_step(st.p) == SQLITE_ROW)
            {
                const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                if (p)
                    tracked.insert(p);
            }
    }
    std::function<void(const std::string&, bool)> sweep =
        [&](const std::string& dirAbs, bool isTmp) {
            std::string pat = dirAbs + "\\*";
            _finddata_t fd;
            intptr_t h = _findfirst(pat.c_str(), &fd);
            if (h == -1)
                return;
            do
            {
                if (fd.name[0] == '.')
                    continue;
                std::string full = dirAbs + "\\" + fd.name;
                if (fd.attrib & _A_SUBDIR)
                {
                    if (!isTmp)
                        sweep(full, false);
                }
                else if (isTmp)
                {
                    // .tmp 仅清 1 天前的残留(§4.2):新近文件可能是在打任务的半成品
                    if ((int64_t)fd.time_write > 0 &&
                        (int64_t)fd.time_write <= now - 86400)
                        cleanFiles.push_back(full);
                }
                else if (!tracked.count(full))
                {
                    // 孤儿产物:无任何 pack 行引用(含 packing 行指向的正式名,不受影响)
                    cleanFiles.push_back(full);
                }
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        };
    sweep(PackTempDir(), false);
    sweep(PackTempDir() + "\\.tmp", true);
    for (auto& f : cleanFiles)
        _unlink(f.c_str());
}

// ---- 打包中心端点 ----

void FileHubModule::HandleZipStart(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid,
                                   const std::string& opAccount)
{
    DEFAULT_LOG_INFO("[FileHub] ZipStart 进入: uid={} query={}", opUid,
                     task->GetQueryValue("task_id", "") ? task->GetQueryValue("task_id", "") : "?");
    const char* tid = task->GetQueryValue("task_id", "");
    if (!ValidTaskId(tid ? tid : ""))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    std::string taskId = tid;

    // ids 解析:逗号分隔,前缀 f=文件/d=目录(如 "f12,d34"),与 JSON 形态等价
    std::vector<std::pair<std::string, uint64_t>> ids;
    const char* idsStr = task->GetQueryValue("ids", "");
    if (idsStr && idsStr[0])
    {
        std::string s = idsStr;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = (comma == std::string::npos) ? s.size() : comma + 1;
            if (tok.size() < 2)
                continue;
            char typeCh = tok[0];
            if (typeCh != 'f' && typeCh != 'd')
                continue;
            char* e = nullptr;
            long long v = strtoll(tok.c_str() + 1, &e, 10);
            if (e && *e == '\0' && v > 0)
                ids.emplace_back(typeCh == 'f' ? "file" : "dir", (uint64_t)v);
        }
    }
    if (ids.empty())
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    // 校验条目存在性 + 空间一致(全部条目须同属一个空间,防跨空间混打)
    int space = -1;
    for (auto& it : ids)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (it.first == "file")
        {
            Stmt st(m_db, "SELECT space,name FROM files WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)it.second);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                return;
            }
            int sp = (int)sqlite3_column_int64(st.p, 0);
            if (space < 0) space = sp;
            else if (sp != space)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "空间不匹配");
                return;
            }
        }
        else
        {
            Stmt st(m_db, "SELECT space,name FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)it.second);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                return;
            }
            int sp = (int)sqlite3_column_int64(st.p, 0);
            if (space < 0) space = sp;
            else if (sp != space)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "空间不匹配");
                return;
            }
        }
    }
    if (space < 0)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    // 个人空间仅本人可打包(与单文件下载同规则;ids 可被他人枚举,必须显式校验)
    if (space != 0 && opUid != (uint64_t)space)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可下载");
        return;
    }

    // zip 文件名:单目录 = 目录名.zip;其余 = ZiMo文件中心-打包下载_<时间戳>.zip
    std::string zipName = "ZiMo文件中心-打包下载_" + std::to_string((int64_t)time(nullptr)) + ".zip";
    if (ids.size() == 1 && ids[0].first == "dir")
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name FROM dirs WHERE id=?");
        if (st.p)
        {
            BindInt(st.p, 1, (int64_t)ids[0].second);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                if (n && n[0])
                    zipName = std::string(n) + ".zip";
            }
        }
    }

    // 同一 task_id 行已存在且仍在打包 → 400(防重复触发)
    {
        std::string t, s, n, e;
        int64_t sz = 0;
        if (LoadTransferTask(taskId, opUid, t, s, n, sz, e) && s == kTaskStatusPacking)
        {
            ZmReqLoopRest::ResponseError(loop, 400, "任务进行中");
            return;
        }
    }

    // 建行(packing) → 后台独立线程 → 立即返回
    if (!CreateTransferTask(taskId, opUid, "zip", kTaskStatusPacking, zipName, 0))
    {
        DEFAULT_LOG_ERROR("[FileHub] ZipStart 建行失败: task={} uid={}", taskId, opUid);
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    DEFAULT_LOG_INFO("[FileHub] ZipStart 建行成功: task={} uid={} ids={} space={} name={}",
                     taskId, opUid, (int)ids.size(), space, zipName);
    auto job = std::make_shared<PackJob>();
    job->taskId = taskId;
    job->opUid = opUid;
    job->opAccount = opAccount;
    job->space = space;
    job->ids = ids;
    job->zipName = zipName;
    job->origin = "download";
    std::thread th([this, job]() {
        try
        {
            RunPackThread(job);
        }
        catch (const std::exception& e)
        {
            DEFAULT_LOG_ERROR("[FileHub] 打包线程异常: task={} what={}", job->taskId, e.what());
            UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed,
                               std::string("打包异常:") + e.what(), -1);
        }
        catch (...)
        {
            DEFAULT_LOG_ERROR("[FileHub] 打包线程异常(未知): task={}", job->taskId);
            UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed, "打包异常:未知", -1);
        }
    });
    th.detach();
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"task_id", taskId}, {"status", kPackStatusPacking}}}});
}

void FileHubModule::HandleZipTaskDownload(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid)
{
    const char* tid = task->GetQueryValue("task_id", "");
    if (!ValidTaskId(tid ? tid : ""))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    std::string taskId = tid;
    std::string type, status, name, err;
    int64_t totalSize = 0;
    if (!LoadTransferTask(taskId, opUid, type, status, name, totalSize, err))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "任务不存在");
        return;
    }
    if (status != kTaskStatusDone)
    {
        ZmReqLoopRest::ResponseError(loop, 404, "任务未就绪");
        return;
    }
    std::string packPath;
    int64_t sz = 0;
    std::string fp; int64_t packId = 0;
    if (!PacksPathForTask(taskId, opUid, packPath, sz, fp, packId) ||
        _access(packPath.c_str(), 0) != 0)
    {
        ZmReqLoopRest::ResponseError(loop, 404, "打包产物不存在");
        return;
    }
    std::string dispName = name.empty() ? "filehub.zip" : name;
    // 混合发送(§3.2):<2GB 分段零拷贝 / ≥2GB 分块回退;两路径均支持 Range 续传
    int rc = SendFileHybrid(loop, task, packPath, dispName, task->GetRequestHeader("Range"));
    if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
        ZmReqLoopRest::ResponseError(loop, 404, "打包产物不存在");
    else if (rc == ZM_HTTP_STATUS_CODE_OK || rc == ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT)
        Log(opUid, "", "download", "zip", 0,
            "{\"name\":\"" + JsonEscape(dispName) + "\",\"size\":" + std::to_string((long long)sz) + "}");
}

// ============================================================================
// 传输任务(transfer_tasks:上传/单文件下载/zip 打包统一记录,队列历史数据源)
// ============================================================================

bool FileHubModule::CreateTransferTask(const std::string& taskId, uint64_t userUid,
                                       const std::string& type, const std::string& status,
                                       const std::string& name, int64_t totalSize)
{
    if (taskId.empty() || userUid == 0)
        return false;
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "INSERT OR IGNORE INTO transfer_tasks(task_id,user_id,type,status,name,total_size,"
        "err,create_time,update_time) VALUES(?,?,?,?,?,?,'',?,?)");
    if (!st.p)
        return false;
    int64_t now = time(nullptr);
    BindText(st.p, 1, taskId);
    BindInt(st.p, 2, (int64_t)userUid);
    BindText(st.p, 3, type);
    BindText(st.p, 4, status);
    BindText(st.p, 5, name);
    BindInt(st.p, 6, totalSize);
    BindInt(st.p, 7, now);
    BindInt(st.p, 8, now);
    return sqlite3_step(st.p) == SQLITE_DONE;
}

bool FileHubModule::UpdateTransferTask(const std::string& taskId, uint64_t userUid,
                                       const std::string& status, const std::string& err,
                                       int64_t totalSize)
{
    if (taskId.empty() || userUid == 0)
        return false;
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    // 仅当行存在且归属匹配时更新;status 终态后(取消/中断/失败)不再被完成路径覆盖
    Stmt st(m_db,
        "UPDATE transfer_tasks SET status=?, err=?, total_size=MAX(total_size, ?), "
        "update_time=? WHERE task_id=? AND user_id=? AND status NOT IN ('done','failed')");
    if (!st.p)
        return false;
    BindText(st.p, 1, status);
    BindText(st.p, 2, err);
    BindInt(st.p, 3, totalSize);
    BindInt(st.p, 4, (int64_t)time(nullptr));
    BindText(st.p, 5, taskId);
    BindInt(st.p, 6, (int64_t)userUid);
    return sqlite3_step(st.p) == SQLITE_DONE;
}

bool FileHubModule::SetTransferTaskPackId(const std::string& taskId, uint64_t userUid,
                                          int64_t packId)
{
    if (taskId.empty() || userUid == 0 || packId <= 0)
        return false;
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db, "UPDATE transfer_tasks SET pack_id=? WHERE task_id=? AND user_id=?");
    if (!st.p)
        return false;
    BindInt(st.p, 1, packId);
    BindText(st.p, 2, taskId);
    BindInt(st.p, 3, (int64_t)userUid);
    return sqlite3_step(st.p) == SQLITE_DONE;
}

bool FileHubModule::LoadTransferTask(const std::string& taskId, uint64_t userUid,
                                     std::string& type, std::string& status,
                                     std::string& name, int64_t& totalSize, std::string& err)
{
    if (taskId.empty() || userUid == 0)
        return false;
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "SELECT type,status,name,total_size,err FROM transfer_tasks "
        "WHERE task_id=? AND user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, taskId);
    BindInt(st.p, 2, (int64_t)userUid);
    if (sqlite3_step(st.p) != SQLITE_ROW)
        return false;
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 4));
    type = t ? t : "";
    status = s ? s : "";
    name = n ? n : "";
    err = e ? e : "";
    totalSize = sqlite3_column_int64(st.p, 3);
    return true;
}

void FileHubModule::HandleTaskCreate(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid)
{
    std::string taskId = zm_json_get_str(body, "task_id");
    if (!ValidTaskId(taskId))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    std::string name = zm_json_get_str(body, "name");
    if (!IsValidName(name))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "文件名不合法");
        return;
    }
    int64_t size = JsonInt(body, "size", 0);
    if (size < 0)
        size = 0;
    // 上传预建行:浏览器关闭后行留痕,重开队列可见"已中断"(HandleTasks 90s 标记)
    CreateTransferTask(taskId, opUid, "upload", kTaskStatusUploading, name, size);
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"task_id", taskId}}}});
}

void FileHubModule::HandleTaskCancel(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid)
{
    std::string taskId = zm_json_get_str(body, "task_id");
    if (!ValidTaskId(taskId))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    // 行置 failed"已取消":上传请求已由前端 abort;打包循环逐文件检测到即中止
    UpdateTransferTask(taskId, opUid, kTaskStatusFailed, "已取消", -1);
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"task_id", taskId}}}});
}

void FileHubModule::HandleTaskDelete(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid)
{
    std::string taskId = zm_json_get_str(body, "task_id");
    if (!ValidTaskId(taskId))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "DELETE FROM transfer_tasks WHERE task_id=? AND user_id=?");
        if (st.p)
        {
            BindText(st.p, 1, taskId);
            BindInt(st.p, 2, (int64_t)opUid);
            sqlite3_step(st.p);
        }
    }
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"task_id", taskId}}}});
}

void FileHubModule::HandleTaskStatus(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid)
{
    const char* tid = task->GetQueryValue("task_id", "");
    std::string type, status, name, err;
    int64_t totalSize = 0;
    // 行缺失(打包请求尚未建行/已删除)返回 404,前端视为"启动中"继续轮询
    if (!LoadTransferTask(tid ? tid : "", opUid, type, status, name, totalSize, err))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "任务不存在");
        return;
    }
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"task_id", tid ? tid : ""}, {"type", type}, {"status", status},
                     {"name", name}, {"total_size", (int64_t)totalSize}, {"err", err}}}});
}

void FileHubModule::HandleTasks(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid)
{
    // 1) 中断标记:超 90s 仍 uploading 的行 → failed"已中断"
    //    (前端上传 60s 即超时放弃,活任务不可能超过 90s;packing/triggered 不在此列,
    //     打包/下载可合法长时间进行,由清理线程兜底)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "UPDATE transfer_tasks SET status=?, err='已中断', update_time=? "
                      "WHERE user_id=? AND status=? AND update_time<?");
        if (st.p)
        {
            int64_t now = time(nullptr);
            BindText(st.p, 1, kTaskStatusFailed);
            BindInt(st.p, 2, now);
            BindInt(st.p, 3, (int64_t)opUid);
            BindText(st.p, 4, kTaskStatusUploading);
            BindInt(st.p, 5, (int64_t)(now - kTaskStaleSec));
            sqlite3_step(st.p);
            if (m_db.Changes() > 0)
                DEFAULT_LOG_INFO("[FileHub] 标记上传中断任务 {} 个", m_db.Changes());
        }
    }

    // 2) 分页查询(按 id 倒序,最新在前)
    const char* pageStr = task->GetQueryValue("page", "0");
    char* end = nullptr;
    long long page = pageStr ? strtoll(pageStr, &end, 10) : 0;
    if (!end || *end != '\0' || page < 0)
        page = 0;

    ZMJSON rsp;
    rsp["result"]["tasks"] = ZMJSON::array();
    rsp["result"]["hasMore"] = false;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT task_id,type,status,name,total_size,err,create_time "
                      "FROM transfer_tasks WHERE user_id=? ORDER BY id DESC LIMIT ? OFFSET ?");
        if (st.p)
        {
            BindInt(st.p, 1, (int64_t)opUid);
            BindInt(st.p, 2, kTaskPageSize + 1);   // 多取 1 判 hasMore
            BindInt(st.p, 3, page * kTaskPageSize);
            int count = 0;
            while (sqlite3_step(st.p) == SQLITE_ROW)
            {
                if (count >= kTaskPageSize)
                {
                    rsp["result"]["hasMore"] = true;
                    break;
                }
                const char* tid = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                const char* ty = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
                const char* ss = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
                const char* nm = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
                const char* er = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 5));
                ZMJSON t;
                t["task_id"] = tid ? tid : "";
                t["type"] = ty ? ty : "";
                t["status"] = ss ? ss : "";
                t["name"] = nm ? nm : "";
                t["total_size"] = sqlite3_column_int64(st.p, 4);
                t["err"] = er ? er : "";
                t["create_time"] = sqlite3_column_int64(st.p, 6);
                rsp["result"]["tasks"].push_back(std::move(t));
                ++count;
            }
        }
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

// ============================================================================
// 分享
// ============================================================================

void FileHubModule::HandleShareCreate(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                      const std::string& opAccount)
{
    // items 解析(前端多目标 {items:[{type,id}...]} ≤50 项;兼容旧 {type,id} 单目标)
    std::vector<std::pair<std::string, uint64_t>> items;
    if (body.contains("items") && body["items"].is_array())
    {
        for (auto& it : body["items"])
        {
            std::string type = zm_json_get_str(it, "type");
            uint64_t id = (uint64_t)JsonInt(it, "id", 0);
            if ((type != "file" && type != "dir") || id == 0)
            {
                ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
                return;
            }
            items.push_back({type, id});
        }
    }
    else
    {
        std::string type = zm_json_get_str(body, "type");
        uint64_t id = (uint64_t)JsonInt(body, "id", 0);
        if ((type == "file" || type == "dir") && id > 0)
            items.push_back({type, id});
    }
    if (items.empty() || items.size() > 50)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    // 校验空间一致 + 归属(公共任何登录用户可分享;个人仅本人)
    int space = -1;
    for (auto& it : items)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (it.first == "file")
        {
            Stmt st(m_db, "SELECT space FROM files WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)it.second);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                return;
            }
            int sp = (int)sqlite3_column_int64(st.p, 0);
            if (space < 0) space = sp;
            else if (sp != space)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "空间不匹配");
                return;
            }
        }
        else
        {
            Stmt st(m_db, "SELECT space FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)it.second);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                return;
            }
            int sp = (int)sqlite3_column_int64(st.p, 0);
            if (space < 0) space = sp;
            else if (sp != space)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "空间不匹配");
                return;
            }
        }
    }
    if (space < 0)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    if (space != 0 && opUid != (uint64_t)space)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可分享");
        return;
    }

    // 单文件 → 直传分享(不打包,保持现状语义)
    if (items.size() == 1 && items[0].first == "file")
    {
        std::string token;
        if (!GenToken(token))
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db,
                "INSERT INTO shares(token_hash,token,owner_id,target_type,target_id,create_time) "
                "VALUES(?,?,?,?,?,?)");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindText(st.p, 1, Sha256Hex(token));
            BindText(st.p, 2, token);
            BindInt(st.p, 3, (int64_t)opUid);
            BindText(st.p, 4, "file");
            BindInt(st.p, 5, (int64_t)items[0].second);
            BindInt(st.p, 6, (int64_t)time(nullptr));
            if (sqlite3_step(st.p) != SQLITE_DONE)
            {
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
                return;
            }
        }
        Log(opUid, opAccount, "share", "file", items[0].second, "{\"token\":\"" + token + "\"}");
        ZmReqLoopRest::ResponseJson(loop, 200,
            {{"result", {{"url", "/share/" + token}}}});
        return;
    }

    // 单目录/多目标 → 打包快照分享:启动打包任务,前端轮询完成后 share_commit
    std::string taskId;
    if (!GenToken(taskId))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    std::string zipName = "ZiMo文件中心-分享_" + std::to_string((int64_t)time(nullptr)) + ".zip";
    if (items.size() == 1 && items[0].first == "dir")
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name FROM dirs WHERE id=?");
        if (st.p)
        {
            BindInt(st.p, 1, (int64_t)items[0].second);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                if (n && n[0])
                    zipName = std::string(n) + ".zip";
            }
        }
    }
    if (!CreateTransferTask(taskId, opUid, "zip", kTaskStatusPacking, zipName, 0))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    auto job = std::make_shared<PackJob>();
    job->taskId = taskId;
    job->opUid = opUid;
    job->opAccount = opAccount;
    job->space = space;
    job->ids = items;
    job->zipName = zipName;
    job->origin = "share";
    std::thread th([this, job]() {
        try
        {
            RunPackThread(job);
        }
        catch (const std::exception& e)
        {
            DEFAULT_LOG_ERROR("[FileHub] 分享打包线程异常: task={} what={}", job->taskId, e.what());
            UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed,
                               std::string("打包异常:") + e.what(), -1);
        }
        catch (...)
        {
            DEFAULT_LOG_ERROR("[FileHub] 分享打包线程异常(未知): task={}", job->taskId);
            UpdateTransferTask(job->taskId, job->opUid, kTaskStatusFailed, "打包异常:未知", -1);
        }
    });
    th.detach();
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"needs_pack", true}, {"task_id", taskId}, {"status", kPackStatusPacking}}}});
}

void FileHubModule::HandleShareCommit(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                      const std::string& opAccount)
{
    std::string taskId = zm_json_get_str(body, "task_id");
    if (!ValidTaskId(taskId))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    std::string type, status, name, err;
    int64_t totalSize = 0;
    if (!LoadTransferTask(taskId, opUid, type, status, name, totalSize, err))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "打包任务不存在");
        return;
    }
    if (type != "zip" || status != kTaskStatusDone)
    {
        ZmReqLoopRest::ResponseError(loop, 409, "打包尚未完成");
        return;
    }
    std::string packPath, fp;
    int64_t sz = 0;
    int64_t packId = 0;
    if (!PacksPathForTask(taskId, opUid, packPath, sz, fp, packId) ||
        _access(packPath.c_str(), 0) != 0)
    {
        ZmReqLoopRest::ResponseError(loop, 404, "打包产物不存在");
        return;
    }
    std::string token;
    if (!GenToken(token))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "INSERT INTO shares(token_hash,token,owner_id,target_type,target_id,create_time) "
            "VALUES(?,?,?,?,?,?)");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindText(st.p, 1, Sha256Hex(token));
        BindText(st.p, 2, token);
        BindInt(st.p, 3, (int64_t)opUid);
        BindText(st.p, 4, "pack");
        BindInt(st.p, 5, packId);
        BindInt(st.p, 6, (int64_t)time(nullptr));
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
    }
    Log(opUid, opAccount, "share", "pack", (uint64_t)packId, "{\"token\":\"" + token + "\"}");
    ZmReqLoopRest::ResponseJson(loop, 200,
        {{"result", {{"url", "/share/" + token}}}});
}

void FileHubModule::HandleShareAccess(ZmReqLoop* loop, ZmHttpdTask* task, const std::string& token)
{
    loop->CancelDeadline();   // 根治:文件下载 SendFile+TriggerReply 手动路径,防 deadline 双信号(同 HandleDownload)

    if (token.empty() || token.size() != 64)
    {
        ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
        return;
    }
    int64_t shareId = 0;
    uint64_t ownerId = 0;
    std::string targetType;
    uint64_t targetId = 0;
    int space = -1;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "SELECT id,owner_id,target_type,target_id FROM shares WHERE token_hash=?");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindText(st.p, 1, Sha256Hex(token));
        if (sqlite3_step(st.p) != SQLITE_ROW)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        shareId = sqlite3_column_int64(st.p, 0);
        ownerId = (uint64_t)sqlite3_column_int64(st.p, 1);
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
        targetType = t ? t : "";
        targetId = (uint64_t)sqlite3_column_int64(st.p, 3);

        // 目标存在性 + 空间
        if (targetType == "file")
        {
            Stmt st2(m_db, "SELECT space FROM files WHERE id=?");
            if (!st2.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st2.p, 1, (int64_t)targetId);
            if (sqlite3_step(st2.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
                return;
            }
            space = (int)sqlite3_column_int64(st2.p, 0);
        }
        else if (targetType == "dir")
        {
            Stmt st2(m_db, "SELECT space FROM dirs WHERE id=?");
            if (!st2.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st2.p, 1, (int64_t)targetId);
            if (sqlite3_step(st2.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
                return;
            }
            space = (int)sqlite3_column_int64(st2.p, 0);
        }
        else
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
    }

    // 个人空间分享:需登录(任何登录用户可下);公共分享:免登录
    uint64_t downloaderId = 0;
    std::string downloaderAccount;
    if (space != 0)
    {
        UserModule::UserInfo ui;
        uint64_t uid = m_userModule->AuthAndTouch(task, &ui);
        if (!uid)
        {
            // 需求:个人分享未登录 → 返回到登录页面(登录成功回跳分享链接)
            // ★ Location 必须是页面端口(443)完整 URL:相对路径会基于 39441 解析,
            //   落到 REST 端口无页面 → 浏览器空白
            const char* host = task->GetRequestHeader("Host");
            std::string hostname = host ? host : "localhost";
            size_t colon = hostname.find(':');
            if (colon != std::string::npos)
                hostname = hostname.substr(0, colon);
            std::string loc = "https://" + hostname + "/login?redirect=/share/" + token +
                              "&hint=" + PercentEncode("此分享需登录后访问,请先登录");
            ZmReqLoopRest::ResponseRedirect(loop, loc.c_str(), 302);
            return;
        }
        downloaderId = uid;
        downloaderAccount = ui.account;
    }

    // 下载(文件直接流 / 目录 zip 顶层名)
    std::string name;
    if (targetType == "file")
    {
        uint64_t dirId = 0;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db, "SELECT dir_id,name FROM files WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)targetId);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
                return;
            }
            dirId = (uint64_t)sqlite3_column_int64(st.p, 0);
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            name = n ? n : "";
        }
        std::string absPath = EntryAbsPath(space, dirId, name);
        // 混合发送(§3.2):<2GB 分段零拷贝 / ≥2GB 分块回退;两路径均支持 Range 续传
        int rc = SendFileHybrid(loop, task, absPath, name, task->GetRequestHeader("Range"));
        if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        if (rc == ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE)
            return;   // 416 已回复
        // 流式响应已接管回复,不再 TriggerReply
    }
    else
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT name FROM dirs WHERE id=?");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)targetId);
        if (sqlite3_step(st.p) != SQLITE_ROW)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        name = n ? n : "";
        if (name.empty())
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
    }

    // 记 downloads(分享下载日志)
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db,
            "INSERT INTO share_download_logs(share_id,target_type,target_id,downloader_id,"
            "downloader_account,ip,create_time) VALUES(?,?,?,?,?,?,?)");
        if (st.p)
        {
            BindInt(st.p, 1, shareId);
            BindText(st.p, 2, targetType);
            BindInt(st.p, 3, (int64_t)targetId);
            BindInt(st.p, 4, (int64_t)downloaderId);
            BindText(st.p, 5, downloaderAccount);
            std::string ip = task->Ip() ? task->Ip() : "";
            BindText(st.p, 6, ip);
            BindInt(st.p, 7, (int64_t)time(nullptr));
            sqlite3_step(st.p);
        }
    }
    Log(downloaderId, downloaderAccount, "download_share", targetType, targetId,
        "{\"share_id\":" + std::to_string(shareId) + "}");

    // 产物分享(pack):直接发送打包快照(Range 续传;<2GB 零拷贝,≥2GB 分块流式)
    if (targetType == "pack")
    {
        std::string packPath, packName, packStatus;
        int64_t packSize = 0;
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db, "SELECT pack_path,name,size,status FROM filehub_packs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)targetId);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
                return;
            }
            const char* pp = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
            const char* pn = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
            const char* ps = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
            packPath = pp ? pp : "";
            packName = pn ? pn : "filehub.zip";
            packStatus = ps ? ps : "";
            packSize = sqlite3_column_int64(st.p, 2);
        }
        if (packStatus != kPackStatusDone || packPath.empty() ||
            _access(packPath.c_str(), 0) != 0)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        if (packSize > 0)
        {
            std::lock_guard<std::mutex> lk(m_db.Mutex());
            Stmt st(m_db, "UPDATE filehub_packs SET access_time=? WHERE id=?");
            if (st.p)
            {
                BindInt(st.p, 1, (int64_t)time(nullptr));
                BindInt(st.p, 2, (int64_t)targetId);
                sqlite3_step(st.p);
            }
        }
        // 记下载日志:上方泛化分支已按 targetType='pack' 写入 share_download_logs
        // 与审计,此处不再重复;直接混合发送产物(下载名用 packs.name)
        int rc = SendFileHybrid(loop, task, packPath, packName,
                                task->GetRequestHeader("Range"));
        if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
        return;
    }

    // 旧式目录分享:按需打包(临时文件 → 流式;残留由 PackCleanup 清理)
    if (targetType == "dir")
    {
        loop->CancelDeadline();
        auto cancelled = [this]() { return !m_gone.load(); };
        std::vector<PackEntry> entries;
        std::vector<std::pair<std::string, uint64_t>> ids = {{"dir", targetId}};
        if (!ExpandPackItems(space, ids, entries, cancelled))
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        std::string tmp = PackTmpPath("lgs" + std::to_string((long long)shareId));
        uint64_t written = 0;
        int fd = -1;
        if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(tmp).c_str(),
                      _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _SH_DENYNO,
                      _S_IREAD | _S_IWRITE) != 0 || fd == -1)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        bool ok = PackEntriesToFd(fd, space, entries, cancelled, written);
        _close(fd);
        if (!ok)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        int rc = SendFileStream(loop, task, tmp, task->GetRequestHeader("Range"));
        if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
        return;
    }
}

void FileHubModule::HandleShareCancel(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                      const std::string& opAccount)
{
    uint64_t shareId = (uint64_t)JsonInt(body, "share_id", 0);
    if (shareId == 0)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    std::string targetType;
    uint64_t targetId = 0;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT owner_id,target_type,target_id FROM shares WHERE id=?");
        if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
        BindInt(st.p, 1, (int64_t)shareId);
        if (sqlite3_step(st.p) != SQLITE_ROW)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享不存在");
            return;
        }
        if ((uint64_t)sqlite3_column_int64(st.p, 0) != opUid)
        {
            ZmReqLoopRest::ResponseError(loop, 403, "仅分享创建者可取消分享");
            return;
        }
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        targetType = t ? t : "";
        targetId = (uint64_t)sqlite3_column_int64(st.p, 2);
        Stmt del(m_db, "DELETE FROM shares WHERE id=?");
        if (del.p)
        {
            BindInt(del.p, 1, (int64_t)shareId);
            sqlite3_step(del.p);
        }
    }
    Log(opUid, opAccount, "cancel_share", targetType, targetId, "{\"share_id\":" + std::to_string(shareId) + "}");
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void FileHubModule::HandleShareList(ZmReqLoop* loop, uint64_t opUid)
{
    ZMJSON rsp;
    rsp["result"]["shares"] = ZMJSON::array();   // 空分享也返回空数组,防前端 null 访问
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db,
        "SELECT id,target_type,target_id,create_time,token FROM shares WHERE owner_id=? "
        "ORDER BY create_time DESC");
    if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
    BindInt(st.p, 1, (int64_t)opUid);
    while (sqlite3_step(st.p) == SQLITE_ROW)
    {
        uint64_t shareId = (uint64_t)sqlite3_column_int64(st.p, 0);
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        std::string targetType = t ? t : "";
        uint64_t targetId = (uint64_t)sqlite3_column_int64(st.p, 2);
        const char* tok = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 4));
        ZMJSON s;
        s["id"] = (int64_t)shareId;
        s["type"] = targetType;
        s["targetId"] = (int64_t)targetId;
        s["createTime"] = sqlite3_column_int64(st.p, 3);
        s["url"] = "/share/" + std::string(tok ? tok : "");
        // 目标名 + 所在空间(已删除则标记失效)
        std::string targetName;
        int targetSpace = -1;
        bool alive = false;
        if (targetType == "file")
        {
            Stmt st2(m_db, "SELECT name,space FROM files WHERE id=?");
            if (st2.p)
            {
                BindInt(st2.p, 1, (int64_t)targetId);
                if (sqlite3_step(st2.p) == SQLITE_ROW)
                {
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st2.p, 0));
                    targetName = n ? n : "";
                    targetSpace = (int)sqlite3_column_int64(st2.p, 1);
                    alive = true;
                }
            }
        }
        else if (targetType == "pack")
        {
            Stmt st2(m_db, "SELECT name,space FROM filehub_packs WHERE id=?");
            if (st2.p)
            {
                BindInt(st2.p, 1, (int64_t)targetId);
                if (sqlite3_step(st2.p) == SQLITE_ROW)
                {
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st2.p, 0));
                    targetName = n ? n : "";
                    targetSpace = -1;
                    alive = true;
                }
            }
        }
        else
        {
            Stmt st2(m_db, "SELECT name,space FROM dirs WHERE id=?");
            if (st2.p)
            {
                BindInt(st2.p, 1, (int64_t)targetId);
                if (sqlite3_step(st2.p) == SQLITE_ROW)
                {
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st2.p, 0));
                    targetName = n ? n : "";
                    targetSpace = (int)sqlite3_column_int64(st2.p, 1);
                    alive = true;
                }
            }
        }
        s["name"] = targetName;
        s["alive"] = alive;
        s["space"] = targetSpace == 0 ? "public" : "personal";
        rsp["result"]["shares"].push_back(s);
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

void FileHubModule::RemoveSharesOf(const std::string& targetType, uint64_t targetId)
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db, "DELETE FROM shares WHERE target_type=? AND target_id=?");
    if (!st.p)
        return;
    BindText(st.p, 1, targetType);
    BindInt(st.p, 2, (int64_t)targetId);
    sqlite3_step(st.p);
}

// ============================================================================
// 文件中心管理(手动一致性同步)
// ============================================================================

void FileHubModule::HandleAdminSync(ZmReqLoop* loop)
{
    // 长任务调度:移入 PostToLoop 续体执行(与 zip 流式同模式),先取消请求死线防超时误杀。
    // 请求域访问(IsClosing/CancelDeadline/回复)收敛到 loop 线程,外部只触碰 m_db(自持锁)。
    loop->PostToLoop([this](ZmReqLoop* l) {
        if (l->IsClosing() || m_gone.load())
            return;
        l->CancelDeadline();   // 同步扫描可能远超请求超时:取消后按需等待,不再受 504 兜底干扰

        int64_t beginMs = GetTickCount64();
        VerifyStats stats;
        try
        {
            stats = VerifyOnce();
        }
        catch (const std::exception& e)
        {
            DEFAULT_LOG_ERROR("[FileHub] 手动一致性同步异常(已隔离): {}", e.what());
            ZmReqLoopRest::ResponseError(l, 500, "同步失败,请查看服务日志");
            return;
        }
        catch (...)
        {
            DEFAULT_LOG_ERROR("[FileHub] 手动一致性同步未知异常(已隔离)");
            ZmReqLoopRest::ResponseError(l, 500, "同步失败,请查看服务日志");
            return;
        }
        if (!stats.ran)
        {
            ZmReqLoopRest::ResponseError(l, 409, "已有同步正在进行,请稍后重试");
            return;
        }

        ZMJSON rsp;
        rsp["result"]["ok"] = true;
        rsp["result"]["elapsedMs"] = (int64_t)(GetTickCount64() - beginMs);
        rsp["result"]["stats"]["dirsAdded"] = (int64_t)stats.dirsAdded;
        rsp["result"]["stats"]["filesAdded"] = (int64_t)stats.filesAdded;
        rsp["result"]["stats"]["dirsRemoved"] = (int64_t)stats.dirsRemoved;
        rsp["result"]["stats"]["filesRemoved"] = (int64_t)stats.filesRemoved;
        ZmReqLoopRest::ResponseJson(l, 200, rsp);
    });
}

// ============================================================================
// 后台周期维护(独立事件循环线程:一致性校验 + 分享下载日志清理 + 传输任务清理)
// ============================================================================

void FileHubModule::MaintainTick()
{
    // Shutdown 短路;窗口去重状态 m_lastTickDay 由事件循环线程独占。
    // 启动后首个周期即维护一次,之后每日 03:00 窗口(修复长运行期文件系统/DB 漂移,
    // 顺带清理过期分享下载日志与传输任务)
    if (m_gone.load())
        return;

    time_t now = time(nullptr);
    if (now > 0)
    {
        struct tm tmv;
        localtime_s(&tmv, &now);
        bool inWindow = (tmv.tm_hour >= kVerifyCleanHour && tmv.tm_hour < kVerifyCleanHour + 1);
        if ((m_lastTickDay == 0) || (inWindow && m_lastTickDay != tmv.tm_mday))
        {
            // 异常隔离:回调异常会击穿 event_base_loop 栈致定时器失效(替代原裸线程直接 terminate)
            try
            {
                VerifyOnce();
                CleanShareDownloadLogs();
                CleanTransferTasks();
                PackCleanup();   // 打包产物过期/孤儿清理(活跃分享引用除外)
            }
            catch (const std::exception& e)
            {
                DEFAULT_LOG_ERROR("[FileHub] 一致性校验异常(已隔离): {}", e.what());
            }
            catch (...)
            {
                DEFAULT_LOG_ERROR("[FileHub] 一致性校验未知异常(已隔离)");
            }
            m_lastTickDay = tmv.tm_mday;
        }
    }
}

VerifyStats FileHubModule::VerifyOnce()
{
    VerifyStats stats;
    // 手动同步与周期校验互斥:已在运行则跳过本轮(周期场景 m_lastTickDay 仍推进,当日不再重试;
    // 手动场景由 HandleAdminSync 以 ran=false 告知 "已有同步在进行")
    bool expect = false;
    if (!m_verifyRunning.compare_exchange_strong(expect, true))
    {
        DEFAULT_LOG_WARN("[FileHub] 一致性校验已在运行,跳过本轮");
        return stats;
    }
    // RAII 释放:异常传播(调用方 try/catch 隔离)或正常返回前复位,防标志永久占用
    struct VerifyRunningGuard
    {
        std::atomic<bool>& flag;
        ~VerifyRunningGuard() { flag.store(false); }
    } runningGuard{m_verifyRunning};

    // 等待 Open 完成(线程在 Open 尾部启动,已就绪);遍历各空间
    // 公共空间(0)+ 个人空间(以文件系统目录为准)
    std::vector<int> spaces;
    spaces.push_back(0);

    std::wstring rootPattern = ZmString::UTF8_To_Unicode(m_hubRoot) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(rootPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string name = ZmString::Unicode_To_UTF8(fd.cFileName);
            if (name == "." || name == "..")
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                char* end = nullptr;
                long long v = strtoll(name.c_str(), &end, 10);
                if (end && *end == 0 && v > 0 && v <= 0x7FFFFFFF)
                    spaces.push_back((int)v);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    for (int space : spaces)
    {
        if (m_gone.load())
        {
            stats.ran = true;
            return stats;
        }
        std::string spaceAbs = m_hubRoot + "\\" + std::to_string(space);
        VerifySpaceTree(space, spaceAbs, stats);
    }
    DEFAULT_LOG_INFO("[FileHub] 一致性校验完成");
    stats.ran = true;
    return stats;
}

void FileHubModule::CleanShareDownloadLogs()
{
    std::lock_guard<std::mutex> lk(m_db.Mutex());
    Stmt st(m_db, "DELETE FROM share_download_logs WHERE create_time<?");
    if (st.p)
    {
        BindInt(st.p, 1, (int64_t)(time(nullptr) - kShareDownloadLogRetain));
        sqlite3_step(st.p);
        if (m_db.Changes() > 0)
            DEFAULT_LOG_INFO("[FileHub] 清理分享下载日志 {} 条", m_db.Changes());
    }
}

void FileHubModule::CleanTransferTasks()
{
    int64_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        // 1) 历史保留:终态超 30 天删行
        Stmt stDel(m_db,
            "DELETE FROM transfer_tasks WHERE status IN ('done','failed') AND update_time<?");
        if (stDel.p)
        {
            BindInt(stDel.p, 1, (int64_t)(now - kTaskRetainDays));
            sqlite3_step(stDel.p);
            if (m_db.Changes() > 0)
                DEFAULT_LOG_INFO("[FileHub] 清理传输任务历史 {} 条", m_db.Changes());
        }
        // 2) 兜底:非终态超 30 分钟强制标 failed(客户端中断;正常路径由 HandleTasks 90s 标记)
        Stmt stStuck(m_db, "UPDATE transfer_tasks SET status=?, err='客户端中断', update_time=? "
                           "WHERE status IN ('uploading','packing','triggered') AND update_time<?");
        if (stStuck.p)
        {
            BindText(stStuck.p, 1, kTaskStatusFailed);
            BindInt(stStuck.p, 2, now);
            BindInt(stStuck.p, 3, (int64_t)(now - kTaskStaleForceSec));
            sqlite3_step(stStuck.p);
            if (m_db.Changes() > 0)
                DEFAULT_LOG_INFO("[FileHub] 清理超时传输任务 {} 个", m_db.Changes());
        }
    }
}

void FileHubModule::VerifySpaceTree(int space, const std::string& spaceAbs, VerifyStats& stats)
{
    // DFS 物理目录树,与 DB 比对
    struct Node
    {
        uint64_t dbDirId;      // 0 = 未匹配
        std::string abs;
    };
    std::vector<Node> stack;
    stack.push_back({0, spaceAbs});

    while (!stack.empty() && !m_gone.load())
    {
        Node node = stack.back();
        stack.pop_back();

        std::wstring pattern = ZmString::UTF8_To_Unicode(node.abs) + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;

        std::vector<std::string> dirNames, fileNames;
        std::vector<int64_t> fileSizes;
        do
        {
            std::string name = ZmString::Unicode_To_UTF8(fd.cFileName);
            if (name == "." || name == "..")
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                dirNames.push_back(name);
            else
            {
                fileNames.push_back(name);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart = fd.nFileSizeLow;
                fileSizes.push_back((int64_t)sz.QuadPart);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);

        // 目录:物理有而 DB 无 → 补建(递归入栈继续);DB 有而物理无 → 删行
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        std::set<std::string> dbDirNames, dbFileNames;
        {
            Stmt st(m_db, "SELECT name FROM dirs WHERE space=? AND parent_id=? AND name != ''");
            if (st.p)   // 排除根行(name=''),防被当孤儿删除
            {
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)node.dbDirId);
                while (sqlite3_step(st.p) == SQLITE_ROW)
                {
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                    if (n)
                        dbDirNames.insert(n);
                }
            }
        }
        for (const auto& nm : dirNames)
        {
            if (dbDirNames.count(nm))
                continue;
            // 补建 dirs 行(OR IGNORE:与请求并发时行已由对方创建 → 幂等跳过,防撞 UNIQUE 误报)
            Stmt ins(m_db,
                "INSERT OR IGNORE INTO dirs(space,parent_id,name,create_by,create_time) VALUES(?,?,?,0,0)");
            if (ins.p)
            {
                BindInt(ins.p, 1, (int64_t)space);
                BindInt(ins.p, 2, (int64_t)node.dbDirId);
                BindText(ins.p, 3, nm);
                sqlite3_step(ins.p);
                if (m_db.Changes() > 0)
                {
                    stats.dirsAdded++;
                    DEFAULT_LOG_INFO("[FileHub] 校验补建目录: space={} {}", space, nm);
                }
            }
        }
        // 删 DB 有而物理无的目录行(级联清其子树:物理目录整体丢失 → 子树 dirs/files 行全为孤儿;
        // 原实现只删单行,遗留 files.dir_id 指向已删目录的幽灵文件)
        for (const auto& dbName : dbDirNames)
        {
            if (std::find(dirNames.begin(), dirNames.end(), dbName) == dirNames.end())
            {
                uint64_t missDirId = 0;
                {
                    Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
                    if (st.p)
                    {
                        BindInt(st.p, 1, (int64_t)space);
                        BindInt(st.p, 2, (int64_t)node.dbDirId);
                        BindText(st.p, 3, dbName);
                        if (sqlite3_step(st.p) == SQLITE_ROW)
                            missDirId = (uint64_t)sqlite3_column_int64(st.p, 0);
                    }
                }
                if (missDirId != 0)
                {
                    std::vector<uint64_t> subs;
                    CollectSubtreeDirsLocked(space, missDirId, subs);
                    if (!subs.empty())
                    {
                        std::string ph;
                        for (size_t i = 0; i < subs.size(); ++i)
                            ph += i ? ",?" : "?";
                        {
                            Stmt del(m_db, ("DELETE FROM files WHERE dir_id IN (" + ph + ")").c_str());
                            if (del.p)
                            {
                                for (size_t i = 0; i < subs.size(); ++i)
                                    BindInt(del.p, (int)i + 1, (int64_t)subs[i]);
                                sqlite3_step(del.p);
                            }
                        }
                        {
                            Stmt del(m_db, ("DELETE FROM dirs WHERE id IN (" + ph + ")").c_str());
                            if (del.p)
                            {
                                for (size_t i = 0; i < subs.size(); ++i)
                                    BindInt(del.p, (int)i + 1, (int64_t)subs[i]);
                                sqlite3_step(del.p);
                            }
                        }
                        stats.dirsRemoved++;
                        DEFAULT_LOG_INFO("[FileHub] 校验删除孤儿目录: space={} {} (级联子树 {} 个目录)",
                                         space, dbName, subs.size());
                    }
                }
            }
        }
        // 子目录递归:拿到新/旧 dir id
        for (const auto& nm : dirNames)
        {
            uint64_t childDbId = 0;
            Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? AND name=?");
            if (st.p)
            {
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)node.dbDirId);
                BindText(st.p, 3, nm);
                if (sqlite3_step(st.p) == SQLITE_ROW)
                    childDbId = (uint64_t)sqlite3_column_int64(st.p, 0);
            }
            if (childDbId != 0)
                stack.push_back({childDbId, node.abs + "\\" + nm});
        }

        // 文件:物理有而 DB 无 → 补建(uploader=0,显示 —);DB 有而物理无 → 删行
        {
            Stmt st(m_db, "SELECT name,size FROM files WHERE space=? AND dir_id=?");
            if (st.p)
            {
                BindInt(st.p, 1, (int64_t)space);
                BindInt(st.p, 2, (int64_t)node.dbDirId);
                while (sqlite3_step(st.p) == SQLITE_ROW)
                {
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
                    if (n)
                        dbFileNames.insert(n);
                }
            }
        }
        for (size_t i = 0; i < fileNames.size(); ++i)
        {
            if (dbFileNames.count(fileNames[i]))
                continue;
            Stmt ins(m_db,
                "INSERT OR IGNORE INTO files(space,dir_id,name,size,mtime,uploader_id,upload_time) "
                "VALUES(?,?,?,?,?,0,0)");
            if (ins.p)
            {
                BindInt(ins.p, 1, (int64_t)space);
                BindInt(ins.p, 2, (int64_t)node.dbDirId);
                BindText(ins.p, 3, fileNames[i]);
                BindInt(ins.p, 4, fileSizes[i]);
                BindInt(ins.p, 5, (int64_t)time(nullptr));
                sqlite3_step(ins.p);
                if (m_db.Changes() > 0)
                {
                    stats.filesAdded++;
                    DEFAULT_LOG_INFO("[FileHub] 校验补建文件: space={} {}", space, fileNames[i]);
                }
            }
        }
        for (const auto& dbName : dbFileNames)
        {
            if (std::find(fileNames.begin(), fileNames.end(), dbName) == fileNames.end())
            {
                Stmt del(m_db, "DELETE FROM files WHERE space=? AND dir_id=? AND name=?");
                if (del.p)
                {
                    BindInt(del.p, 1, (int64_t)space);
                    BindInt(del.p, 2, (int64_t)node.dbDirId);
                    BindText(del.p, 3, dbName);
                    sqlite3_step(del.p);
                    if (m_db.Changes() > 0)
                    {
                        stats.filesRemoved++;
                        DEFAULT_LOG_INFO("[FileHub] 校验删除孤儿文件: space={} {}", space, dbName);
                    }
                }
            }
        }
    }
}
