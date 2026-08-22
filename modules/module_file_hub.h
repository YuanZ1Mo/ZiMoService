#ifndef MODULE_FILE_HUB_H
#define MODULE_FILE_HUB_H

#include "zm_net_req_loop_protocol.h"   // ZmReqLoop / ZmReqLoopRest / ZMJSON
#include "zm_net_http.h"                // ZmHttpdTask / evhttp_cmd_type / BYTE

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "zm_util_sqlite.h"

class UserModule;
class HttpServerManager;
class ZipWriter;
class ZmEvBaseRunLoop;

/** @brief 一致性校验修复统计(ran=false = 本轮被跳过,另一轮校验已在运行) */
struct VerifyStats
{
    bool ran = false;
    int64_t dirsAdded = 0;    ///< 物理有目录而库无 → 补建行
    int64_t dirsRemoved = 0;  ///< 库有而物理缺失 → 删行(级联子树)
    int64_t filesAdded = 0;   ///< 物理有文件而库无 → 补建行
    int64_t filesRemoved = 0; ///< 库有而物理缺失 → 删行
};

/**
 * @brief 文件中心模块(业务层,数据库驱动)
 *
 * 公共/个人双空间:
 *   - 文件本体:exe 同级 modules\filehub\<space>\(0=公共,uid=个人)
 *   - 元数据:db/filehub/filehub.db(dirs/files/shares/share_download_logs,方案 2 全库驱动)
 *   - 操作日志:业务日志库 db/audit/audit.db 的 filehub_logs(经 UserModule 通用接口)
 *
 * 写操作顺序:鉴权/冲突检查(DB)→ 文件系统 → DB → 日志;DB 失败回滚文件系统。
 * 启动后台线程做一致性校验(以文件系统为准重建/删行,记 restore 日志)。
 *
 * 路由:service_portal.cpp 分发链挂在 portal 模块之前(防 /portal/filehub/* 被 portal 404 吞);
 * 另处理独立顶层路径 /share/<token>(免登录可达,公共分享免鉴权、个人分享需登录)。
 */
class FileHubModule
{
public:
    /** @brief 注入 UserModule(鉴权/角色/模块权限/业务日志)+ 已初始化 filehub.db 连接(不拥有) */
    FileHubModule(UserModule* userModule, zm::ZmSqliteConn& fileHubDb);
    ~FileHubModule();

    /** @brief 启动:检查库可用 + 建文件系统公共根目录 + 起一致性校验线程 */
    bool Open();

    /** @brief 停止钩子:置 gone + join 校验线程(库连接由 DbInitializer 统一关闭) */
    void Shutdown();

    /**
     * @brief REST 分发入口(service_portal.cpp 的 else 链调用,位于 portal 之前)
     * @return true 本模块已处理(含错误响应);false 未命中,走 portal 原逻辑
     */
    bool DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                      ZmHttpdTask* task, const BYTE* body, size_t bodyLen);

    /**
     * @brief 自注册页面端口静态路由(service_portal.cpp 调用一行)
     *        /share/<token>:302 转发到 REST 端口 39441(分享链接免登录可达)
     */
    void RegisterHttpRoutes(HttpServerManager* httpMgr);

    /** @brief 文件仓库根(exe 同级 modules\filehub) */
    std::string GetHubRoot() const;

    /**
     * @brief 通用单文件下载:分块流式发送(1MB 块,支持单段 Range 断点续传)
     *
     * 背景:原实现 SetReplyFile → evbuffer_file_segment → Windows 下 MapViewOfFile
     * 单视图承载长度受 DWORD 限制,>4GB 文件整段映射失败 → 500"无法下载"
     * (IDM 分段 Range 正常)。改为与 zip 打包同模式的分块流式,任意大小文件可用。
     *
     * @param rangeHeader 请求 headers 里可取,传 nullptr/空 = 全文件 200
     * @return 200/206 已启动流式响应(调用方不得再回复);
     *         404 = 文件不存在(未回复,调用方响应);
     *         416 = Range 不满足(已回复 416)
     */
    int SendFileStream(ZmReqLoop* loop, ZmHttpdTask* task, const std::string& physicalPath,
                       const char* rangeHeader);

private:
    // ========================================================================
    // 空间与路径(路径全部由 DB id 组装,不接受用户原始路径,防穿越天然消失)
    // ========================================================================

    /** @brief space 解析:public→0 / personal→当前 uid;非法返回 -1 */
    int SpaceOf(const char* scope, uint64_t uid);

    /** @brief 个人空间懒创建:文件系统目录 + dirs 根行,幂等 */
    bool EnsureUserRoot(uint64_t uid);

    /** @brief 目录物理路径(dir_id 沿 parent 链拼名);dirId=0=空间根 */
    bool DirAbsPath(int space, uint64_t dirId, std::string& absPath);

    /** @brief 条目物理路径 = 目录路径 + 名称 */
    std::string EntryAbsPath(int space, uint64_t dirId, const std::string& name);

    /** @brief 递归收集目录子树全部 dir id(含自身) */
    void CollectSubtreeDirs(int space, uint64_t dirId, std::vector<uint64_t>& out);

    /** @brief 同上,但要求调用方已持 m_db.Mutex()(Verify 级联清理等锁内场景;
     *          勿在锁外调用,否则嵌套加锁抛异常) */
    void CollectSubtreeDirsLocked(int space, uint64_t dirId, std::vector<uint64_t>& out);

    /** @brief 递归收集子树全部文件行(space,dirId 子树的 files) */
    void CollectSubtreeFiles(int space, uint64_t dirId, std::vector<uint64_t>& out);

    /**
     * @brief 批量计算目录子树总大小(roots 各自含自身的子树,一次递归 CTE)
     * @param out 输出 root dir id → 子树总大小(含 files.size 求和);替代逐目录 DirSize 的 N+1
     */
    void CollectDirSizes(int space, const std::vector<uint64_t>& roots,
                         std::map<uint64_t, int64_t>& out);

    /**
     * @brief 批量计算目录相对空间根的路径链(dir_id → "a/b/c",根目录为空串)
     *        一次拉取所有涉及目录行,父链缺失迭代补查;替代 relPathOf 逐层查询
     */
    void BatchRelPaths(const std::vector<uint64_t>& dirIds,
                       std::map<uint64_t, std::string>& out);

    // ========================================================================
    // 鉴权辅助(模块内公共前置)
    // ========================================================================

    /**
     * @brief 鉴权 + 空间解析(HTTP 响应已发出则返回 0)
     * @param outUid   命中时输出用户 id
     * @param outSpace 输出空间 id;space 非法时响应 400
     */
    uint64_t AuthAndSpace(ZmHttpdTask* task, const char* scope, int& outSpace,
                          std::string& outRole, std::string& outAccount);

    /** @brief 写保护:公共空间(space=0)仅 uploader 本人或 developer/admin;个人仅本人;
     *          拒绝时输出具体原因到 errText */
    bool CanModify(const std::string& role, uint64_t opUid, int space, uint64_t uploaderId,
                   std::string& errText);

    /** @brief 业务日志(经 UserModule 写 filehub_logs;targetType 'file'/'dir') */
    void Log(uint64_t opId, const std::string& opAccount,
             const std::string& action, const std::string& targetType,
             uint64_t targetId, const std::string& detail);

    // ========================================================================
    // 列表 / 搜索
    // ========================================================================

    void HandleList(ZmReqLoop* loop, ZmHttpdTask* task, int space, uint64_t opUid);
    void HandleSearch(ZmReqLoop* loop, ZmHttpdTask* task, int space);

    /** @brief 面包屑链(根行显示 公共文件夹/个人文件夹) */
    void BuildPathChain(ZMJSON& pathArr, int space, uint64_t dirId);

    // ========================================================================
    // 写操作(统一:鉴权 → 权限 → 存在性 → 同名冲突 → 文件系统 → DB → 日志 → 回滚)
    // ========================================================================

    void HandleMkdir(ZmReqLoop* loop, const ZMJSON& body, int space, uint64_t opUid,
                     const std::string& opAccount);
    void HandleRename(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                      const std::string& opRole, const std::string& opAccount);
    void HandleMove(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                    const std::string& opRole, const std::string& opAccount);
    void HandleCopy(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                    const std::string& opRole, const std::string& opAccount);
    void HandleDelete(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                      const std::string& opRole, const std::string& opAccount);

    /** @brief 从 body 解析 {type:'file'|'dir', id} 数组;非法返回 false */
    static bool ParseIdList(const ZMJSON& body, std::vector<std::pair<std::string, uint64_t>>& out);

    /** @brief 校验目录 id 存在(space 内);返回 false */
    bool DirExists(int space, uint64_t dirId);

    // ========================================================================
    // 上传 / 下载
    // ========================================================================

    void HandleUpload(ZmReqLoop* loop, ZmHttpdTask* task, int space, uint64_t opUid,
                      const std::string& opAccount);
    void HandleDownload(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid,
                        const std::string& opAccount);

    /** @brief 流式接收上传 body 到 physicalPath;成功输出实际 size */
    int ReceiveFileStream(ZmHttpdTask* task, const std::string& physicalPath, uint64_t& outSize);

    // ========================================================================
    // 传输任务(transfer_tasks 统一记录上传/单文件下载/zip 打包,前端队列历史数据源)
    // ========================================================================

    /** @brief 历史分页查询(加载时顺带标记超时仍 uploading 的行 failed"已中断") */
    void HandleTasks(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid);

    /** @brief 上传预建行(选文件即建,关浏览器后行留痕;参数 task_id/name/size) */
    void HandleTaskCreate(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid);

    /** @brief 取消任务(上传/打包):行置 failed"已取消";打包循环逐文件检测行状态中止 */
    void HandleTaskCancel(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid);

    /** @brief 删除任务行(队列"删除"按钮,校验归属) */
    void HandleTaskDelete(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid);

    /** @brief 任务状态查询(zip 打包轮询;行缺失返回 404,前端视为打包启动中) */
    void HandleTaskStatus(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid);

    // ========================================================================
    // zip 打包下载
    // ========================================================================

    /**
     * @brief 直链流式打包下载(导航请求即打包请求,边打边发,零临时文件)
     *        query: task_id + ids(逗号分隔,前缀 f=文件/d=目录,如 "f12,d34")
     *        建行(packing)→ 先响应头 → 流式打包;每文件检查行状态(取消/删除即中止);
     *        完成置 done+total_size,失败置 failed+err
     */
    void HandleZipDownload(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t opUid,
                           const std::string& opAccount);

    // ---- 传输任务行存取 helper(内部持锁) ----

    /** @brief 建行(INSERT OR IGNORE,幂等);返回是否已存在/新建 */
    bool CreateTransferTask(const std::string& taskId, uint64_t userUid, const std::string& type,
                            const std::string& status, const std::string& name, int64_t totalSize);

    /** @brief 更新行状态(totalSize<0 不更新大小;err 覆盖) */
    bool UpdateTransferTask(const std::string& taskId, uint64_t userUid, const std::string& status,
                            const std::string& err, int64_t totalSize);

    /** @brief 读取行(归属校验合一);返回 false = 行不存在或非本用户 */
    bool LoadTransferTask(const std::string& taskId, uint64_t userUid, std::string& type,
                          std::string& status, std::string& name, int64_t& totalSize,
                          std::string& err);

    /** @brief 单个文件入 zip(条目名 = prefix + 文件名);skipFileIds 命中跳过(去重防御) */
    bool ZipAddFile(ZipWriter& zip, int space, uint64_t fileId, const std::string& entryPrefix,
                    const std::set<uint64_t>& skipFileIds);

    /** @brief 目录递归入 zip(顶层名保留);空目录写目录条目 */
    bool ZipAddDirRecursive(ZipWriter& zip, int space, uint64_t dirId,
                            const std::string& entryPrefix,
                            const std::set<uint64_t>& skipFileIds);

    // ========================================================================
    // 分享
    // ========================================================================

    void HandleShareCreate(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                           const std::string& opAccount);
    void HandleShareAccess(ZmReqLoop* loop, ZmHttpdTask* task, const std::string& token);
    void HandleShareCancel(ZmReqLoop* loop, const ZMJSON& body, uint64_t opUid,
                           const std::string& opAccount);
    void HandleShareList(ZmReqLoop* loop, uint64_t opUid);

    /** @brief 删除目标时级联删相关 shares 行(分享失效) */
    void RemoveSharesOf(const std::string& targetType, uint64_t targetId);

    // ========================================================================
    // 后台周期维护(独立事件循环线程:一致性校验 + 分享下载日志清理 + 传输任务清理)
    // ========================================================================

    /** @brief 定时器到期处理(事件循环线程):每日窗口检查 + 校验/清理整批,异常隔离 */
    void MaintainTick();
    /**
     * @brief 单轮全空间校验(启动首个周期执行一次 + 每日 03:00 窗口 + 手动同步)
     * @return 修复统计;ran=false 表示另一轮校验正在进行(已跳过本轮)
     */
    VerifyStats VerifyOnce();
    /** @brief 校验单个空间目录树与 DB 比对,漂移以文件系统为准修复并记日志 */
    void VerifySpaceTree(int space, const std::string& spaceAbs, VerifyStats& stats);

    /** @brief 清理过期分享下载日志(保留期 kShareDownloadLogRetain,与审计一致) */
    void CleanShareDownloadLogs();

    /**
     * @brief 清理传输任务:done/failed 超 kTaskRetainDays 删行;
     *        非终态(uploading/packing/triggered)超 kTaskStaleForceSec 标 failed"客户端中断"
     */
    void CleanTransferTasks();

    // ========================================================================
    // 文件中心管理(管理模块接口:手动触发一致性同步)
    // ========================================================================

    /** @brief POST /portal/filehubAdmin/sync:手动触发全空间同步(响应含修复统计);
     *          长任务经 PostToLoop 执行并取消请求死线,防超时误杀 */
    void HandleAdminSync(ZmReqLoop* loop);

    // ========================================================================
    // 数据
    // ========================================================================

    zm::ZmSqliteConn& m_db;           ///< filehub.db 连接引用(归 DbInitializer 所有,锁经 m_db.Mutex())
    std::atomic<bool> m_openOk {false};
    std::atomic<bool> m_gone {false};
    std::atomic<bool> m_verifyRunning {false};  ///< 一致性校验运行中(手动/周期互斥)
    ZmEvBaseRunLoop* m_bgLoop = nullptr;   ///< 后台周期维护事件循环(Open 创建,Shutdown 停止)
    int m_lastTickDay = 0;                 ///< 上次维护日(窗口去重,事件循环线程独占)
    UserModule* m_userModule = nullptr;  ///< 注入:鉴权/角色/模块权限/业务日志(不拥有)
    std::string m_hubRoot;            ///< 文件仓库根(exe 同级 modules\filehub)
};

#endif // MODULE_FILE_HUB_H
