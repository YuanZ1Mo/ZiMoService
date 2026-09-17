#ifndef ZM_MODULE_FILE_ADMIN_H
#define ZM_MODULE_FILE_ADMIN_H

// ============================================================================
// ZmFileAdminModule:文件中心管理
// 职责:概览统计、一致性同步(含取消)、缓存区管理、全量任务、回收站全量管理、
// 审计与分享日志查询、空间配额设置。
// 权限点 filehubAdmin(门禁由 handler 首行 HasPermission 完成)。
// 一致性同步原则:文件系统是事实源,**只补库、只删行,不动物理文件**;
// 与回收站的交界:磁盘→库比对时 deleted=1 的行计入"已登记"(否则会给回收站里的
// 物理文件补出重复行),库→磁盘方向跳过 deleted=1 的行。
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "modules/module_file_node.h" // ZmListQuery / ZmOpCtx

class ZmHttpRestfulServer;
class ZmSessionModule;
class ZmPermissionModule;
class ZmAuthGateModule;
class ZmUserModule;
class ZmFileDbModule;
class ZmFileNodeModule;
class ZmFileTaskModule;
class ZmFileAuditModule;
class ZmFilePackModule;
class ZmFileUploadModule;
class ZmFileStoreModule;
class ZmTaskHandle;
struct ZmGateResult;

class ZmFileAdminModule
{
  public:
    ZmFileAdminModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                      ZmPermissionModule* permission, ZmAuthGateModule* gate,
                      ZmUserModule* user, ZmFileDbModule* db, ZmFileNodeModule* node,
                      ZmFileTaskModule* task, ZmFileAuditModule* audit, ZmFilePackModule* pack,
                      ZmFileStoreModule* store, ZmFileUploadModule* upload);
    ~ZmFileAdminModule();

    /// @brief 注册 /filehub/admin/* 全部路由
    void RegisterRoutes();

    /// @brief 启动一致性同步(同一时刻只允许一个;重复触发 → 409 SYNC_RUNNING)
    /// @param dryRun 只报告不修复
    /// @param ctx 操作者
    /// @return {task_no} 或失败结果
    drogon::Task<ZMJSON> StartSync(bool dryRun, const ZmOpCtx& ctx);

    /// @brief 启动一致性同步(非协程入口:供清理钩子在工作池线程调用)
    ///
    /// 与 StartSync 的唯一区别是任务行用同步 DB 接口创建 —— 清理钩子运行在工作池
    /// 线程,没有可挂起的事件循环;直接丢弃 StartSync 返回的 Task 会因 drogon 协程
    /// initial_suspend 为 suspend_always 而一行都不执行(任务行建了、扫描不跑)。
    ///
    /// @param dryRun  只报告不修复
    /// @param ctx     操作者(清理钩子传系统身份)
    /// @return true 已启动;false 任务行创建失败或已有一轮在运行
    bool StartSyncDetached(bool dryRun, const ZmOpCtx& ctx);

    /// @brief 取消同步(已完成部分保留、不回滚)
    drogon::Task<ZMJSON> CancelSync();

    /// @brief 启动高频巡检(事件循环启动后生效)
    ///
    /// 每 10 分钟一次:回收空闲的打包缓存、回收没有活会话的上传任务。
    /// 与每日 03:00 的全量清理互补 —— 那一条管保留期与阈值,这一条管时效。
    void StartMaintenance();

private:
    // ── 门禁与响应 ──
    drogon::Task<ZmGateResult>     Authorize(const drogon::HttpRequestPtr& req);
    static drogon::HttpResponsePtr Respond(const ZMJSON& out);

    // ── handler ──
    drogon::Task<drogon::HttpResponsePtr> HandleStats(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSyncStart(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSyncStatus(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSyncCancel(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleCache(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleCacheClean(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrash(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashRestore(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashPurge(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashClean(drogon::HttpRequestPtr req);
    /// 整体清空(物理删除全部回收站条目,不受保留期限制)
    drogon::Task<drogon::HttpResponsePtr> HandleTrashClearAll(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTasks(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTaskCancel(drogon::HttpRequestPtr req,
                                                           std::string            taskNo);
    drogon::Task<drogon::HttpResponsePtr> HandleLogs(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleShareLogs(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSpaces(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSetQuota(drogon::HttpRequestPtr req,
                                                         std::string            spaceStr);

    // ── 一致性同步实现(工作池线程内) ──
    /// @brief 任务执行体:跑一轮扫描 → 收尾任务 → 写审计
    ///
    /// 自带兜底收尾:扫描抛异常或漏调 Finish 都不会把任务留在"进行中",并复位同步
    /// 运行标志(否则后续同步会被 409 永久挡住)。调用方负责先置"进行中"。
    ///
    /// @param dryRun  只报告不修复
    /// @param ctx     操作者(清理钩子传系统身份)
    /// @param handle  任务句柄
    void RunSyncBody(bool dryRun, const ZmOpCtx& ctx, ZmTaskHandle& handle);

    /// @brief 复位同步运行标志(正常收尾由 RunSyncSync 完成,异常路径由此兜底)
    void ResetSyncFlags();

    /// @brief 同步执行体:遍历全部空间与目录,按磁盘为准修复漂移
    /// @param taskNo 任务号;dryRun 只报告;handle 进度/取消
    /// @return 报告 {scanned_dirs, scanned_items, added, removed, fixed, skipped, elapsed_ms}
    ZMJSON RunSyncSync(const std::string& taskNo, bool dryRun, ZmTaskHandle* handle);
    /// @brief 递归处理一个目录(读盘 → 比对 → 落库),并展开其子目录
    ZMJSON SyncDirSync(int64_t space, int64_t dirId, bool dryRun, ZmTaskHandle* handle,
                       ZMJSON& report);

    /// 统计用:取今日零点(unix 秒)
    static int64_t TodayStart();

    ZmHttpRestfulServer* m_rest       = nullptr;
    ZmSessionModule*     m_session    = nullptr;
    ZmPermissionModule*  m_permission = nullptr;
    ZmAuthGateModule*    m_gate       = nullptr;
    ZmUserModule*        m_user       = nullptr;
    ZmFileDbModule*      m_db         = nullptr;
    ZmFileNodeModule*    m_node       = nullptr;
    ZmFileTaskModule*    m_task       = nullptr;
    ZmFileAuditModule*   m_audit      = nullptr;
    ZmFilePackModule*    m_pack       = nullptr;
    ZmFileStoreModule*   m_store      = nullptr;
    ZmFileUploadModule*  m_upload     = nullptr;   ///< 巡检用(回收中断的上传任务)

    // 同步运行态(进程内;同一时刻只允许一个)
    std::mutex        m_syncMtx;
    std::atomic<bool> m_syncRunning{false};
    std::atomic<bool> m_syncCancel{false};
    std::string       m_syncTaskNo;
    int64_t           m_syncBeginMs = 0; ///< 本轮开始时刻(steady_clock 毫秒;供运行中 elapsed)
    ZMJSON            m_syncProgress; ///< 运行中进度(供 sync 状态接口)
};

#endif // ZM_MODULE_FILE_ADMIN_H
