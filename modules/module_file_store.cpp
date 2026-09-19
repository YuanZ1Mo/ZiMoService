#include "modules/module_file_store.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "modules/module_db.h"
#include "modules/module_file_db.h"
#include "modules/module_file_defs.h"

#include "zm_net_http_server.h"
#include "zm_util_logger.h"

#include <algorithm>
#include <cstdio>

using namespace drogon;

namespace
{
/// 复制过程中的临时文件后缀(同目录原子替换用;一致性同步按它跳过残留文件)
constexpr const char* kCopyTempSuffix = ".zmtmp";

/// FILETIME → unix 秒(1601-01-01 到 1970-01-01 的 100ns 数)
int64_t FileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.LowPart                       = ft.dwLowDateTime;
    v.HighPart                      = ft.dwHighDateTime;
    const unsigned long long kEpoch = 116444736000000000ULL;
    if (v.QuadPart < kEpoch)
        return 0;
    return static_cast<int64_t>((v.QuadPart - kEpoch) / 10000000ULL);
}

/**
 * @brief UTF-8 → UTF-16
 *
 * 库内名称一律 UTF-8,Windows API 一律宽字符,转换集中在此处。
 *
 * @param s UTF-8 串
 * @return 宽字符串(转换失败返回空)
 */
std::wstring Utf8ToWide(const std::string& s)
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

/**
 * @brief UTF-16 → UTF-8
 *
 * @param w 宽字符串
 * @return UTF-8 串(转换失败返回空)
 */
std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        nullptr);
    return s;
}

/// 去掉路径尾部的分隔符(根目录 "C:\" 形态除外)
std::string TrimTrailingSlash(const std::string& p)
{
    if (p.size() <= 1)
        return p;
    if (p.back() != '\\' && p.back() != '/')
        return p;
    // "C:\" 形态保留
    if (p.size() == 3 && p[1] == ':')
        return p;
    return p.substr(0, p.size() - 1);
}
} // namespace

// ============================================================================
// 构造 / 析构
// ============================================================================
ZmFileStoreModule::ZmFileStoreModule(ZmFileDbModule* db, const std::string& rootDir)
    : m_db(db), m_rootDir(TrimTrailingSlash(rootDir))
{
}

ZmFileStoreModule::~ZmFileStoreModule() = default;

// ============================================================================
// 目录位置
// ============================================================================
std::string ZmFileStoreModule::SpaceRootOf(const std::string& rootDir, int64_t space)
{
    return TrimTrailingSlash(rootDir) + "\\space\\" + std::to_string(space);
}

std::string ZmFileStoreModule::SpaceRoot(int64_t space) const
{
    return SpaceRootOf(m_rootDir, space);
}

std::string ZmFileStoreModule::CacheRoot(int64_t space) const
{
    return m_rootDir + "\\space_cache\\" + std::to_string(space);
}

std::string ZmFileStoreModule::ZipDir(int64_t space) const
{
    return CacheRoot(space) + "\\zip";
}

std::string ZmFileStoreModule::ChunkDir(int64_t space, const std::string& uploadId) const
{
    return CacheRoot(space) + "\\chunk\\" + uploadId;
}

std::string ZmFileStoreModule::TrashRoot(int64_t space) const
{
    return m_rootDir + "\\space_trash\\" + std::to_string(space);
}

std::string ZmFileStoreModule::TrashEntryDir(int64_t space, int64_t nodeId) const
{
    return TrashRoot(space) + "\\" + std::to_string(nodeId);
}

std::string ZmFileStoreModule::TrashEntryPath(int64_t space, int64_t nodeId,
                                              const std::string& name) const
{
    return TrashEntryDir(space, nodeId) + "\\" + name;
}

// ============================================================================
// 路径组装
// ============================================================================
ZmStoreResult ZmFileStoreModule::PhysicalPathSync(int64_t nodeId, int64_t& space,
                                                  std::string& out)
{
    if (!m_db)
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "数据模块不可用");
    if (nodeId == 0)
    {
        // 空间根:space 由调用方给出
        if (space < 0)
            return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::NotFound), "空间不存在");
        out = SpaceRoot(space);
        return ZmStoreResult::Good();
    }
    if (!m_db->PathPartsSync(nodeId, space, out))
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::NotFound), "条目不存在");
    out = SpaceRoot(space) + "\\" + out;
    return ZmStoreResult::Good();
}

drogon::Task<ZMJSON> ZmFileStoreModule::PhysicalPath(int64_t nodeId)
{
    co_return co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [this, nodeId]() -> ZMJSON
        {
            ZMJSON        r     = ZMJSON::object();
            int64_t       space = 0;
            std::string   path;
            ZmStoreResult res = PhysicalPathSync(nodeId, space, path);
            r["ok"]           = res.ok;
            r["code"]         = res.code;
            r["message"]      = res.message;
            r["path"]         = path;
            r["space"]        = space;
            return r;
        });
}

std::string ZmFileStoreModule::ToExtended(const std::string& path)
{
    if (path.rfind("\\\\?\\", 0) == 0)
        return path;
    if (path.size() >= 2 && path[1] == ':')
        return "\\\\?\\" + path;
    // UNC 形态 \\server\share → \\?\UNC\server\share
    if (path.rfind("\\\\", 0) == 0)
        return "\\\\?\\UNC\\" + path.substr(2);
    return path;
}

bool ZmFileStoreModule::ValidateLength(int64_t space, const std::string& relPath,
                                       std::string& message) const
{
    // 预留临时后缀与 \\?\ 前缀的余量:条目本体路径 ≤ 240 字符
    std::string full = SpaceRoot(space);
    if (!relPath.empty())
        full += "\\" + relPath;
    if (static_cast<int64_t>(full.size()) > zm_file::kMaxPathChars)
    {
        message = "路径过长(超过 240 字符),请缩短名称或换个位置";
        return false;
    }
    return true;
}

// ============================================================================
// 物理操作
// ============================================================================
int ZmFileStoreModule::MapWin32Error(unsigned long err)
{
    switch (err)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return static_cast<int>(ZmErrCode::NotFound);
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return static_cast<int>(ZmErrCode::Locked);
    case ERROR_ACCESS_DENIED:
        return static_cast<int>(ZmErrCode::AccessDenied);
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return static_cast<int>(ZmErrCode::DiskFull);
    default:
        return static_cast<int>(ZmErrCode::IoError);
    }
}

ZmStoreResult ZmFileStoreModule::EnsureDir(const std::string& path)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    // CreateDirectoryW 只建最末一级:逐级补齐中间目录(space_cache\<space>\chunk\<id> 这类)
    size_t start = 4; // 跳过 "\\?\"
    if (w.size() > 8 && w.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        start = 8;
    for (size_t i = start; i < w.size(); ++i)
    {
        if (w[i] != L'\\')
            continue;
        std::wstring sub = w.substr(0, i);
        if (sub.empty() || sub.back() == L':')
            continue; // 盘符本身不建
        if (!CreateDirectoryW(sub.c_str(), nullptr))
        {
            unsigned long err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
                return ZmStoreResult::Fail(MapWin32Error(err),
                                           "创建目录失败(错误码 " + std::to_string(err) + ")");
        }
    }
    if (CreateDirectoryW(w.c_str(), nullptr))
        return ZmStoreResult::Good();
    unsigned long err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS)
        return ZmStoreResult::Good();
    return ZmStoreResult::Fail(MapWin32Error(err),
                               "创建目录失败(错误码 " + std::to_string(err) + ")");
}

ZmStoreResult ZmFileStoreModule::AtomicPlace(const std::string& tmpPath,
                                             const std::string& finalPath)
{
    std::wstring src = Utf8ToWide(ToExtended(tmpPath));
    std::wstring dst = Utf8ToWide(ToExtended(finalPath));
    if (src.empty() || dst.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    if (MoveFileExW(src.c_str(), dst.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return ZmStoreResult::Good();
    unsigned long err = GetLastError();
    return ZmStoreResult::Fail(MapWin32Error(err),
                               "写入目标位置失败(错误码 " + std::to_string(err) + ")");
}

ZmStoreResult ZmFileStoreModule::MovePath(const std::string& src, const std::string& dst)
{
    std::wstring ws = Utf8ToWide(ToExtended(src));
    std::wstring wd = Utf8ToWide(ToExtended(dst));
    if (ws.empty() || wd.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    if (MoveFileExW(ws.c_str(), wd.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return ZmStoreResult::Good();
    unsigned long err = GetLastError();
    return ZmStoreResult::Fail(MapWin32Error(err),
                               "移动失败(错误码 " + std::to_string(err) + ")");
}

ZmStoreResult ZmFileStoreModule::CopyTreeImpl(const std::string& src, const std::string& dst)
{
    std::wstring ws = Utf8ToWide(ToExtended(src));
    std::wstring wd = Utf8ToWide(ToExtended(dst));
    if (ws.empty() || wd.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");

    DWORD attr = GetFileAttributesW(ws.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return ZmStoreResult::Fail(MapWin32Error(GetLastError()), "源文件不存在");

    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        // 写到同目录的临时名再原子替换:直接覆盖会让并发读方看到"半截内容"
        // (临时名带进程号与时间戳,避免与用户文件重名;残留文件由一致性同步跳过)
        std::wstring wt = wd + Utf8ToWide(kCopyTempSuffix + std::to_string(GetCurrentProcessId()) +
                                          "." + std::to_string(GetTickCount64()));
        if (!CopyFileW(ws.c_str(), wt.c_str(), FALSE))
        {
            unsigned long err = GetLastError();
            return ZmStoreResult::Fail(MapWin32Error(err),
                                       "复制文件失败(错误码 " + std::to_string(err) + ")");
        }
        if (!MoveFileExW(wt.c_str(), wd.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            unsigned long err = GetLastError();
            DeleteFileW(wt.c_str());
            return ZmStoreResult::Fail(MapWin32Error(err),
                                       "复制入位失败(错误码 " + std::to_string(err) + ")");
        }
        return ZmStoreResult::Good();
    }

    // 目录:先建自身再递归子项(空目录同样保留)
    if (!CreateDirectoryW(wd.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "创建目标目录失败(错误码 " + std::to_string(err) + ")");
    }

    std::vector<ZmDiskEntry> children;
    ZmStoreResult            scan = ScanDirSync(src, children);
    if (!scan.ok)
        return scan;
    for (const auto& c : children)
    {
        ZmStoreResult one = CopyTreeImpl(src + "\\" + c.name, dst + "\\" + c.name);
        if (!one.ok)
            return one;
    }
    return ZmStoreResult::Good();
}

ZmStoreResult ZmFileStoreModule::CopyTreeSync(const std::string& src, const std::string& dst)
{
    return CopyTreeImpl(src, dst);
}

bool ZmFileStoreModule::IsCopyTempName(const std::string& name)
{
    return name.find(kCopyTempSuffix) != std::string::npos;
}

ZmStoreResult ZmFileStoreModule::RemoveTreeImpl(const std::string& path)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");

    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        // 已被删除(外部因素)视为成功:目标是"最终不存在"
        return ZmStoreResult::Good();
    }
    if ((attr & FILE_ATTRIBUTE_READONLY) != 0)
        SetFileAttributesW(w.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        if (DeleteFileW(w.c_str()))
            return ZmStoreResult::Good();
        unsigned long err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            return ZmStoreResult::Good();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "删除文件失败(错误码 " + std::to_string(err) + ")");
    }

    std::vector<ZmDiskEntry> children;
    ZmStoreResult            scan = ScanDirSync(path, children);
    if (!scan.ok)
        return scan;
    for (const auto& c : children)
    {
        ZmStoreResult one = RemoveTreeImpl(path + "\\" + c.name);
        if (!one.ok)
            return one;
    }
    if (RemoveDirectoryW(w.c_str()))
        return ZmStoreResult::Good();
    unsigned long err = GetLastError();
    if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND)
        return ZmStoreResult::Good();
    return ZmStoreResult::Fail(MapWin32Error(err),
                               "删除目录失败(错误码 " + std::to_string(err) + ")");
}

ZmStoreResult ZmFileStoreModule::RemoveTreeSync(const std::string& path)
{
    return RemoveTreeImpl(path);
}

ZmStoreResult ZmFileStoreModule::RemoveFileSync(const std::string& path)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    if (DeleteFileW(w.c_str()))
        return ZmStoreResult::Good();
    unsigned long err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return ZmStoreResult::Good();
    return ZmStoreResult::Fail(MapWin32Error(err),
                               "删除文件失败(错误码 " + std::to_string(err) + ")");
}

ZmStoreResult ZmFileStoreModule::StatSync(const std::string& path, ZmDiskEntry& out)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fad))
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "读取条目信息失败(错误码 " + std::to_string(err) + ")");
    }
    out.isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.ctime = FileTimeToUnix(fad.ftCreationTime);
    out.size  = out.isDir ? 0
                          : (static_cast<int64_t>(fad.nFileSizeHigh) << 32) |
                               static_cast<int64_t>(fad.nFileSizeLow);
    out.mtime = FileTimeToUnix(fad.ftLastWriteTime);
    return ZmStoreResult::Good();
}

ZmStoreResult ZmFileStoreModule::ScanDirSync(const std::string&        path,
                                             std::vector<ZmDiskEntry>& out)
{
    out.clear();
    std::wstring w = Utf8ToWide(ToExtended(path) + "\\*");
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");

    WIN32_FIND_DATAW fd{};
    HANDLE           h = FindFirstFileW(w.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        unsigned long err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) // 空目录
            return ZmStoreResult::Good();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "读取目录失败(错误码 " + std::to_string(err) + ")");
    }
    do
    {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        ZmDiskEntry e;
        e.name  = WideToUtf8(name);
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.ctime = FileTimeToUnix(fd.ftCreationTime);
        e.size  = e.isDir ? 0
                          : (static_cast<int64_t>(fd.nFileSizeHigh) << 32) |
                               static_cast<int64_t>(fd.nFileSizeLow);
        e.mtime = FileTimeToUnix(fd.ftLastWriteTime);
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ZmStoreResult::Good();
}

ZmStoreResult ZmFileStoreModule::WriteFileSync(const std::string& path, const char* data,
                                               size_t len)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "写入文件失败(错误码 " + std::to_string(err) + ")");
    }
    size_t written = 0;
    bool   ok      = true;
    while (written < len)
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - written, 1u << 20));
        DWORD done  = 0;
        if (!WriteFile(h, data + written, chunk, &done, nullptr) || done == 0)
        {
            ok = false;
            break;
        }
        written += done;
    }
    CloseHandle(h);
    if (!ok)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "写入文件失败(错误码 " + std::to_string(err) + ")");
    }
    return ZmStoreResult::Good();
}

ZmStoreResult ZmFileStoreModule::AppendFileSync(const std::string& path, const char* data,
                                                size_t len)
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "追加写入失败(错误码 " + std::to_string(err) + ")");
    }
    size_t written = 0;
    bool   ok      = true;
    while (written < len)
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - written, 1u << 20));
        DWORD done  = 0;
        if (!WriteFile(h, data + written, chunk, &done, nullptr) || done == 0)
        {
            ok = false;
            break;
        }
        written += done;
    }
    CloseHandle(h);
    if (!ok)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "追加写入失败(错误码 " + std::to_string(err) + ")");
    }
    return ZmStoreResult::Good();
}

ZmStoreResult ZmFileStoreModule::ReadFileSync(const std::string& path, std::string& out,
                                              int64_t maxBytes)
{
    out.clear();
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "路径编码失败");
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        unsigned long err = GetLastError();
        return ZmStoreResult::Fail(MapWin32Error(err),
                                   "读取文件失败(错误码 " + std::to_string(err) + ")");
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz))
    {
        CloseHandle(h);
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "读取文件大小失败");
    }
    if (maxBytes > 0 && sz.QuadPart > maxBytes)
    {
        CloseHandle(h);
        return ZmStoreResult::Fail(static_cast<int>(ZmErrCode::IoError), "文件过大");
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t readTotal = 0;
    while (readTotal < out.size())
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - readTotal, 1u << 20));
        DWORD done  = 0;
        if (!ReadFile(h, &out[readTotal], chunk, &done, nullptr) || done == 0)
            break;
        readTotal += done;
    }
    CloseHandle(h);
    out.resize(readTotal);
    return ZmStoreResult::Good();
}

bool ZmFileStoreModule::Exists(const std::string& path) const
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return false;
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ZmFileStoreModule::Touch(const std::string& path) const
{
    // 用系统时间改写"最后修改时间":缓存清理以它作为"最后访问"的近似
    // (NTFS 的"最后访问时间"在多数系统上被禁用,不能作为依据)
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return;
    HANDLE h = CreateFileW(w.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

bool ZmFileStoreModule::IsDirectory(const std::string& path) const
{
    std::wstring w = Utf8ToWide(ToExtended(path));
    if (w.empty())
        return false;
    DWORD attr = GetFileAttributesW(w.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int64_t ZmFileStoreModule::FileSize(const std::string& path) const
{
    ZmDiskEntry e;
    // 取信息失败返回 -1,调用方据此区分"不存在"与"空文件"
    ZmStoreResult r = const_cast<ZmFileStoreModule*>(this)->StatSync(path, e);
    return r.ok ? e.size : -1;
}

void ZmFileStoreModule::TreeSizeSync(const std::string& path, int64_t& bytes,
                                     int64_t& items) const
{
    bytes = 0;
    items = 0;
    std::vector<ZmDiskEntry> children;
    ZmStoreResult r = const_cast<ZmFileStoreModule*>(this)->ScanDirSync(path, children);
    if (!r.ok)
        return;
    for (const auto& c : children)
    {
        ++items;
        if (c.isDir)
            TreeSizeSync(path + "\\" + c.name, bytes, items);
        else
            bytes += c.size;
    }
}
