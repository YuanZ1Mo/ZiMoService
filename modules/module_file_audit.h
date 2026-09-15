#ifndef ZM_MODULE_FILE_AUDIT_H
#define ZM_MODULE_FILE_AUDIT_H

// ============================================================================
// ZmFileAuditModule:文件中心审计模块
// 职责:file_logs(业务审计)与 share_logs(分享访问日志)的**唯一写入方**与查询服务。
// 两个写入入口按"这次操作有没有业务事务"分:
// · RecordFileOpSync / RecordBatchSync —— 写操作,在调用方事务内落库
// (有操作必有记录,要么都成要么都不成);
// · RecordDownload / RecordShareAccess —— 只读行为,独立异步写,失败只记警告。
// 日志格式、脱敏、字段拼装集中在本模块,业务模块只描述"谁对什么做了什么"。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>
#include <zm_util_sqlite.h>

#include <cstdint>
#include <string>

class ZmFileDbModule;

/// 审计日志查询条件(空值表示不过滤)
struct ZmFileLogQuery
{
    int64_t     uid = 0;    ///< 操作者(0 = 不限)
    std::string action;     ///< 动作(空 = 不限)
    int64_t     space = -1; ///< 空间(-1 = 不限)
    int64_t     from  = 0;  ///< 起始时间(0 = 不限)
    int64_t     to    = 0;  ///< 结束时间(0 = 不限)
    std::string keyword;    ///< 条目名关键词(空 = 不限)
};

/// 分享访问日志查询条件
struct ZmShareLogQuery
{
    std::string token;       ///< 分享 token(空 = 不限)
    int64_t     shareId = 0; ///< 分享 id(0 = 不限)
    int64_t     from    = 0;
    int64_t     to      = 0;
};

class ZmFileAuditModule
{
  public:
    explicit ZmFileAuditModule(ZmFileDbModule* db);
    ~ZmFileAuditModule();

    // ── 入口一:参与调用方事务(写操作) ──
    /**
 * @brief 记录一条文件业务审计(与调用方同事务)
 *
 * 必须在 WithTx 回调内调用,传入回调形参里的连接 —— 同一线程重入同一把递归锁,
 * 落在当前事务里。失败时调用方应让事务整体回滚(审计不可丢)。
 *
 * @param db 调用方事务连接(来自 WithTx 回调形参)
 * @param uid 操作者 uid
 * @param account 操作者账号快照
 * @param action 动作(见 module_file_defs 的 zm_file::kAct*)
 * @param space 条目所属空间
 * @param nodeId 目标条目 id(无具体条目时传 0)
 * @param nodeName 目标名称/路径快照
 * @param detail 详情(JSON 串;可为空)
 * @param ip 客户端 IP
 * @param result 1=成功 2=失败
 * @return true 写入成功
 */
    bool RecordFileOpSync(ZmSqliteDb& db, int64_t uid, const std::string& account,
                          const std::string& action, int64_t space, int64_t nodeId,
                          const std::string& nodeName, const std::string& detail,
                          const std::string& ip, int result);

    /**
 * @brief 批量操作记一条审计(删除这类"一批一条"的场景)
 *
 * detail 内含条目清单摘要:条目数 > 200 时只记前 200 条与总数。
 *
 * @param db 调用方事务连接
 * @param uid 操作者 uid
 * @param account 操作者账号快照
 * @param action 动作
 * @param space 空间
 * @param items 条目数组,每项含 id/name/(可选)type/size
 * @param ip 客户端 IP
 * @param result 1=成功 2=失败
 * @return true 写入成功
 */
    bool RecordBatchSync(ZmSqliteDb& db, int64_t uid, const std::string& account,
                         const std::string& action, int64_t space, const ZMJSON& items,
                         const std::string& ip, int result);

    // ── 入口二:独立异步写(只读行为) ──
    /// @brief 记录下载审计(action=download)
    /// @param bytesSent 实际发送字节数;ranged 是否 Range 请求;result 1=成功 2=失败
    /// @return true 写入成功(失败只记警告,不影响响应)
    drogon::Task<bool> RecordDownload(int64_t uid, const std::string& account, int64_t space,
                                      int64_t nodeId, const std::string& nodeName,
                                      int64_t bytesSent, bool ranged, const std::string& ip,
                                      int result);

    /// @brief 记录分享访问日志
    /// @param shareId 分享 id;token 分享标识快照
    /// @param action 1=访问 2=提取码校验 3=列目录 4=下载 5=打包下载
    /// @param uid 登录用户(0=免登录)
    /// @param detail 失败原因/字节数等
    drogon::Task<bool> RecordShareAccess(int64_t shareId, const std::string& token, int action,
                                         int result, int64_t uid, const std::string& ip,
                                         const std::string& ua, const std::string& detail);

    // ── 查询(管理端 / 我的分享) ──
    /// @return {total, page, size, list}(list 含 uid/account/action/space/node_name/detail/ip/result/create_time)
    drogon::Task<ZMJSON> QueryFileLogs(const ZmFileLogQuery& q, int page, int size);
    /// @return {total, page, size, list}(IP/UA 已脱敏)
    drogon::Task<ZMJSON> QueryShareLogs(const ZmShareLogQuery& q, int page, int size);
    /// @brief 某个分享的访问日志(创建者视角;IP/UA 已脱敏)
    drogon::Task<ZMJSON> QueryShareLogsByShare(int64_t shareId, int page, int size);

    /// @brief IP 脱敏(IPv4 保留前三段)
    static std::string MaskIp(const std::string& ip);
    /// @brief UA 摘要(截断到 24 字符)
    static std::string MaskUa(const std::string& ua);

  private:
    ZmFileDbModule* m_db = nullptr;
};

#endif // ZM_MODULE_FILE_AUDIT_H
