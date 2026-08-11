#include "module_file_hub.h"

#include "module_user.h"
#include "service_define.h"
#include "http_server_manager.h"

#include "zm_net_req_loop_protocol.h"   // ZmReqLoopRest
#include "zm_logger.h"
#include "zm_util_str.h"
#include "zm_util_sys.h"
#include "zm_util_zlib.h"

#include <sqlite3.h>
#include <openssl/rand.h>
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

    m_openOk.store(true);
    DEFAULT_LOG_INFO("[FileHub] 文件中心初始化完成,仓库 {}", m_hubRoot);

    // 启动一致性校验线程(启动即执行一次)
    m_verifyThread = std::thread(&FileHubModule::VerifyLoop, this);
    return true;
}

void FileHubModule::Shutdown()
{
    m_gone.store(true);
    if (m_verifyThread.joinable())
    {
        m_verifyThread.join();
        m_verifyThread = std::thread();
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

void FileHubModule::RegisterHttpRoutes(HttpServerManager* httpMgr)
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

    // share/unshare 目标自带空间,不要求 space 参数(仅需登录)
    if (path == "/portal/filehub/share" || path == "/portal/filehub/unshare")
    {
        std::string role, account;
        int space = -1;
        uint64_t uid = AuthAndSpace(task, "public", space, role, account);
        if (!uid) { ZmReqLoopRest::ResponseError(loop, 401, "会话已失效"); return true; }
        if (path == "/portal/filehub/share")
            HandleShareCreate(loop, req, uid, account);
        else
            HandleShareCancel(loop, req, uid, account);
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
    if (path == "/portal/filehub/zip")
    {
        HandleZip(loop, task, req, space, uid, account);
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

    uint64_t dirId = (uint64_t)strtoull(task->GetQueryValue("dir_id", "0"), nullptr, 10);
    const char* name = task->GetQueryValue("name", "");
    if (!IsValidName(name ? name : ""))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "文件名不合法");
        return;
    }
    if (!DirExists(space, dirId))
    {
        ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
        return;
    }
    // 同名冲突 + 写盘 + 落库整体持锁:并发同名上传由该锁串行化,冲突检查即最终裁决,
    // 后到者 409 且不触碰先到者的物理文件。原分锁实现存在窗口:两请求都通过检查 → 互覆写
    // 同一路径 → INSERT 撞 UNIQUE 回滚误删对方文件,造成数据丢失。
    std::string absPath = EntryAbsPath(space, dirId, name);   // 路径解析自持锁,须在整体锁外
    uint64_t size = 0;
    uint64_t newId = 0;
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
            ZmReqLoopRest::ResponseError(loop, 409, "同名文件已存在");
            return;
        }

        // 写盘(锁内:冲突检查与落库之间不允许其他请求插入)
        if (ReceiveFileStream(task, absPath, size) != 0)
        {
            DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
            ZmReqLoopRest::ResponseError(loop, 500, "上传失败");
            return;
        }

        // X-File-Size 声明校验(有则比对,不符删半成品)
        const char* declared = task->GetRequestHeader("X-File-Size");
        if (declared && declared[0])
        {
            uint64_t expected = (uint64_t)strtoull(declared, nullptr, 10);
            if (expected != size)
            {
                DeleteFileW(ZmString::UTF8_To_Unicode(absPath).c_str());
                ZmReqLoopRest::ResponseError(loop, 400, "文件大小不一致");
                return;
            }
        }
        // 允许 0 字节文件上传(空配置文件等合法场景;ReceiveFileStream 对空 body 创建空文件)

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
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
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
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        newId = (uint64_t)m_db.LastInsertRowId();
    }

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

int FileHubModule::ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
                                      const std::string& rangeStr, int64_t fileSize)
{
    if (rangeStr.size() < 7 || _strnicmp(rangeStr.c_str(), "bytes=", 6) != 0)
        return -1;

    std::string rangeVal = rangeStr.substr(6);
    if (rangeVal.find(',') != std::string::npos) return -1;

    size_t dashPos = rangeVal.find('-');
    if (dashPos == std::string::npos) return -1;

    // stoll 无防护(非法/溢出输入抛异常穿透请求线程):改 strtoll + 严格校验,非法回 416
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
    auto badRange = [&]() -> int {
        task->SetReply(ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE, "Range Not Satisfiable");
        task->PutReplyHeader("Content-Range", ("bytes */" + std::to_string(fileSize)).c_str());
        return ZM_HTTP_STATUS_CODE_RANGE_NOT_SATISFIABLE;
    };

    int64_t start = 0, end = fileSize - 1;
    std::string startStr = rangeVal.substr(0, dashPos);
    std::string endStr = rangeVal.substr(dashPos + 1);

    if (startStr.empty() && !endStr.empty()) {
        int64_t suffixLen = 0;
        if (!parseNum(endStr, &suffixLen))
            return badRange();
        if (suffixLen >= fileSize) start = 0;
        else start = fileSize - suffixLen;
        end = fileSize - 1;
    } else if (!startStr.empty() && endStr.empty()) {
        if (!parseNum(startStr, &start))
            return badRange();
        end = fileSize - 1;
    } else {
        if (!parseNum(startStr, &start) || !parseNum(endStr, &end))
            return badRange();
    }

    if (start < 0 || end >= fileSize || start > end)
        return badRange();

    int64_t rangeLength = end - start + 1;
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;

    task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(path));
    task->PutReplyHeader("Content-Disposition",
        ("attachment; filename=\"" + ExtractFilename(path) + "\"").c_str());
    task->PutReplyHeader("Accept-Ranges", "bytes");
    task->PutReplyHeader("Content-Range",
        ("bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize)).c_str());
    task->SetReply(ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT, "Partial Content");

    if (task->SetReplyFile(fd, start, rangeLength) != 0) { _close(fd); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }
    return ZM_HTTP_STATUS_CODE_PARTIAL_CONTENT;
}

int FileHubModule::SendFile(ZmHttpdTask* task, const std::string& physicalPath)
{
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return ZM_HTTP_STATUS_CODE_NOT_FOUND;

    int64_t fileSize = _filelengthi64(fd);
    if (fileSize < 0) { _close(fd); return ZM_HTTP_STATUS_CODE_NOT_FOUND; }

    // 0 字节文件:允许下载,返回 200 空响应(不再视为 404)
    if (fileSize == 0)
    {
        _close(fd);
        task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(physicalPath));
        task->PutReplyHeader("Content-Disposition",
            ("attachment; filename=\"" + ExtractFilename(physicalPath) + "\"").c_str());
        task->SetReply(ZM_HTTP_STATUS_CODE_OK);
        return ZM_HTTP_STATUS_CODE_OK;   // 调用方负责 TriggerReply
    }

    const char* rangeHeader = task->GetRequestHeader("Range");
    if (rangeHeader && rangeHeader[0]) {
        _close(fd);
        int rangeResult = ServeFileWithRange(task, physicalPath, rangeHeader, fileSize);
        if (rangeResult > 0) return rangeResult;
        if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(physicalPath).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
            return ZM_HTTP_STATUS_CODE_NOT_FOUND;
    }

    task->PutReplyHeader("Content-type", ZmHttpUtil::GetMimeType(physicalPath));
    task->PutReplyHeader("Content-Disposition",
        ("attachment; filename=\"" + ExtractFilename(physicalPath) + "\"").c_str());
    task->PutReplyHeader("Accept-Ranges", "bytes");
    task->SetReply(200);

    if (task->SetReplyFile(fd, 0, fileSize) != 0) { _close(fd); return ZM_HTTP_STATUS_CODE_INTERNAL_ERROR; }
    return ZM_HTTP_STATUS_CODE_OK;
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
    int space = -1;
    uint64_t dirId = 0;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT space,dir_id,name FROM files WHERE id=?");
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
    }
    // 权限:公共空间任何登录用户;个人空间仅本人
    if (space != 0 && opUid != (uint64_t)space)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可下载");
        return;
    }
    std::string absPath = EntryAbsPath(space, dirId, name);
    int rc = SendFile(task, absPath);
    if (rc == ZM_HTTP_STATUS_CODE_NOT_FOUND)
    {
        ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
        return;
    }
    task->TriggerReply();   // RESTful 异步路径:SetReply 后须显式触发,否则 deadline 504
    Log(opUid, opAccount, "download", "file", fileId, "{\"name\":\"" + name + "\"}");
}

// ============================================================================
// zip 打包下载
// ============================================================================

bool FileHubModule::ZipAddFile(ZipWriter& zip, int space, uint64_t fileId,
                               const std::string& entryPrefix,
                               const std::set<uint64_t>& skipFileIds)
{
    if (skipFileIds.count(fileId))
        return true;   // 已被文件夹展开覆盖,跳过(文件优先规则下不应发生,防御)

    int fSpace = -1;
    uint64_t dirId = 0;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        Stmt st(m_db, "SELECT space,dir_id,name FROM files WHERE id=?");
        if (!st.p)
            return false;
        BindInt(st.p, 1, (int64_t)fileId);
        if (sqlite3_step(st.p) != SQLITE_ROW)
            return false;
        fSpace = (int)sqlite3_column_int64(st.p, 0);
        dirId = (uint64_t)sqlite3_column_int64(st.p, 1);
        const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
        name = n ? n : "";
    }
    if (name.empty())
        return true;   // 空名跳过(防御)
    if (fSpace != space)
        return false;   // 跨空间条目拒绝(不应发生,防御)

    std::string entryName = entryPrefix.empty() ? name : entryPrefix + "/" + name;

    std::string absPath = EntryAbsPath(fSpace, dirId, name);
    int fd = -1;
    if (_wsopen_s(&fd, ZmString::UTF8_To_Unicode(absPath).c_str(),
                  _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd == -1)
        return false;

    if (!zip.BeginEntry(entryName, false))
    {
        _close(fd);
        return false;
    }
    std::vector<unsigned char> buf(64 * 1024);   // 堆分配:防栈溢出
    bool ok = true;
    for (;;)
    {
        int n = (int)_read(fd, buf.data(), (unsigned int)buf.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!zip.Write(buf.data(), (size_t)n)) { ok = false; break; }
    }
    _close(fd);
    if (!zip.EndEntry())
        ok = false;
    return ok;
}

bool FileHubModule::ZipAddDirRecursive(ZipWriter& zip, int space, uint64_t dirId,
                                       const std::string& entryPrefix,
                                       const std::set<uint64_t>& skipFileIds)
{
    std::string dirName;
    std::vector<uint64_t> childDirs;
    std::vector<uint64_t> childFiles;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        // 目录名
        if (dirId != 0)
        {
            Stmt st(m_db, "SELECT name FROM dirs WHERE id=? AND space=?");
            if (!st.p)
                return false;
            BindInt(st.p, 1, (int64_t)dirId);
            BindInt(st.p, 2, (int64_t)space);
            if (sqlite3_step(st.p) != SQLITE_ROW)
                return false;
            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
            dirName = n ? n : "";
        }
        Stmt st(m_db, "SELECT id FROM dirs WHERE space=? AND parent_id=? ORDER BY name");
        if (!st.p)
            return false;
        BindInt(st.p, 1, (int64_t)space);
        BindInt(st.p, 2, (int64_t)dirId);
        while (sqlite3_step(st.p) == SQLITE_ROW)
            childDirs.push_back((uint64_t)sqlite3_column_int64(st.p, 0));
        Stmt st2(m_db, "SELECT id FROM files WHERE space=? AND dir_id=? ORDER BY name");
        if (!st2.p)
            return false;
        BindInt(st2.p, 1, (int64_t)space);
        BindInt(st2.p, 2, (int64_t)dirId);
        while (sqlite3_step(st2.p) == SQLITE_ROW)
            childFiles.push_back((uint64_t)sqlite3_column_int64(st2.p, 0));
    }

    std::string prefix = entryPrefix;
    if (!dirName.empty())
        prefix = prefix.empty() ? dirName : prefix + "/" + dirName;
    std::string dirEntry = prefix.empty() ? "" : prefix + "/";

    // 空目录写目录条目
    if (childDirs.empty() && childFiles.empty() && !dirEntry.empty())
    {
        if (!zip.BeginEntry(dirEntry, true) || !zip.EndEntry())
            return false;
        return true;
    }

    for (uint64_t fid : childFiles)
    {
        if (!ZipAddFile(zip, space, fid, prefix, skipFileIds))
            return false;
    }
    for (uint64_t cid : childDirs)
    {
        if (!ZipAddDirRecursive(zip, space, cid, prefix, skipFileIds))
            return false;
    }
    return true;
}

void FileHubModule::HandleZip(ZmReqLoop* loop, ZmHttpdTask* task, const ZMJSON& body, int space,
                              uint64_t opUid, const std::string& opAccount)
{
    // ★ 立即取消 deadline:打包可能在 loop 队列中排队(前序打包未完成),
    //   若等到 ResponseStreamStart 的 lambda 才取消,排队期间 5s deadline
    //   到期会触发 TIMEOUT → onTimeout 504 收尾 → 流式被掐断,前端 fetch 挂起
    loop->CancelDeadline();

    std::vector<std::pair<std::string, uint64_t>> ids;
    if (!ParseIdList(body, ids))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    // 校验条目空间一致 + 存在性
    for (auto& it : ids)
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
            if ((int)sqlite3_column_int64(st.p, 0) != space)
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
            if ((int)sqlite3_column_int64(st.p, 0) != space)
            {
                ZmReqLoopRest::ResponseError(loop, 403, "空间不匹配");
                return;
            }
        }
    }

    // 先响应头,再遍历打包(目录不存在等错误在流开始前返回)
    // zip 文件名:单目录 = 目录名.zip;其余 = filehub-打包.zip
    std::string zipName = "filehub-打包.zip";
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

    // 流式回复:Start 与打包均 PostToLoop(队列顺序保证 Start 先执行;
    // 直接同步 SendReplyChunk 会早于 StartStreamReply,chunk 丢失/挂死)
    // 文件名:ASCII 兜底 + RFC 5987 UTF-8 编码(中文名浏览器才能正确识别)
    std::string zipDisp = "attachment; filename=\"filehub.zip\"; filename*=UTF-8''" +
                          PercentEncode(zipName);
    ZmReqLoopRest::ResponseStreamStart(loop, 200,
        {{"Content-Type", "application/zip"},
         {"Content-Disposition", zipDisp.c_str()}});

    loop->PostToLoop([this, task, space, ids, zipName, opUid, opAccount](ZmReqLoop* l) {
        if (l->IsClosing())
            return;
        try
        {
            ZipWriter zip;
            std::vector<unsigned char> chunk;
            std::set<uint64_t> skipFileIds;
            bool ok = true;
            for (auto& it : ids)
            {
                if (task->IsConnClosed())
                    break;   // 连接已关闭:停止打包发送(防 doer 生命周期竞态)
                if (it.first == "file")
                {
                    if (!ZipAddFile(zip, space, it.second, "", skipFileIds)) { ok = false; break; }
                }
                else
                {
                    if (!ZipAddDirRecursive(zip, space, it.second, "", skipFileIds)) { ok = false; break; }
                }
                zip.Drain(chunk);
                if (!chunk.empty())
                    task->SendReplyChunk(chunk.data(), chunk.size());
            }
            if (ok)
            {
                ok = zip.Finish();
                zip.Drain(chunk);
                if (ok && !chunk.empty())
                    task->SendReplyChunk(chunk.data(), chunk.size());
            }
            // ★ 无条件结束流:即使连接已关闭,EndStreamReply 也安全(驱动 doer 回收,
            //   保证前端 fetch 收到流结束)。若跳过,流永不结束 → 前端永久挂起卡住
            task->EndStreamReply();
            (void)ok;
            Log(opUid, opAccount, "download", "zip", 0,
                "{\"count\":" + std::to_string(ids.size()) + "}");
        }
        catch (...)
        {
            // 打包异常兜底:无条件结束流,防异常进入 loop 收尾与流式冲突
            DEFAULT_LOG_ERROR("[FileHub] zip 打包异常");
            task->EndStreamReply();
        }
    });
}

// ============================================================================
// 分享
// ============================================================================

void FileHubModule::HandleShareCreate(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                                      const std::string& opAccount)
{
    std::string type = zm_json_get_str(body, "type");
    uint64_t id = (uint64_t)JsonInt(body, "id", 0);
    if ((type != "file" && type != "dir") || id == 0)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }

    int space = -1;
    uint64_t uploaderId = 0;
    {
        std::lock_guard<std::mutex> lk(m_db.Mutex());
        if (type == "file")
        {
            Stmt st(m_db, "SELECT space,uploader_id FROM files WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "文件不存在");
                return;
            }
            space = (int)sqlite3_column_int64(st.p, 0);
            uploaderId = (uint64_t)sqlite3_column_int64(st.p, 1);
        }
        else
        {
            Stmt st(m_db, "SELECT space,create_by FROM dirs WHERE id=?");
            if (!st.p) { ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误"); return; }
            BindInt(st.p, 1, (int64_t)id);
            if (sqlite3_step(st.p) != SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 404, "目录不存在");
                return;
            }
            space = (int)sqlite3_column_int64(st.p, 0);
            uploaderId = (uint64_t)sqlite3_column_int64(st.p, 1);
        }
    }

    // 权限:公共文件任何登录用户可分享;个人空间仅本人
    if (space != 0 && opUid != (uint64_t)space)
    {
        ZmReqLoopRest::ResponseError(loop, 403, "个人空间仅本人可分享");
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
        BindText(st.p, 4, type);
        BindInt(st.p, 5, (int64_t)id);
        BindInt(st.p, 6, (int64_t)time(nullptr));
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
    }

    Log(opUid, opAccount, "share", type, id, "{\"token\":\"" + token + "\"}");
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
        if (SendFile(task, absPath) == ZM_HTTP_STATUS_CODE_NOT_FOUND)
        {
            ZmReqLoopRest::ResponseError(loop, 404, "分享已失效");
            return;
        }
        task->TriggerReply();   // RESTful 异步路径:SetReply 后须显式触发
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

    // 目录分享:zip 打包(顶层保留该文件夹名;打包移入 PostToLoop,保证 Start 先执行)
    if (targetType == "dir")
    {
        loop->CancelDeadline();   // 同 HandleZip:排队期间防 5s deadline 掐断流式
        std::string shareZipName = name + ".zip";
        std::string shareDisp = "attachment; filename=\"filehub.zip\"; filename*=UTF-8''" +
                                PercentEncode(shareZipName);
        ZmReqLoopRest::ResponseStreamStart(loop, 200,
            {{"Content-Type", "application/zip"},
             {"Content-Disposition", shareDisp.c_str()}});
        loop->PostToLoop([this, task, space, targetId, name, shareId, downloaderId,
                          downloaderAccount](ZmReqLoop* l) {
            if (l->IsClosing())
                return;
            ZipWriter zip;
            std::vector<unsigned char> chunk;
            std::set<uint64_t> skip;
            bool ok = ZipAddDirRecursive(zip, space, targetId, "", skip);
            if (ok) ok = zip.Finish();
            zip.Drain(chunk);
            if (ok && !chunk.empty())
                task->SendReplyChunk(chunk.data(), chunk.size());
            task->EndStreamReply();
            Log(downloaderId, downloaderAccount, "download_share", "dir", targetId,
                "{\"share_id\":" + std::to_string(shareId) + "}");
        });
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
// 启动一致性校验(独立线程:以文件系统为准重建/删行,记 restore 日志)
// ============================================================================

void FileHubModule::VerifyLoop()
{
    // 启动即校验一次,之后每日 03:00 窗口校验(修复长运行期文件系统/DB 漂移),
    // 顺带清理过期分享下载日志;轮询 60s,可被 Shutdown 打断
    int lastVerifyDay = 0;
    while (!m_gone.load())
    {
        time_t now = time(nullptr);
        if (now > 0)
        {
            struct tm tmv;
            localtime_s(&tmv, &now);
            bool inWindow = (tmv.tm_hour >= kVerifyCleanHour && tmv.tm_hour < kVerifyCleanHour + 1);
            if ((lastVerifyDay == 0) || (inWindow && lastVerifyDay != tmv.tm_mday))
            {
                VerifyOnce();
                CleanShareDownloadLogs();
                lastVerifyDay = tmv.tm_mday;
            }
        }
        for (int i = 0; i < 60 && !m_gone.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void FileHubModule::VerifyOnce()
{
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
            return;
        std::string spaceAbs = m_hubRoot + "\\" + std::to_string(space);
        VerifySpaceTree(space, spaceAbs);
    }
    DEFAULT_LOG_INFO("[FileHub] 一致性校验完成");
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

void FileHubModule::VerifySpaceTree(int space, const std::string& spaceAbs)
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
                    DEFAULT_LOG_INFO("[FileHub] 校验补建目录: space={} {}", space, nm);
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
                    DEFAULT_LOG_INFO("[FileHub] 校验补建文件: space={} {}", space, fileNames[i]);
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
                    DEFAULT_LOG_INFO("[FileHub] 校验删除孤儿文件: space={} {}", space, dbName);
                }
            }
        }
    }
}
