#ifndef ZM_MODULE_FILE_PACK_H
#define ZM_MODULE_FILE_PACK_H

// ============================================================================
// ZmFilePackModule:打包模块
// 职责:展开待打包条目集合(含子树)、用 minizip-ng 在工作池构建 zip、
// 进度上报、取消、压缩包清理。
// 先压缩好再直链下载(而非流式打包):压缩包是稳定文件,可 Content-Length、
// 可 Range 断点续传、可交给外部下载器;代价是多一次磁盘读写,由缓存清理兜底。
// 压缩算法:deflate 级别 1;对已压缩格式(图片/视频/归档/Office)走 store 只封装。
// 清理以**空闲超时**为准(30 分钟),不按"响应结束即删" —— 多线程下载器会并发
// 发起多个 Range 请求,按请求结束即删会误删。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "modules/module_file_node.h" // ZmOpCtx

class ZmFileDbModule;
class ZmFileStoreModule;
class ZmFileNodeModule;
class ZmFileTaskModule;
class ZmFileAuditModule;
class ZmFileTokenModule;

class ZmFilePackModule
{
  public:
    ZmFilePackModule(ZmFileDbModule* db, ZmFileStoreModule* store, ZmFileNodeModule* node,
                     ZmFileTaskModule* task, ZmFileAuditModule* audit,
                     ZmFileTokenModule* token);
    ~ZmFilePackModule();

    /**
 * @brief 创建打包任务并启动执行(闸门在任务体内排队)
 *
 * @param space 空间;ids 待打包条目(目录递归)
 * @param ctx 操作者
 * @return {task_no};超上限 → {"error":{...}}(PACK_TOO_LARGE)
 */
    drogon::Task<ZMJSON> Create(int64_t space, const std::vector<int64_t>& ids,
                                const ZmOpCtx& ctx);

    /// @brief 打包任务执行体(工作池线程内运行;由 Create / 重试路径调用)
    /// @param taskNo 任务号
    void RunPack(const std::string& taskNo);

    /// @brief 重试(type=3):按登记的输入重新打包
    drogon::Task<ZMJSON> Retry(const ZMJSON& oldTask);

    /**
 * @brief 清理某任务的压缩包(用户主动 / 管理端)
 *
 * @param taskNo 任务号;uid 操作者;adminAll true = 管理端(不受归属限制)
 * @return {} 或 {"error":{...}}
 */
    drogon::Task<ZMJSON> CleanTask(const std::string& taskNo, int64_t uid, bool adminAll);

    /// @brief 记录压缩包被访问(把文件时间戳推到现在,空闲清理据此判定)
    void TouchAccess(const std::string& taskNo);

    /// @brief 空闲清理:最后一次访问超过空闲阈值的压缩包即删除(高频巡检,见 StartMaintenance)
    /// @param now 当前 unix 秒
    void CleanIdle(int64_t now);

    /// @brief 周期清理:空闲超时 / 24 小时兜底 / 容量阈值 / 孤儿分片目录
    void CleanCache(int64_t now);

    /// @brief 启动期:进行中的打包任务置已中断,其半成品由缓存清理回收
    void RecoverOrphans();

    /// @brief 缓存区统计(管理端)
    /// @param out zips/chunks/bytes 累加
    void StatCache(int64_t& bytes, int64_t& zips, int64_t& chunks);

    /// @brief 缓存区文件清单(管理端列表)
    /// @return [{name, size, space, create_time, access_time, kind}]
    drogon::Task<ZMJSON> ListCache();

    /// @brief 清理指定缓存文件(管理端;names 为空 = 全部)
    /// @return {deleted, bytes}
    drogon::Task<ZMJSON> CleanCacheFiles(const std::vector<std::string>& names);

  private:
    /// @brief 压缩包物理路径
    std::string ZipPath(int64_t space, const std::string& zipName) const;
    /// @brief 压缩包显示名(单目录/单文件用其名,多个用"打包下载")
    static std::string DisplayName(const ZMJSON& firstNode, size_t count);
    /// @brief 容量保护:总占用超阈值时按最久未访问优先删压缩包,回落到 80%
    void EnforceCacheQuota(int64_t now);
    /// @brief 生成缓存文件名 `<uid>_<yyyyMMddHHmmss>_<6 位随机>.zip`
    static std::string CacheFileName(int64_t uid);

    ZmFileDbModule*    m_db    = nullptr;
    ZmFileStoreModule* m_store = nullptr;
    ZmFileNodeModule*  m_node  = nullptr;
    ZmFileTaskModule*  m_task  = nullptr;
    ZmFileAuditModule* m_audit = nullptr;
    ZmFileTokenModule* m_token = nullptr;

    std::mutex                               m_mtx;
    std::unordered_map<std::string, int64_t> m_access; ///< task_no → 最后访问时刻
};

#endif // ZM_MODULE_FILE_PACK_H
