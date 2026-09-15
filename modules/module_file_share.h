#ifndef ZM_MODULE_FILE_SHARE_H
#define ZM_MODULE_FILE_SHARE_H

// ============================================================================
// ZmFileShareModule:分享模块
// 职责:分享创建/修改/取消、提取码哈希与失败冷却、公开面访问(信息/提取码校验/
// 列目录/下载)、分享访问日志写入。
// token 为 32 字符高熵随机串:不承载任何 id、不参与可逆变换、校验时精确查库。
// 子树校验用递归 CTE(dir_id 必须能沿 parent_id 上溯到分享目标),不做路径前缀比较。
// 提取码只存 HMAC-SHA256 哈希(ZM_FILE_HUB_HMAC_KEY),明文仅创建时下发一次。
// 失败冷却按 (IP, token) 二维计数,落在 write_limits 表 —— 只按 token 计会被人
// 连打 5 次错码锁死分享,构成拒绝服务。
// 与回收站的关系:目标在回收站期间不可用(404 SHARE_UNAVAILABLE),恢复后自动可用;
// 只有彻底删除才置失效。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "modules/module_file_node.h" // ZmListQuery / ZmOpCtx

class ZmFileDbModule;
class ZmFileNodeModule;
class ZmFileStoreModule;
class ZmFileAuditModule;
class ZmFileTokenModule;
class ZmFilePackModule;

class ZmFileShareModule
{
  public:
    ZmFileShareModule(ZmFileDbModule* db, ZmFileNodeModule* node, ZmFileStoreModule* store,
                      ZmFileAuditModule* audit, ZmFileTokenModule* token,
                      ZmFilePackModule* pack);
    ~ZmFileShareModule();

    // ── 登录侧 ──
    /**
 * @brief 创建分享
 *
 * @param ctx 操作者
 * @param space 目标条目所在空间(服务端按 node_id 实际空间覆盖)
 * @param nodeId 分享目标(文件或目录)
 * @param pwdEnabled 是否设置提取码(服务端生成 4 位随机码)
 * @param expireDays 有效期天数(0 = 永久)
 * @param expireTime 自定义到期时刻(优先于 expireDays;0 = 不指定)
 * @param maxDownloads 下载次数上限(0 = 不限)
 * @param loginOnly 是否仅登录可见
 * @return {share_id, token, pwd?, expire_time};失败 → {"error":{...}}
 */
    drogon::Task<ZMJSON> Create(const ZmOpCtx& ctx, int64_t space, int64_t nodeId,
                                bool pwdEnabled, int64_t expireDays, int64_t expireTime,
                                int64_t maxDownloads, bool loginOnly);

    /// @brief 我的分享列表(status ≤0 不过滤)
    /// @return {total, page, size, list}
    drogon::Task<ZMJSON> List(int64_t uid, int status, int page, int size);

    /// @brief 修改分享(有效期/次数上限/重置提取码/仅登录可见)
    /// @return {pwd?}(重置时一次性返回新明文)
    drogon::Task<ZMJSON> Patch(int64_t uid, int64_t shareId, const ZMJSON& body);

    /// @brief 取消分享(置已取消,立即不可访问)
    drogon::Task<ZMJSON> Cancel(int64_t uid, int64_t shareId, const ZmOpCtx& ctx);

    /// @brief 分享访问日志(创建者视角;IP/UA 脱敏由审计模块完成)
    drogon::Task<ZMJSON> Logs(int64_t uid, int64_t shareId, int page, int size);

    // ── 公开面(免会话) ──
    /**
 * @brief 分享信息
 *
 * @param token 分享标识
 * @param viewerUid 访问者登录 uid(0 = 免登录)
 * @param ip 客户端 IP;ua 客户端 UA
 * @return {name, node_type, need_pwd, expired, expire_time, owner_uid, owner_name?,
 * login_only, need_login, max_downloads, download_count}
 * 需要登录而未登录 → 401 + need_login=true
 */
    drogon::Task<ZMJSON> Info(const std::string& token, int64_t viewerUid,
                              const std::string& ip, const std::string& ua);

    /**
 * @brief 校验提取码 → 签发访问凭证(调用方写入 Cookie zm_share,2 小时)
 *
 * @param token 分享标识;pwd 明文提取码;ip 客户端 IP;ua 客户端 UA
 * @return {pass:true, cred} 或 {pass:false};冷却中 → 429 SHARE_LOCKED
 */
    drogon::Task<ZMJSON> Verify(const std::string& token, const std::string& pwd,
                                const std::string& ip, const std::string& ua);

    /**
 * @brief 列分享目录
 *
 * @param token 分享标识
 * @param dirId 分享子树内的目录 id(0 = 分享目标本身)
 * @param q 排序/分页
 * @param credOk 访问凭证是否有效(调用方校验 Cookie 后传入)
 * @return {total, page, size, list, breadcrumb};分享/目标不可用 → {"error":{...}}
 */
    drogon::Task<ZMJSON> ListDir(const std::string& token, int64_t dirId, const ZmListQuery& q,
                                 bool credOk, int64_t viewerUid, const std::string& ip,
                                 const std::string& ua);

    /**
 * @brief 分享下载(单文件换令牌 / 多条目打包)
 *
 * @param token 分享标识;ids 待下载条目(须在分享子树内)
 * @param credOk 访问凭证是否有效
 * @param ctx 操作者(免登录时 uid=0)
 * @return 单文件 {url, expire_time};打包 {task_no}(幂等,就绪后返回 url)
 */
    drogon::Task<ZMJSON> Download(const std::string& token, const std::vector<int64_t>& ids,
                                  bool credOk, int64_t viewerUid, const std::string& ip,
                                  const std::string& ua);

    /// @brief 校验访问凭证(Cookie zm_share 的值是否与内存凭证一致且未过期)
    bool CreditValid(const std::string& token, const std::string& cred);

    /// @brief 生成 4 位提取码(去掉易混淆字符)
    static std::string GeneratePwd();

    /// @brief 提取码哈希(HMAC-SHA256,十六进制小写)
    static std::string HashPwd(const std::string& token, const std::string& pwd);

    /// @brief 分享链接地址(base + /s/<token>)
    static std::string BuildShareUrl(const std::string& base, const std::string& token);

  private:
    /// @brief 读取分享行(不存在返回空对象)
    ZMJSON LoadShareSync(int64_t shareId);
    /// @brief 判定分享当前可用性
    /// @return 0 = 可用;否则 HTTP 状态码
    int CheckUsableSync(const ZMJSON& share, std::string& code, std::string& message);
    /// @brief dir_id 是否位于分享子树内(递归 CTE 上溯)
    bool InShareTreeSync(int64_t dirId, int64_t shareRootId);

    ZmFileDbModule*    m_db    = nullptr;
    ZmFileNodeModule*  m_node  = nullptr;
    ZmFileStoreModule* m_store = nullptr;
    ZmFileAuditModule* m_audit = nullptr;
    ZmFileTokenModule* m_token = nullptr;
    ZmFilePackModule*  m_pack  = nullptr;

    /// 访问凭证(进程内;键 = 分享 token)
    struct Cred
    {
        std::string value;
        int64_t     expire = 0;
    };
    std::mutex                            m_credMtx;
    std::unordered_map<std::string, Cred> m_creds;

    /// 公开面打包去重:分享 token + 条目集合 → 任务号(幂等重试的依据)
    std::mutex                                   m_packMtx;
    std::unordered_map<std::string, std::string> m_packKeys;
};

#endif // ZM_MODULE_FILE_SHARE_H
