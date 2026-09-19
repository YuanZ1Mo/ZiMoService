#ifndef ZM_MODULE_FILE_STORE_H
#define ZM_MODULE_FILE_STORE_H

// ============================================================================
// ZmFileStoreModule:文件中心物理存储
// 路径映射:由节点 id 沿 parent_id 链从库中取 name 逐级拼出物理路径
// (**不接收任何客户端路径字符串**),统一加 \\?\ 前缀绕过 MAX_PATH。
// 物理操作:建目录 / 原子改名入位 / 复制 / 递归删除 / 磁盘枚举。
// 缓存区:space_cache\<space>\zip(压缩包)与 \chunk\<upload_id>(分片)。
// 回收站区:space_trash\<space>\<node_id>\(条目原名);属用户数据,不随缓存清理删除。
// 本模块不查权限、不做同名裁决,只接收已确定的物理路径;错误经 ZmStoreResult
// 带错误码上抛,Win32 错误码在本层收敛为 ZmErrCode。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <string>
#include <vector>

class ZmFileDbModule;

/// 物理层错误码(与 HTTP 层错误码的对应关系)
enum class ZmErrCode : int
{
    Ok           = 0,
    NotFound     = 1, ///< 文件/目录不存在 → 404 NODE_NOT_FOUND
    Locked       = 2, ///< 被其它进程占用 → 409 FILE_LOCKED
    AccessDenied = 3, ///< 权限不足/只读 → 409 FILE_LOCKED
    PathTooLong  = 4, ///< 超 240 字符 → 400 PATH_TOO_LONG
    DiskFull     = 5, ///< 磁盘空间不足 → 507 INSUFFICIENT_STORAGE
    IoError      = 6, ///< 其它 IO 错误 → 500 INTERNAL
};

/// 物理操作结果(成功/失败错误码/可读消息)
struct ZmStoreResult
{
    bool        ok   = false;
    int         code = 0; ///< 0=成功;非 0 = ZmErrCode
    std::string message;  ///< 可读消息(中文,可直接回给客户端)

    /// @brief 构造失败结果
    /// @param code_ 错误码(ZmErrCode)
    /// @param message_ 可读消息
    /// @return 失败结果
    static ZmStoreResult Fail(int code_, const std::string& message_)
    {
        ZmStoreResult r;
        r.ok      = false;
        r.code    = code_;
        r.message = message_;
        return r;
    }
    /// @return 成功结果
    static ZmStoreResult Good()
    {
        ZmStoreResult r;
        r.ok   = true;
        r.code = 0;
        return r;
    }
};

/// 磁盘条目(一致性扫描与缓存统计用)
struct ZmDiskEntry
{
    std::string name; ///< 条目名(磁盘真实名)
    bool        isDir = false;
    int64_t     size  = 0; ///< 文件字节数;目录为 0
    /// 最后修改时间(unix 秒);缓存清理的空闲判定即以此为"最后访问"
    int64_t     mtime = 0;
    int64_t     ctime = 0; ///< 创建时间(unix 秒;仅展示用)
};

class ZmFileStoreModule
{
  public:
    /**
 * @brief 构造物理存储模块
 *
 * @param db 数据访问模块(拼路径时取条目名与父链)
 * @param rootDir 文件中心根目录(其下为 space\ 、space_cache\ 与 space_trash\)
 */
    ZmFileStoreModule(ZmFileDbModule* db, const std::string& rootDir);
    ~ZmFileStoreModule();

    // ── 目录位置 ──
    /// @return 空间根物理目录(调用方不必持有)
    std::string SpaceRoot(int64_t space) const;
    /// @return 空间缓存根目录
    std::string CacheRoot(int64_t space) const;
    /// @return 空间打包产物目录
    std::string ZipDir(int64_t space) const;
    /// @return 上传分片目录
    std::string ChunkDir(int64_t space, const std::string& uploadId) const;
    /// @return 空间回收站根目录(其下按条目 id 再分一层)
    std::string TrashRoot(int64_t space) const;
    /// @return 回收站中某条目的存放目录(其下即条目原名)
    std::string TrashEntryDir(int64_t space, int64_t nodeId) const;
    /// @return 回收站中某条目的物理路径(条目原名作最后一级)
    std::string TrashEntryPath(int64_t space, int64_t nodeId, const std::string& name) const;
    /// @return 文件中心根目录
    const std::string& RootDir() const { return m_rootDir; }

    /**
 * @brief 由条目 id 组装物理绝对路径(目录/文件通用)
 *
 * 沿 parent 链从库中取 name 拼接;**不校验可见树**,可见性判断由调用方负责。
 * 注意:回收站条目的文件不在本函数算出的路径上,而在 TrashEntryPath 处,两者勿混用。
 *
 * @param nodeId 条目 id;0 = 空间根
 * @param space [in,out] 传入时为期望空间;nodeId=0 时用其直接出路径;
 * nodeId>0 时由库中行覆盖为条目实际所属空间
 * @param out [out] 物理路径(不带 \\?\ 前缀)
 * @return 结果;条目不存在 → NotFound,超长 → PathTooLong
 */
    ZmStoreResult PhysicalPathSync(int64_t nodeId, int64_t& space, std::string& out);

    /// @brief 条目 id → 物理路径(协程版,供 handler 层使用;space 由 id 决定)
    /// @param nodeId 条目 id
    /// @return {ok, path, space};失败时 ok=false
    drogon::Task<ZMJSON> PhysicalPath(int64_t nodeId);

    // ── 物理操作(全部同步;须在工作池线程内调用) ──
    /// @brief 创建目录(含多级;已存在视为成功)
    ZmStoreResult EnsureDir(const std::string& path);
    /// @brief 确保空间根目录存在(个人空间懒创建的物理侧;幂等)
    /// @param space 空间号
    ZmStoreResult EnsureSpaceRoot(int64_t space) { return EnsureDir(SpaceRoot(space)); }
    /// @brief 临时文件改写为目标名(覆盖已存在文件);同盘原子
    ZmStoreResult AtomicPlace(const std::string& tmpPath, const std::string& finalPath);
    /// @brief 改名/移动(文件或目录;目标已存在时先删目标文件)
    ZmStoreResult MovePath(const std::string& src, const std::string& dst);
    /// @brief 复制(文件复制;目录递归复制整棵子树)
    ///
    /// 文件先写到目标同目录的临时名再原子替换:覆盖已有文件时不会出现"半截内容"的可见窗口。
    ZmStoreResult CopyTreeSync(const std::string& src, const std::string& dst);

    /// @brief 是否为复制过程的临时文件名(一致性同步据此跳过,避免把残留临时文件补建成条目)
    /// @param name 文件名(不含路径)
    /// @return true 是临时名
    static bool IsCopyTempName(const std::string& name);
    /// @brief 递归删除(文件或目录;只读属性先清除)
    ZmStoreResult RemoveTreeSync(const std::string& path);
    /// @brief 删除单个文件(不存在视为成功)
    ZmStoreResult RemoveFileSync(const std::string& path);
    /// @brief 枚举目录下的直接子项
    ZmStoreResult ScanDirSync(const std::string& path, std::vector<ZmDiskEntry>& out);
    /// @brief 取单个路径的信息(文件或目录)
    ZmStoreResult StatSync(const std::string& path, ZmDiskEntry& out);
    /// @brief 路径是否存在
    bool Exists(const std::string& path) const;
    /// @brief 是否为目录
    bool IsDirectory(const std::string& path) const;
    /// @brief 取文件大小(不存在返回 -1)
    int64_t FileSize(const std::string& path) const;
    /// @brief 把文件的"最后修改时间"置为当前(缓存文件的"最后访问"标记;失败无副作用)
    void Touch(const std::string& path) const;

    /// @brief 统计目录占用(递归;返回字节数与条目数)
    void TreeSizeSync(const std::string& path, int64_t& bytes, int64_t& items) const;

    // ── 文件读写(工作池线程内;上传落盘与分片合并用) ──
    /// @brief 覆盖写文件(父目录不存在时创建)
    ZmStoreResult WriteFileSync(const std::string& path, const char* data, size_t len);
    /// @brief 追加写(不存在则创建)
    ZmStoreResult AppendFileSync(const std::string& path, const char* data, size_t len);
    /// @brief 读整个文件(超过 maxBytes 时失败,避免误读大文件进内存)
    /// @param maxBytes 上限(0 = 不限)
    ZmStoreResult ReadFileSync(const std::string& path, std::string& out,
                               int64_t maxBytes = 0);

    // ── 路径校验 ──
    /// @brief 校验逻辑路径长度(含空间根拼接后 ≤ 240 字符)
    /// @param space 空间
    /// @param relPath 相对空间根的路径(以 \ 分隔,不含空间根)
    /// @param message [out] 失败原因
    /// @return true 长度合法
    bool ValidateLength(int64_t space, const std::string& relPath, std::string& message) const;
    /// @brief 加 \\?\ 前缀(已是长路径形式时原样返回)
    static std::string ToExtended(const std::string& path);
    /// @return 空间根物理目录(静态版,供无实例场景用)
    static std::string SpaceRootOf(const std::string& rootDir, int64_t space);

  private:
    /// @brief Win32 错误码 → ZmErrCode
    static int MapWin32Error(unsigned long err);
    /// @brief 递归删除实现(工作池线程内)
    ZmStoreResult RemoveTreeImpl(const std::string& path);
    /// @brief 递归复制实现(工作池线程内)
    ZmStoreResult CopyTreeImpl(const std::string& src, const std::string& dst);

    ZmFileDbModule* m_db = nullptr;
    std::string     m_rootDir; ///< 文件中心根目录(不带尾部分隔符)
};

#endif // ZM_MODULE_FILE_STORE_H
