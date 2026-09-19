#ifndef ZM_MODULE_FILE_SHARE_H
#define ZM_MODULE_FILE_SHARE_H

// ============================================================================
// ZmFileShareModule:分享模块
// 职责:分享创建/修改/取消、提取码哈希与失败冷却、公开面访问(信息/提取码校验/
// 列目录/下载)、分享访问日志写入。
// token 为 32 字符高熵随机串:不承载任何 id、不参与可逆变换、校验时精确查库。
// 一条分享可绑定多个条目(share_nodes;单条分享也写一行,读路径统一):列表里
// 多根时按"任一根命中"校验子树,公开面顶层以虚拟根平铺各条目(见 ListDir)。
// 子树校验用递归 CTE(dir_id 必须能沿 parent_id 上溯到某个分享根),不做路径前缀比较。
// 部分条目被删除/进回收站时跳过该条,其余照常可用;全部不可用才判分享不可用。
// 提取码只存 HMAC-SHA256 哈希(ZM_FILE_HUB_HMAC_KEY),明文仅创建时下发一次。
// 失败冷却按 (IP, token) 二维计数,落在 write_limits 表 —— 只按 token 计会被人
// 连打 5 次错码锁死分享,构成拒绝服务。
// 与回收站的关系:目标在回收站期间不可用(404 SHARE_UNAVAILABLE),恢复后自动可用;
// 只有彻底删除才置失效。
// login_only 的分享:信息公开、列目录、下载三条路径都要先过"已登录"这道门 ——
// 只在信息公开那条拦,拿到 token 的人直接打列表/下载接口就能把内容读走。
// 免登录的凭证同理(提取码),两者的先后是"先登录、后提取码"。
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
 * @brief 创建分享(单条或多选)
 *
 * @param ctx 操作者
 * @param space 目标条目所在空间(仅作客户端说法,落库与"公共空间不提供仅登录可见"都以
 *              node_ids 实时解析出的空间为准)
 * @param nodeIds 分享目标(文件/目录;去重后须同一空间;无分享专属条数上限,仅受批量操作安全阈值约束)
 * @param pwdEnabled 是否设置提取码(服务端生成 4 位随机码)
 * @param expireDays 有效期天数(0 = 永久)
 * @param expireTime 自定义到期时刻(优先于 expireDays;0 = 不指定)
 * @param maxDownloads 下载次数上限(0 = 不限)
 * @param loginOnly 是否仅登录可见(公共空间的分享恒为否)
 * @param displayName 自定义展示名(空 = 沿用主条目名);只影响分享页标题与列表,不影响文件名
 * @param customPwd 自定义提取码(空 = 随机生成);须为 4 个可见字符
 * @return {share_id, token, pwd?, expire_time};失败 → {"error":{...}}
 */
    drogon::Task<ZMJSON> Create(const ZmOpCtx& ctx, int64_t space,
                                const std::vector<int64_t>& nodeIds, bool pwdEnabled,
                                int64_t expireDays, int64_t expireTime, int64_t maxDownloads,
                                bool loginOnly, const std::string& displayName = {},
                                const std::string& customPwd = {});

    /// @brief 我的分享列表(status ≤0 不过滤)
    /// @param space   空间(负数 = 不限)
    /// @param keyword 关键词(空 = 不限);匹配展示名
    /// @return {total, page, size, list}
    drogon::Task<ZMJSON> List(int64_t uid, int status, int64_t space, int page, int size,
                              const std::string& keyword = {});

    /// @brief 修改分享(有效期/次数上限/重置提取码/仅登录可见)
    /// @return {pwd?}(重置时一次性返回新明文)
    drogon::Task<ZMJSON> Patch(int64_t uid, int64_t shareId, const ZMJSON& body);

    /// @brief 取消分享(置已取消,立即不可访问;记录保留,可恢复)
    drogon::Task<ZMJSON> Cancel(int64_t uid, int64_t shareId, const ZmOpCtx& ctx);

    /// @brief 恢复被取消的分享(仅已取消状态;过期/达下载上限的不可恢复)
    drogon::Task<ZMJSON> Resume(int64_t uid, int64_t shareId, const ZmOpCtx& ctx);

    /**
     * @brief 彻底删除分享记录(任意状态均可删;记录从列表移除,链接立即失效)
     *
     * 与 Cancel 的区别:Cancel 只改状态、记录留在列表里且可恢复;本接口把行从库里删掉、
     * 不可恢复。访问日志(share_logs)保留,由 90 天保留期统一清理(§5.8)。
     *
     * @param uid          调用者(只能删自己的分享)
     * @param ids          指定要删的记录 id(空 = 配合 inactiveOnly 清空)
     * @param inactiveOnly true = 只清空该用户全部非有效记录(已取消 + 已失效)
     * @return {purged: 实际删除条数}
     */
    drogon::Task<ZMJSON> Purge(int64_t uid, const std::vector<int64_t>& ids, bool inactiveOnly,
                               const ZmOpCtx& ctx);

    /// @brief 分享访问日志(创建者视角;IP/UA 脱敏由审计模块完成)
    drogon::Task<ZMJSON> Logs(int64_t uid, int64_t shareId, int page, int size);

    // ── 公开面(免会话) ──
    /**
 * @brief 分享信息
 *
 * @param token 分享标识
 * @param credOk 提取码访问凭证是否有效(调用方校验 Cookie 后传入;有效则 need_pwd=false)
 * @param viewerUid 访问者登录 uid(0 = 免登录)
 * @param ip 客户端 IP;ua 客户端 UA
 * @return {name, node_type, multi, node_count, nodes[], hidden_count, need_pwd, expired,
 * expire_time, owner_uid, owner_name?, login_only, need_login, max_downloads, download_count}
 * nodes 只含当前可见的条目(取实时名称);需登录而未登录 → 401 + need_login=true
 */
    drogon::Task<ZMJSON> Info(const std::string& token, bool credOk, int64_t viewerUid,
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
 * 多条目分享:dir_id=0 返回虚拟根(平铺各条目),面包屑只有分享名一级;
 * 单条目分享:dir_id=0 即分享目标本身,行为与既往一致。
 *
 * @param token 分享标识
 * @param dirId 分享子树内的目录 id(0 = 分享目标本身 / 多条目虚拟根)
 * @param q 排序/分页/关键词(q.keyword 非空则走搜索:搜当前目录及子目录,找得到多深的都列出来)
 * @param credOk 访问凭证是否有效(调用方校验 Cookie 后传入)
 * @return {total, page, size, list, breadcrumb};分享/目标不可用 → {"error":{...}}
 */
    drogon::Task<ZMJSON> ListDir(const std::string& token, int64_t dirId, const ZmListQuery& q,
                                 bool credOk, int64_t viewerUid, const std::string& ip,
                                 const std::string& ua);

    /**
 * @brief 分享下载(单文件换令牌 / 多条目打包)
 *
 * @param token 分享标识;ids 待下载条目(须在任一分享根的子树内)
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
    /// @brief 一次公开面浏览的既定事实(门禁过后才成立,列目录与搜索共用)
    struct ViewCtx
    {
        ZMJSON               share;        ///< shares 行
        std::string          token;        ///< 分享标识(审计用)
        int64_t              shareId = 0;
        int64_t              space   = 0;
        std::vector<int64_t> roots;        ///< 可见分享根(顺序 = 绑定顺序)
        int64_t              rootId  = 0;  ///< 当前目录所属的分享根
        bool                 multi   = false;  ///< 多条目分享(顶层是虚拟根)
    };

    /// @brief 读取分享行(不存在返回空对象)
    ZMJSON LoadShareSync(int64_t shareId);
    /// @brief 分享绑定的根条目(实时读 nodes,附 visible 标记;旧行无关联时按 shares.node_id 兜底)
    /// @return [{id,type,name,size,ext,items,update_time,visible}]
    ZMJSON ShareRootsSync(const ZMJSON& share);
    /**
 * @brief 分享内的搜索(公开面)
 *
 * 结果以"分享内的位置"呈现:路径按分享根裁剪,免登录访客看不到分享者上层目录的名字。
 * 位置由两部分拼成 —— 面包屑给出的前缀(分享顶层 → 当前目录)+ 每条自己的所在目录。
 *
 * @param v     门禁后的浏览上下文
 * @param dirId 当前目录(0 = 分享顶层);它在虚拟根下时基准取各分享根
 * @param q     排序/分页/关键词(q.keyword 须非空)
 * @return 同 ListDir,另加 {keyword, truncated};搜索失败 → {"error":{...}}
 */
    drogon::Task<ZMJSON> SearchInShare(const ViewCtx& v, int64_t dirId, const ZmListQuery& q,
                                       int64_t viewerUid, const std::string& ip,
                                       const std::string& ua);
    /**
 * @brief 分享内的面包屑(分享顶层 → 当前目录)
 *
 * 多条目分享首级是分享名(虚拟根,id=0,点它回顶层);单条目分享首级就是分享根。
 * 列目录与搜索共用同一份,避免"面包屑"与"位置前缀"两处各算一遍还得对齐。
 *
 * @param v     门禁后的浏览上下文
 * @param dirId 当前目录 id(0 = 分享顶层本身:多条目分享的虚拟根 / 单条目分享的分享根)
 * @return [{id,name}]
 */
    drogon::Task<ZMJSON> BuildBreadcrumb(const ViewCtx& v, int64_t dirId);
    /// @brief 判定分享当前可用性(全部根条目不可见才判不可用)
    /// @return 0 = 可用;否则 HTTP 状态码
    int CheckUsableSync(const ZMJSON& share, std::string& code, std::string& message);
    /// @brief dir_id 是否位于任一根的子树内(递归 CTE 上溯;单根时行为不变)
    bool InShareTreeSync(int64_t dirId, const std::vector<int64_t>& roots);

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
