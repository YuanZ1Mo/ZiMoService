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
#include <thread>
#include <vector>

#include "zm_util_sqlite.h"

class UserModule;
class HttpServerManager;
class ZipWriter;

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

    /** @brief 通用单文件下载(带 Range 断点续传;零拷贝 SetReplyFile) */
    int SendFile(ZmHttpdTask* task, const std::string& physicalPath);

    /** @brief Range 断点续传回复(单段 range);非 Range 请求返回 -1 */
    int ServeFileWithRange(ZmHttpdTask* task, const std::string& path,
                           const std::string& rangeStr, int64_t fileSize);

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
    // zip 打包下载
    // ========================================================================

    void HandleZip(ZmReqLoop* loop, ZmHttpdTask* task, const ZMJSON& body, int space,
                   uint64_t opUid, const std::string& opAccount);

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
    // 启动一致性校验(独立线程)
    // ========================================================================

    void VerifyLoop();
    /** @brief 单轮全空间校验(启动即执行一次 + 每日 03:00 窗口) */
    void VerifyOnce();
    /** @brief 校验单个空间目录树与 DB 比对,漂移以文件系统为准修复并记日志 */
    void VerifySpaceTree(int space, const std::string& spaceAbs);

    /** @brief 清理过期分享下载日志(保留期 kShareDownloadLogRetain,与审计一致) */
    void CleanShareDownloadLogs();

    // ========================================================================
    // 数据
    // ========================================================================

    zm::ZmSqliteConn& m_db;           ///< filehub.db 连接引用(归 DbInitializer 所有,锁经 m_db.Mutex())
    std::atomic<bool> m_openOk {false};
    std::atomic<bool> m_gone {false};
    std::thread m_verifyThread;       ///< 一致性校验线程(Open 启动,Shutdown join)
    UserModule* m_userModule = nullptr;  ///< 注入:鉴权/角色/模块权限/业务日志(不拥有)
    std::string m_hubRoot;            ///< 文件仓库根(exe 同级 modules\filehub)
};

#endif // MODULE_FILE_HUB_H
