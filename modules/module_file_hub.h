#ifndef ZM_MODULE_FILE_HUB_H
#define ZM_MODULE_FILE_HUB_H

// ============================================================================
// ZmFileHubModule:文件中心接口编排
// 面向用户侧全部 REST 接口的注册与编排,是文件中心对外唯一的路由出口。
// handler 固定骨架:取会话 → CheckCtxSync → HasPermission("filehub") →
// 参数解析与校验 → 转发数据服务模块 → 统一响应(失败经 {"error":{...}} 翻译)。
// 权限点:门户点 filehub 同时充当 API 门禁点;管理端接口见 ZmFileAdminModule。
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <string>
#include <vector>

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
class ZmFileUploadModule;
class ZmFilePackModule;
class ZmFileTokenModule;
class ZmFileShareModule;
struct ZmGateResult;

class ZmFileHubModule
{
  public:
    ZmFileHubModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                    ZmPermissionModule* permission, ZmAuthGateModule* gate, ZmUserModule* user,
                    ZmFileDbModule* db, ZmFileNodeModule* node, ZmFileTaskModule* task,
                    ZmFileAuditModule* audit, ZmFileUploadModule* upload,
                    ZmFilePackModule* pack, ZmFileTokenModule* token,
                    ZmFileShareModule* share);
    ~ZmFileHubModule();

    /// @brief 注册用户侧全部路由(含分享公开面;公开面走免鉴权分支)
    void RegisterRoutes();

    /// @brief 空间归属门禁:越权访问他人空间时记一条失败审计并返回 403
    ///
    /// 失败也入审计:被拒绝的写请求要留痕(便于排查"为什么删不掉",也留下越权尝试记录)。
    /// 审计写库走工作池,不在事件循环线程做阻塞 DB。
    ///
    /// @param space   目标空间(0 = 公共空间)
    /// @param ctx     操作者(审计快照 uid/account/ip)
    /// @param action  本次试图执行的动作码(zm_file::kAct*)
    /// @return 非空 = 应直接返回的 403 响应;空 = 放行
    drogon::Task<drogon::HttpResponsePtr> DenyForeignSpace(int64_t space, const ZmOpCtx& ctx,
                                                          const std::string& action);

    /// @brief 注册下载闸门的巡检定时器(事件循环启动后生效)
    ///
    /// 每 30 秒回收一次"客户端在响应开始发送前就断开"而漏掉的并发名额 ——
    /// 正常路径由平台发送结束回调归还,这里只兜底。
    void StartDownloadSweeper();

    /// 登记文件中心的两个权限点(幂等;经权限模块写入 user.db)
    /// 由装配层在用户系统种子之后调用
    void RegisterPermissions();

  private:
    // ── 门禁与响应 ──
    /// 会话 + 账号状态 + 权限点(filehubAdmin 或 filehub)
    drogon::Task<ZmGateResult> Authorize(const drogon::HttpRequestPtr& req,
                                         bool                          admin = false);
    /// 统一响应:失败结果翻译为 {code,message(,附加字段)}
    static drogon::HttpResponsePtr Respond(const ZMJSON& out);
    /// 由会话上下文取操作者(审计用)
    static ZmOpCtx OpOf(const struct ZmSessionCtx& ctx, const drogon::HttpRequestPtr& req);

    // ── 归属名解析(展示层;经用户模块批量取昵称) ──
    /// 为条目数组补 owner_name(owner_uid=0 → "系统")
    drogon::Task<void> FillOwnerNames(ZMJSON& list);
    /// 为回收站数组补 del_owner_name
    drogon::Task<void> FillTrashNames(ZMJSON& list);

    // ── 空间与浏览 ──
    drogon::Task<drogon::HttpResponsePtr> HandleSpaces(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleList(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSearch(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleNode(drogon::HttpRequestPtr req,
                                                     std::string            idStr);
    drogon::Task<drogon::HttpResponsePtr> HandleStat(drogon::HttpRequestPtr req);
    // ── 条目操作 ──
    drogon::Task<drogon::HttpResponsePtr> HandleMkdir(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleEnsure(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleEnsureBatch(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleRename(drogon::HttpRequestPtr req,
                                                       std::string            idStr);
    drogon::Task<drogon::HttpResponsePtr> HandleMove(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleCopy(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleDelete(drogon::HttpRequestPtr req);
    // ── 回收站 ──
    drogon::Task<drogon::HttpResponsePtr> HandleTrash(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashRestore(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashPurge(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTrashClear(drogon::HttpRequestPtr req);
    // ── 上传 ──
    drogon::Task<drogon::HttpResponsePtr> HandleUploadSimple(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleUploadInit(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleUploadChunk(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleUploadComplete(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleUploadStatus(drogon::HttpRequestPtr req,
                                                             std::string            uploadId);
    drogon::Task<drogon::HttpResponsePtr> HandleUploadCancel(drogon::HttpRequestPtr req,
                                                             std::string            uploadId);
    // ── 下载与打包 ──
    drogon::Task<drogon::HttpResponsePtr> HandleDownloadToken(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr>
    HandleDownload(drogon::HttpRequestPtr req, std::string token, std::string filename);
    drogon::Task<drogon::HttpResponsePtr> HandlePack(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandlePackClean(drogon::HttpRequestPtr req,
                                                          std::string            taskNo);
    // ── 传输任务 ──
    drogon::Task<drogon::HttpResponsePtr> HandleTasks(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTasksActive(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleTaskDetail(drogon::HttpRequestPtr req,
                                                           std::string            taskNo);
    drogon::Task<drogon::HttpResponsePtr> HandleTaskCancel(drogon::HttpRequestPtr req,
                                                           std::string            taskNo);
    drogon::Task<drogon::HttpResponsePtr> HandleTaskRetry(drogon::HttpRequestPtr req,
                                                          std::string            taskNo);
    drogon::Task<drogon::HttpResponsePtr> HandleTasksClear(drogon::HttpRequestPtr req);
    // ── 分享(登录侧) ──
    drogon::Task<drogon::HttpResponsePtr> HandleShareCreate(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleShareList(drogon::HttpRequestPtr req);
    drogon::Task<drogon::HttpResponsePtr> HandleSharePatch(drogon::HttpRequestPtr req,
                                                           std::string            idStr);
    drogon::Task<drogon::HttpResponsePtr> HandleShareCancel(drogon::HttpRequestPtr req,
                                                            std::string            idStr);
    drogon::Task<drogon::HttpResponsePtr> HandleShareLogs(drogon::HttpRequestPtr req,
                                                          std::string            idStr);
    // ── 分享(公开面,免会话) ──
    drogon::Task<drogon::HttpResponsePtr> HandleShareInfo(drogon::HttpRequestPtr req,
                                                          std::string            token);
    drogon::Task<drogon::HttpResponsePtr> HandleShareVerify(drogon::HttpRequestPtr req,
                                                            std::string            token);
    drogon::Task<drogon::HttpResponsePtr> HandleShareListDir(drogon::HttpRequestPtr req,
                                                             std::string            token);
    drogon::Task<drogon::HttpResponsePtr> HandleShareDownload(drogon::HttpRequestPtr req,
                                                              std::string            token);
    /// 公开面写凭证 Cookie(提取码校验通过 / 免提取码分享)
    static void SetShareCookie(const drogon::HttpResponsePtr& resp, const std::string& token,
                               const std::string& cred);

    /// 登记可重试任务类型的执行体(打包/复制/回收站清理/目录统计)
    void RegisterRetryExecs();

    ZmHttpRestfulServer* m_rest       = nullptr;
    ZmSessionModule*     m_session    = nullptr;
    ZmPermissionModule*  m_permission = nullptr;
    ZmAuthGateModule*    m_gate       = nullptr;
    ZmUserModule*        m_user       = nullptr;
    ZmFileDbModule*      m_db         = nullptr;
    ZmFileNodeModule*    m_node       = nullptr;
    ZmFileTaskModule*    m_task       = nullptr;
    ZmFileAuditModule*   m_audit      = nullptr;
    ZmFileUploadModule*  m_upload     = nullptr;
    ZmFilePackModule*    m_pack       = nullptr;
    ZmFileTokenModule*   m_token      = nullptr;
    ZmFileShareModule*   m_share      = nullptr;
};

#endif // ZM_MODULE_FILE_HUB_H
