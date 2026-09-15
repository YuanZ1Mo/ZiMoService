#ifndef ZM_MODULE_FILE_TASK_H
#define ZM_MODULE_FILE_TASK_H

// ============================================================================
// ZmFileTaskModule:传输任务模块
// 职责:任务落库与状态机流转、进度上报(节流写库)、取消/重试、列表查询、
// 启动期僵尸任务清理。
// 对外只暴露 task_no(随机串),不暴露自增 id。
// 取消语义分两级:排队中直接置已取消;进行中置**内存取消标记**,由执行方在
// 条目/分片边界观察到后自行收尾(清理半成品)再置位 —— 不强杀正在写盘的线程。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

class ZmFileDbModule;

/// 任务执行体的运行期句柄(上报进度 / 响应取消 / 收尾)
class ZmTaskHandle
{
  public:
    /**
 * @brief 构造句柄
 *
 * @param owner 所属任务模块
 * @param taskNo 任务号
 * @param uid 发起者
 * @param space 所在空间
 */
    ZmTaskHandle(class ZmFileTaskModule* owner, std::string taskNo, int64_t uid,
                 int64_t space);

    /// @return 任务号
    const std::string& TaskNo() const { return m_taskNo; }
    /// @return 发起者 uid
    int64_t Uid() const { return m_uid; }
    /// @return 所在空间
    int64_t Space() const { return m_space; }
    /// @return 是否已被请求取消(执行方应在条目边界检查)
    bool Cancelled() const;

    /**
 * @brief 上报进度(节流:距上次写库 ≥500ms 或完成度跨 5% 才落库)
 *
 * @param doneSize 已处理字节数
 * @param doneItems 已处理条目数
 */
    void Progress(int64_t doneSize, int64_t doneItems);

    /**
 * @brief 收尾:置终态并清除取消标记
 *
 * @param status 终态(3=已完成 4=失败 5=已取消 6=已中断)
 * @param error 失败原因(可空)
 * @param result 结果串(如压缩包名 / 统计 JSON;可空)
 */
    void Finish(int status, const std::string& error = "", const std::string& result = "");

  private:
    ZmFileTaskModule* m_owner = nullptr;
    std::string       m_taskNo;
    int64_t           m_uid         = 0;
    int64_t           m_space       = 0;
    int64_t           m_lastWriteMs = 0;  ///< 上次落库时刻(毫秒)
    int64_t           m_lastPct     = -1; ///< 上次落库时的完成度百分比
};

class ZmFileTaskModule
{
  public:
    /// 重试执行体:入参为原任务行,返回 {task_no} 或业务失败结果
    using RetryExec = std::function<drogon::Task<ZMJSON>(const ZMJSON& oldTask)>;

    explicit ZmFileTaskModule(ZmFileDbModule* db);
    ~ZmFileTaskModule();

    /// @brief 启动期僵尸任务清理:进行中(2)的任务置已中断(6),错误信息"服务重启中断"
    void MarkZombieTasks();

    // ── 创建 ──
    /**
 * @brief 创建任务(状态=排队中)
 *
 * @param type 任务类型(zm_file::kTask*)
 * @param uid 发起者
 * @param space 所在空间
 * @param name 展示名(文件名/压缩包名/目录名)
 * @param target 展示用路径
 * @param size 总字节数(未知 0)
 * @param totalItems 总条目数(未知 0)
 * @param refId 关联标识(上传:upload_id;打包:压缩包名;回收站清理:space)
 * @return {task_no}
 */
    drogon::Task<ZMJSON> Create(int type, int64_t uid, int64_t space, const std::string& name,
                                const std::string& target, int64_t size, int64_t totalItems,
                                const std::string& refId);

    /**
 * @brief 同步创建任务(已在工作池线程内时用,省一次协程投递)
 *
 * @param type 任务类型;uid 发起者;space 空间
 * @param name 展示名;target 展示路径;size 总字节;totalItems 总条目数
 * @param refId 关联标识
 * @param taskNoOut [out] 新任务号
 * @return true 创建成功
 */
    bool CreateSync(int type, int64_t uid, int64_t space, const std::string& name,
                    const std::string& target, int64_t size, int64_t totalItems,
                    const std::string& refId, std::string& taskNoOut);

    /**
 * @brief 创建任务并立即在工作池启动执行体
 *
 * 执行体在工作池线程运行,通过 ZmTaskHandle 上报进度与收尾;执行体内部只允许
 * 调用 *Sync 方法,不得 co_await。
 *
 * @param body 执行体(参数为任务句柄)
 * @return {task_no}
 */
    drogon::Task<ZMJSON> Start(int type, int64_t uid, int64_t space, const std::string& name,
                               const std::string& target, int64_t size, int64_t totalItems,
                               const std::string&                 refId,
                               std::function<void(ZmTaskHandle&)> body);

    // ── 查询 ──
    /// @brief 任务列表(用户侧;type/status ≤0 表示不过滤)
    drogon::Task<ZMJSON> List(int64_t uid, int type, int status, int page, int size);
    /// @brief 全量任务列表(管理侧;uid=0 表示不限)
    drogon::Task<ZMJSON> AdminList(int64_t uid, int type, int status, int page, int size);
    /// @brief 排队中 + 进行中的任务精简列表(前端轮询主接口)
    /// @param adminAll true = 全量(管理端);false = 仅本人
    drogon::Task<ZMJSON> Active(int64_t uid, bool adminAll);
    /// @brief 任务详情(附 result)
    drogon::Task<ZMJSON> Detail(const std::string& taskNo, int64_t uid, bool adminAll);

    // ── 操作 ──
    /// @brief 取消任务(排队中直接置已取消;进行中置取消标记;管理端强制置位)
    drogon::Task<ZMJSON> Cancel(const std::string& taskNo, int64_t uid, bool adminAll);
    /// @brief 清除历史(终态任务;status ≤0 表示全部终态)
    drogon::Task<ZMJSON> Clear(int64_t uid, int status, bool adminAll);
    /// @brief 重试:按原任务类型重建新任务并重新执行(不复活旧行)
    drogon::Task<ZMJSON> Retry(const std::string& taskNo, int64_t uid);
    /// @brief 登记某类型的重试执行体(未登记的类型重试返回明确失败)
    void RegisterRetryExec(int type, RetryExec exec);

    // ── 同步接口(执行体/句柄用;工作池线程内调用) ──
    /// @return 任务行(不存在返回空对象)
    ZMJSON TaskRowSync(const std::string& taskNo);
    /// @brief 进度落库(节流由调用方负责)
    bool UpdateProgressSync(const std::string& taskNo, int64_t doneSize, int64_t doneItems);
    /// @brief 置状态(可带错误与结果)
    bool SetStatusSync(const std::string& taskNo, int status, const std::string& error = "",
                       const std::string& result = "");
    /// @return 是否已请求取消
    bool IsCancelled(const std::string& taskNo) const;
    /// @brief 清除取消标记(重试/收尾时)
    void ClearCancel(const std::string& taskNo);
    /// @brief 批量清除取消标记(按前缀)
    void ClearCancelByRef(const std::string& taskNo) { ClearCancel(taskNo); }
    /// @return 该用户进行中的上传任务数(并发闸门 ≤3)
    int64_t CountRunningUploadsSync(int64_t uid);
    /// @return 该用户进行中的打包任务数(并发闸门 ≤2)
    int64_t CountRunningPacksSync(int64_t uid);
    /// @brief 启动"打包闸门":全局同时进行的打包任务数(≤2)
    /// @return true 可立即开始;false 需排队
    bool TryAcquirePackSlot(const std::string& taskNo);
    /// @brief 释放打包闸门
    void ReleasePackSlot(const std::string& taskNo);
    /// @brief 等待打包闸门(在闸门满时按序排队)
    /// @param taskNo 任务号;handle 用于观察取消
    /// @return true 已获得;false 被取消
    bool WaitPackSlot(const std::string& taskNo, ZmTaskHandle* handle);

    // ── 重试输入登记(进程内;服务重启后失效 → 重试提示重新发起) ──
    /// @brief 登记重试所需输入
    void SetRetryPayload(const std::string& taskNo, const ZMJSON& payload);
    /// @return 重试输入(不存在返回空对象)
    ZMJSON GetRetryPayload(const std::string& taskNo);
    /// @brief 丢弃重试输入(任务清理时)
    void DropRetryPayload(const std::string& taskNo);

    /// @brief 任务行 → 对外 JSON
    static ZMJSON TaskView(const ZMJSON& row);

  private:
    /// @brief 全局打包闸门可用槽位数
    static constexpr int kPackSlots = 2;

    ZmFileDbModule* m_db = nullptr;

    // 取消标记与打包闸门(进程内)
    mutable std::mutex                       m_mtx;
    std::unordered_map<std::string, bool>    m_cancelFlags;  ///< task_no → 已请求取消
    std::unordered_map<std::string, int64_t> m_packRunning;  ///< task_no → 占位时刻
    std::unordered_map<std::string, ZMJSON>  m_retryPayload; ///< task_no → 重试输入
    std::unordered_map<int, RetryExec>       m_retryExecs;   ///< 类型 → 重试执行体
};

#endif // ZM_MODULE_FILE_TASK_H
