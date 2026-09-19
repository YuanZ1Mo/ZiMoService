#ifndef ZM_MODULE_FILE_DEFS_H
#define ZM_MODULE_FILE_DEFS_H

// ============================================================================
// 文件中心公共定义:常量 / 错误码 / 枚举 / 行取值助手 / 共用小工具
// 所有模块共用;只放"多处引用且必须一致"的东西,避免各自实现发散。
// ============================================================================

#include <zm_util_json.h>

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// ── 容量与阈值 ──
namespace zm_file
{
inline constexpr int64_t kChunkSize         = 8LL * 1024 * 1024;         ///< 分片大小 8MB
inline constexpr int64_t kSimpleUploadMax   = 8LL * 1024 * 1024;         ///< 单请求上传阈值
inline constexpr int64_t kMaxFileSize       = 20LL * 1024 * 1024 * 1024; ///< 单文件上限 20GB
inline constexpr int64_t kMaxChunks         = 2560;                      ///< 分片总数上限
inline constexpr int64_t kUploadTtlSec      = 24LL * 3600;               ///< 上传会话有效期
inline constexpr int64_t kTokenTtlSec       = 600;                       ///< 下载令牌 10 分钟
inline constexpr int     kTokenMaxConn      = 5;    ///< 同令牌并发连接上限
inline constexpr int     kGlobalDownloadMax = 100;  ///< 全局并发直链下载上限
inline constexpr int64_t kMaxPackItems      = 5000; ///< 单次打包条目上限
inline constexpr int64_t kMaxPackBytes      = 20LL * 1024 * 1024 * 1024; ///< 单次打包字节上限
inline constexpr int64_t kCopyAsyncItems    = 500;                    ///< 复制转异步的条目阈值
inline constexpr int64_t kCopyAsyncBytes  = 1LL * 1024 * 1024 * 1024; ///< 复制转异步的字节阈值
inline constexpr int64_t kBatchMaxIds     = 2000;                     ///< 单次批量操作条目上限
inline constexpr int64_t kSearchLimit     = 500;                      ///< 搜索结果上限
inline constexpr int64_t kSearchKwMax     = 64;                       ///< 关键词长度上限
inline constexpr int64_t kNameMaxBytes    = 255;                      ///< 名称字节上限
inline constexpr int64_t kEnsurePathMax   = 1024;        ///< ensure 单条相对路径长度上限
inline constexpr int64_t kMaxPathChars    = 240;         ///< 物理路径字符上限
inline constexpr int64_t kMaxDepth        = 32;          ///< 路径深度上限
inline constexpr int64_t kTrashRetainDays = 30;          ///< 回收站保留天数
// 打包压缩包是缓存:最后一次访问后空闲即回收(下载会把时间推到现在,相当于续期),
// 另有 24 小时兜底与容量阈值。产物可能比任务记录(终态 7 天)先被回收,此时历史里
// 仍显示"已完成"、点下载会提示已被清理,需重新打包 —— 这是缓存定位下的预期行为
inline constexpr int64_t kCacheIdleSec    = 3LL * 3600;  ///< 压缩包空闲清理阈值(秒;从最后一次访问起算)
inline constexpr int64_t kCacheKeepSec    = 24LL * 3600; ///< 压缩包兜底清理阈值(秒)
inline constexpr int64_t kCacheChunkKeepSec = 24LL * 3600; ///< 上传分片临时目录兜底清理阈值(秒;与打包无关,仍按天级回收)
inline constexpr int64_t kCacheQuotaBytes = 500LL * 1024 * 1024 * 1024; ///< 缓存区总占用阈值(超阈值按最久未访问删到 80%)
inline constexpr int64_t kShareMaxPerUser = 200;         ///< 单用户有效分享上限
inline constexpr int64_t kShareCredTtlSec = 2LL * 3600;  ///< 分享访问凭证有效期
inline constexpr int64_t kSharePwdFailMax = 5;           ///< 提取码连续失败上限
inline constexpr int64_t kSharePwdCoolSec = 600;         ///< 提取码失败冷却时长
inline constexpr int64_t kUploadMaxRunning = 3;          ///< 单用户并发上传任务上限
inline constexpr int64_t kPackMaxRunning  = 2;           ///< 单用户进行中打包任务上限

// ── 条目类型 ──
inline constexpr int kTypeDir  = 1; ///< 目录
inline constexpr int kTypeFile = 2; ///< 文件

// ── 任务类型 ──
inline constexpr int kTaskUpload     = 1;
inline constexpr int kTaskCopy       = 2;
inline constexpr int kTaskPack       = 3;
inline constexpr int kTaskStat       = 4;
inline constexpr int kTaskSync       = 5;
inline constexpr int kTaskTrashClear = 6;

// ── 任务状态 ──
inline constexpr int kTaskQueued      = 1;
inline constexpr int kTaskRunning     = 2;
inline constexpr int kTaskDone        = 3;
inline constexpr int kTaskFailed      = 4;
inline constexpr int kTaskCanceled    = 5;
inline constexpr int kTaskInterrupted = 6;

// ── 上传会话状态 ──
inline constexpr int kUploadRunning  = 1;
inline constexpr int kUploadDone     = 2;
inline constexpr int kUploadCanceled = 3;
inline constexpr int kUploadExpired  = 4;

// ── 分享状态 ──
inline constexpr int kShareActive   = 1;
inline constexpr int kShareCanceled = 2;
inline constexpr int kShareInvalid  = 3;

// ── 审计动作(写入 file_logs.action) ──
inline constexpr const char* kActUpload      = "upload";
inline constexpr const char* kActDownload    = "download";
inline constexpr const char* kActPack        = "pack";
inline constexpr const char* kActMkdir       = "mkdir";
inline constexpr const char* kActRename      = "rename";
inline constexpr const char* kActMove        = "move";
inline constexpr const char* kActCopy        = "copy";
inline constexpr const char* kActDelete      = "delete";
inline constexpr const char* kActRestore     = "restore";
inline constexpr const char* kActPurge       = "purge";
inline constexpr const char* kActTrashClear  = "trash_clear";
inline constexpr const char* kActShareCreate = "share_create";
inline constexpr const char* kActShareCancel = "share_cancel";
inline constexpr const char* kActShareResume = "share_resume";
inline constexpr const char* kActSharePurge  = "share_purge";
inline constexpr const char* kActAdminSync   = "admin_sync";
inline constexpr const char* kActAdminCache  = "admin_cache_clean";

// ── 分享访问日志动作 ──
inline constexpr int kShareLogView     = 1; ///< 访问分享页
inline constexpr int kShareLogVerify   = 2; ///< 提取码校验
inline constexpr int kShareLogList     = 3; ///< 列目录
inline constexpr int kShareLogDownload = 4; ///< 下载单文件
inline constexpr int kShareLogPack     = 5; ///< 打包下载
} // namespace zm_file

// ── 业务错误码(HTTP 状态码区分大类) ──
namespace zm_file_err
{
inline constexpr const char* kBadRequest        = "BAD_REQUEST";
inline constexpr const char* kNameInvalid       = "NAME_INVALID";
inline constexpr const char* kPathTooDeep       = "PATH_TOO_DEEP";
inline constexpr const char* kPathTooLong       = "PATH_TOO_LONG";
inline constexpr const char* kMoveIntoSelf      = "MOVE_INTO_SELF";
inline constexpr const char* kPackTooLarge      = "PACK_TOO_LARGE";
inline constexpr const char* kBatchTooLarge     = "BATCH_TOO_LARGE";
inline constexpr const char* kChunkSizeMismatch = "CHUNK_SIZE_MISMATCH";
inline constexpr const char* kHashMismatch      = "HASH_MISMATCH";
inline constexpr const char* kPermDenied        = "PERM_DENIED";
inline constexpr const char* kQuotaExceeded     = "QUOTA_EXCEEDED";
inline constexpr const char* kNodeNotFound      = "NODE_NOT_FOUND";
inline constexpr const char* kDirNotFound       = "DIR_NOT_FOUND";
inline constexpr const char* kFileNotFound      = "FILE_NOT_FOUND";
inline constexpr const char* kTrashItemNotFound = "TRASH_ITEM_NOT_FOUND";
inline constexpr const char* kUploadNotFound    = "UPLOAD_NOT_FOUND";
inline constexpr const char* kShareNotFound     = "SHARE_NOT_FOUND";
inline constexpr const char* kShareUnavailable  = "SHARE_UNAVAILABLE";
inline constexpr const char* kNameExists        = "NAME_EXISTS";
inline constexpr const char* kFileLocked        = "FILE_LOCKED";
inline constexpr const char* kSyncRunning       = "SYNC_RUNNING";
inline constexpr const char* kShareExpired      = "SHARE_EXPIRED";
inline constexpr const char* kTokenExpired      = "TOKEN_EXPIRED";
inline constexpr const char* kFileTooLarge      = "FILE_TOO_LARGE";
inline constexpr const char* kTooManyUploads    = "TOO_MANY_UPLOADS";
inline constexpr const char* kTooManyShares     = "TOO_MANY_SHARES";
inline constexpr const char* kTooManyPacks      = "TOO_MANY_PACKS";
inline constexpr const char* kShareLocked       = "SHARE_LOCKED";
inline constexpr const char* kInternal          = "INTERNAL";
} // namespace zm_file_err

// ── 冲突策略 ──
namespace zm_file_conflict
{
inline constexpr const char* kAsk       = "ask";
inline constexpr const char* kSkip      = "skip";
inline constexpr const char* kRename    = "rename";
inline constexpr const char* kOverwrite = "overwrite";
} // namespace zm_file_conflict

// ============================================================================
// 行取值助手(查询结果行为 JSON 对象;字段缺失/类型不符时回落默认值)
// ============================================================================
/**
 * @brief 取行内的 64 位整数
 *
 * @param row 结果行
 * @param key 字段名
 * @param defaultValue 缺失或非数值时的回落值
 * @return 字段值
 */
inline int64_t zm_file_row_int(const ZMJSON& row, const char* key, int64_t defaultValue = 0)
{
    if (!row.is_object() || !row.contains(key))
        return defaultValue;
    const ZMJSON& v = row[key];
    if (v.is_number_integer() || v.is_number_unsigned())
        return v.get<int64_t>();
    if (v.is_number_float())
        return static_cast<int64_t>(v.get<double>());
    if (v.is_string())
    {
        try
        {
            return std::stoll(v.get<std::string>());
        }
        catch (...)
        {
            return defaultValue;
        }
    }
    return defaultValue;
}

/**
 * @brief 取行内的字符串(NULL/缺失 → 空串)
 *
 * @param row 结果行
 * @param key 字段名
 * @return 字段值
 */
inline std::string zm_file_row_str(const ZMJSON& row, const char* key)
{
    if (!row.is_object() || !row.contains(key))
        return "";
    const ZMJSON& v = row[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_null())
        return "";
    return v.dump();
}

// ============================================================================
// 跨模块结果约定
// 业务模块返回的 JSON 若是 {"error":{code,status,message}} 形态,表示操作失败,
// 由编排层(ZmFileHubModule / ZmFileAdminModule)翻译为 HTTP 错误响应;
// 其余情况一律视为成功数据。
// ============================================================================
/**
 * @brief 构造业务失败结果
 *
 * @param code 错误码(见 zm_file_err)
 * @param status 建议的 HTTP 状态码
 * @param message 可展示文案
 * @return {"error":{code,status,message}}
 */
inline ZMJSON ZmFileError(const char* code, int status, const std::string& message)
{
    ZMJSON j              = ZMJSON::object();
    j["error"]            = ZMJSON::object();
    j["error"]["code"]    = code;
    j["error"]["status"]  = status;
    j["error"]["message"] = message;
    return j;
}

/**
 * @brief 构造带附加数据的业务失败结果
 *
 * extra 的字段会并到 error 对象里(如冲突清单 conflicts),编排层再原样展开到响应体
 * 顶层,前端从 error.data.<字段> 读取。
 *
 * @param code 错误码
 * @param status 建议 HTTP 状态码
 * @param message 可展示文案
 * @param extra 附加字段({"conflicts":[...]} 之类)
 * @return 失败结果
 */
inline ZMJSON ZmFileErrorExtra(const char* code, int status, const std::string& message,
                               const ZMJSON& extra)
{
    ZMJSON j = ZmFileError(code, status, message);
    if (extra.is_object())
    {
        for (auto it = extra.begin(); it != extra.end(); ++it)
            j["error"][it.key()] = it.value();
    }
    return j;
}

/// @return 是否为业务失败结果
inline bool ZmFileHasError(const ZMJSON& j)
{
    return j.is_object() && j.contains("error") && j["error"].is_object();
}

/// @return 失败结果的建议 HTTP 状态码(缺省 400)
inline int ZmFileErrorStatus(const ZMJSON& j, int def = 400)
{
    if (!ZmFileHasError(j))
        return def;
    return static_cast<int>(zm_file_row_int(j["error"], "status", def));
}

/// @return 失败结果的错误码
inline std::string ZmFileErrorCode(const ZMJSON& j)
{
    return ZmFileHasError(j) ? zm_file_row_str(j["error"], "code") : "";
}

/// @return 失败结果的文案
inline std::string ZmFileErrorMessage(const ZMJSON& j)
{
    return ZmFileHasError(j) ? zm_file_row_str(j["error"], "message") : "";
}

// ── 共用小工具 ──

/// @return 去掉首尾空白的副本
inline std::string ZmTrimSpaces(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

/**
 * @brief 给 WHERE 追加"关键词模糊匹配"条件(文件中心各列表搜索共用)
 *
 * 口径统一在这里,免得各处各写一份而后漂移。三件事固定:去首尾空白;
 * 空关键词不加条件;超长(> kSearchKwMax)一律视为"搜不到" —— 追加的是 `AND 0`
 * 而不是提前返回,因为列表的总数/占用统计要照常算,否则前端会跟着显示成 0。
 *
 * @param where [in,out] WHERE 子句
 * @param p     [in,out] 参数表(条件从 p.size()+1 起编号追加)
 * @param kw    关键词
 * @param cols  参与匹配的列,多个之间为 OR(如 {"name"} 或 {"name", "task_no"})
 * @return true = 追加了条件;false = 关键词为空(where 未被改动)
 *
 * @example
 *   ZmAddKeywordCond(where, p, q.keyword, {"name"});
 */
inline bool ZmAddKeywordCond(std::string& where, std::vector<std::string>& p,
                             const std::string& kw, const std::vector<const char*>& cols)
{
    std::string k = ZmTrimSpaces(kw);
    if (k.empty())
        return false;
    if (k.size() > static_cast<size_t>(zm_file::kSearchKwMax))
    {
        where += " AND 0";
        return true;
    }
    where += " AND (";
    for (size_t i = 0; i < cols.size(); ++i)
    {
        if (i)
            where += " OR ";
        where += std::string(cols[i]) + " LIKE ?" + std::to_string(p.size() + 1);
        p.push_back("%" + k + "%");
    }
    where += ")";
    return true;
}

#endif // ZM_MODULE_FILE_DEFS_H
