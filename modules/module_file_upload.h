#ifndef ZM_MODULE_FILE_UPLOAD_H
#define ZM_MODULE_FILE_UPLOAD_H

// ============================================================================
// ZmFileUploadModule:上传模块
// 职责:小文件单请求上传(≤8MB)、分片上传会话(初始化/分片/合并)、秒传、冲突裁决。
// 写序:物理先落临时文件 → 临时文件 + 原子改名入位 → DB(条目+配额+审计)同事务;
// DB 未落成则删除刚入位的物理文件(否则一致性同步会把它补建成一条 owner=0 的记录)。
// 冲突裁决在**目标目录写锁内**执行:上传期间目标目录可能被他人改动。
// 分片落 space_cache\<space>\chunk\<upload_id>\<index>.part;元数据走请求头。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "modules/module_file_node.h" // ZmOpCtx

class ZmFileDbModule;
class ZmFileStoreModule;
class ZmFileNodeModule;
class ZmFileTaskModule;
class ZmFileAuditModule;
class ZmDirLock;

class ZmFileUploadModule
{
  public:
    ZmFileUploadModule(ZmFileDbModule* db, ZmFileStoreModule* store, ZmFileNodeModule* node,
                       ZmFileTaskModule* task, ZmFileAuditModule* audit, ZmDirLock* lock);
    ~ZmFileUploadModule();

    /**
 * @brief 单请求上传(≤8MB):一次 POST 发送完整文件体
 *
 * @param ctx 操作者
 * @param space 目标空间;dirId 目标目录(0=空间根)
 * @param name 文件名(调用方已解码)
 * @param conflict 冲突策略 ask/skip/rename/overwrite
 * @param body 文件字节
 * @return {node_id, task_no};失败 → {"error":{...}}(ask 冲突时附 conflicts)
 */
    drogon::Task<ZMJSON> Simple(const ZmOpCtx& ctx, int64_t space, int64_t dirId,
                                const std::string& name, const std::string& conflict,
                                const std::string& body);

    /**
 * @brief 初始化分片上传(含秒传判定与断点续传复用)
 *
 * @param ctx 操作者
 * @param space 目标空间;dirId 目标目录
 * @param name 文件名;size 总字节数;hash 整文件 SHA-256(可空)
 * @param conflict 冲突策略(最终裁决在 complete 阶段)
 * @return {upload_id, chunk_size, chunk_total, uploaded[], expire_time, task_no,
 * instant?, node_id?}
 */
    drogon::Task<ZMJSON> Init(const ZmOpCtx& ctx, int64_t space, int64_t dirId,
                              const std::string& name, int64_t size, const std::string& hash,
                              const std::string& conflict);

    /**
 * @brief 接收一个分片(可乱序、可重复上传)
 *
 * @param ctx 操作者
 * @param uploadId 上传会话标识
 * @param index 片号(从 0 开始)
 * @param data 分片字节
 * @return {received, chunk_done, chunk_total}
 */
    drogon::Task<ZMJSON> PutChunk(const ZmOpCtx& ctx, const std::string& uploadId,
                                  int64_t index, const std::string& data);

    /**
 * @brief 完成合并:校验片齐 → 合并 → 校验哈希 → 锁内裁决 → 原子入位 → 落库
 *
 * @param ctx 操作者
 * @param uploadId 上传会话标识
 * @return {node_id, task_no}
 */
    drogon::Task<ZMJSON> Complete(const ZmOpCtx& ctx, const std::string& uploadId);

    /// @brief 查询会话状态(断点续传)
    /// @return {uploaded[], chunk_total, expire_time}
    drogon::Task<ZMJSON> Status(const ZmOpCtx& ctx, const std::string& uploadId);

    /// @brief 取消上传(删分片目录、会话置已取消、任务置已取消)
    drogon::Task<ZMJSON> Cancel(const ZmOpCtx& ctx, const std::string& uploadId);

    /// @brief 重试(type=1 任务):按 upload_id 重跑合并入位
    drogon::Task<ZMJSON> Retry(const ZMJSON& oldTask);

    /// @brief 周期清理:超过有效期的会话置已过期 + 删分片目录与分片行
    void PurgeExpired(int64_t now);

    /// @brief 回收"没有活会话"的上传任务(高频巡检)
    ///
    /// 客户端中断分片上传后不会再回头收尾:会话若已被取消/过期/清掉,任务却会一直
    /// 停在"进行中",既在任务面板里挂着,又占着单用户 ≤3 的上传并发额度。这里把
    /// 这类任务置为已中断。
    ///
    /// @param now 当前 unix 秒
    void SweepStaleTasks(int64_t now);

    /// @brief 计算文件的 SHA-256(十六进制小写;失败返回空串)
    static std::string HashFileSha256(const std::string& path);
    /// @brief 计算内存缓冲的 SHA-256(十六进制小写)
    static std::string HashBufSha256(const char* data, size_t len);

  private:
    /// @brief 入位收尾(单请求/分片合并/秒传共用)
    ///
    /// 冲突裁决(目录锁内)→ 原子改名入位 → 同事务落条目/配额/审计 → 任务收尾。
    /// 任一步失败按阶段回滚(临时文件保留供重试;入位后落库失败则删除物理文件)。
    ///
    /// @param tmpPath 已写好的临时文件
    /// @param taskNo 关联任务号(可为空)
    /// @param instant 是否秒传命中(仅影响审计 detail 的 instant 标记)
    /// @return {node_id, task_no};失败 → {"error":{...}}
    ZMJSON FinalizeSync(const ZmOpCtx& ctx, int64_t space, int64_t dirId,
                        const std::string& name, const std::string& conflict,
                        const std::string& tmpPath, int64_t size, const std::string& hash,
                        const std::string& taskNo, bool instant = false);

    /// @brief 临时文件目录(space_cache\<space>\tmp)
    std::string TmpDir(int64_t space) const;
    /// @brief 生成临时文件路径
    std::string NewTmpPath(int64_t space) const;
    /// @brief 秒传:同空间内按 hash+size 找已有文件
    /// @return 命中条目的物理路径;未命中返回空串
    std::string FindInstantSource(int64_t space, const std::string& hash, int64_t size);

    /// 并发闸门的临界区:检查"进行中上传数"与创建会话/任务必须原子完成
    std::mutex m_gateMtx;

    ZmFileDbModule*    m_db    = nullptr;
    ZmFileStoreModule* m_store = nullptr;
    ZmFileNodeModule*  m_node  = nullptr;
    ZmFileTaskModule*  m_task  = nullptr;
    ZmFileAuditModule* m_audit = nullptr;
    ZmDirLock*         m_lock  = nullptr;
};

#endif // ZM_MODULE_FILE_UPLOAD_H
