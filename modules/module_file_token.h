#ifndef ZM_MODULE_FILE_TOKEN_H
#define ZM_MODULE_FILE_TOKEN_H

// ============================================================================
// ZmFileTokenModule:下载令牌模块
// 职责:短时下载令牌的签发与校验(32 字符随机、10 分钟、绑定目标)、
// 并发闸门(同令牌 ≤5 连接、全局 ≤100)、打包产物令牌。
// 令牌**不落库**:10 分钟的生命周期不值得为它建表 + 加清理任务;
// 存储抽成接口只为将来多进程部署换 Redis,本期只有进程内实现。
// 直链路径免会话:令牌本身即凭证,校验时**复查可见树**(目标被删 → 404)。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace trantor
{
class TcpConnection;
}

class ZmFileStoreModule;
class ZmFileNodeModule;
class ZmFileTaskModule;

/// 令牌类型
enum class ZmTokenKind : int
{
    Node  = 1, ///< 登录用户的条目下载
    Pack  = 2, ///< 打包产物(task_no → 缓存压缩包)
    Share = 3, ///< 分享目标(绑定 share_token)
};

/// 令牌解析结果
struct ZmTokenTarget
{
    bool        ok     = false;
    int         status = 0; ///< ok=false 时的 HTTP 状态码
    std::string code;       ///< ok=false 时的错误码
    std::string message;    ///< ok=false 时的文案
    ZmTokenKind kind = ZmTokenKind::Node;
    std::string path; ///< 物理路径
    std::string name; ///< 下载文件名
    int64_t     nodeId = 0;
    int64_t     space  = 0;
    int64_t     uid    = 0; ///< 签发者(分享令牌为 0)
    std::string shareToken; ///< 分享标识(仅 Share)
    std::string taskNo;     ///< 任务号(仅 Pack)
    int64_t     fileSize = 0;
    uint64_t    slotId   = 0; ///< 本次占用的名额句柄(归还时用;0 = 未占用)
};

class ZmFileTokenModule
{
  public:
    ZmFileTokenModule(ZmFileStoreModule* store, ZmFileNodeModule* node,
                      ZmFileTaskModule* task);
    ~ZmFileTokenModule();

    // ── 签发 ──
    /// @brief 签发条目下载令牌(登录用户)
    /// @param uid 签发者;nodeId 条目 id
    /// @return {token, expire_time, name};失败 → {"error":{...}}
    drogon::Task<ZMJSON> IssueNode(int64_t uid, int64_t nodeId);

    /// @brief 签发分享目标下载令牌
    /// @param shareToken 分享标识;nodeId 分享子树内的条目
    /// @return {token, expire_time, name};失败 → {"error":{...}}
    drogon::Task<ZMJSON> IssueShare(const std::string& shareToken, int64_t nodeId);

    /// @brief 签发打包产物下载令牌
    /// @param taskNo 打包任务号(其 result 记录压缩包相对路径)
    /// @return {token, expire_time, name};失败 → {"error":{...}}
    drogon::Task<ZMJSON> IssuePack(int64_t uid, const std::string& taskNo);

    // ── 校验 ──
    /**
 * @brief 解析令牌:有效期 + 并发闸门 + 目标复查(可见树 / 文件存在)
 *
 * 成功即占用一个连接名额(结果里的 slotId 即该名额句柄),调用方**必须**在
 * 响应构造完成后调用 ReleaseSlot(slotId) 归还。
 *
 * @param token 令牌串
 * @return 目标信息;失败经 status/code/message 表达
 */
    drogon::Task<ZmTokenTarget> Resolve(const std::string& token);

    /// @brief 登记在途传输:把连接弱引用挂到名额上(供巡检回收)
    ///
    /// 正常路径靠"发送结束回调"归还名额;但若客户端在响应开始发送前就断开,
    /// 回调不会被触发(框架在发送阶段才造流),名额会漏。这里补上连接弱引用,
    /// 由 SweepLeaks 按"连接是否还在"兜底回收。
    ///
    /// @param slotId 名额句柄(Resolve 返回)
    /// @param conn   连接弱引用(跨事件循环只判存活,不读其状态)
    void TrackTransfer(uint64_t slotId, const std::weak_ptr<trantor::TcpConnection>& conn);

    /// @brief 归还一个在途名额(幂等:同一句柄重复调用只归还一次)
    /// @param slotId 名额句柄
    void ReleaseSlot(uint64_t slotId);

    /// @brief 回收泄漏的名额(周期调用)
    ///
    /// 判定只看"连接是否已消失":连接还在、传输还在跑,名额就该继续占着 ——
    /// 大文件传输会远超令牌有效期,按时间判会把在途名额提前放掉。
    /// 连接弱引用为空的登记(Resolve 之后、进入发送之前)给一段宽限期。
    ///
    /// @param now 当前 unix 秒
    void SweepLeaks(int64_t now);

    /// @brief 吊销某打包任务的全部令牌(压缩包被清理时)
    void RevokeByPack(const std::string& taskNo);

    /// @brief 拼直链地址
    /// @param base 站点基址(如 https://host:39441)
    /// @param token 令牌
    /// @param name 文件名(内部做百分号编码)
    /// @return 形如 <base>/zimo/api/filehub/dl/<token>/<encoded>
    static std::string BuildUrl(const std::string& base, const std::string& token,
                                const std::string& name);

    /// @brief URL 百分号编码(UTF-8 字节级)
    static std::string UrlEncode(const std::string& s);
    /// @brief URL 百分号解码
    static std::string UrlDecode(const std::string& s);

  private:
    /// 令牌记录
    struct Rec
    {
        ZmTokenKind kind   = ZmTokenKind::Node;
        int64_t     uid    = 0;
        int64_t     nodeId = 0;
        std::string shareToken;
        std::string path;   ///< Pack:压缩包物理路径
        std::string name;   ///< 下载文件名
        std::string taskNo; ///< Pack:任务号
        int64_t     expire = 0;
        int         conns  = 0; ///< 当前并发连接数
    };

    /// @return 新令牌串(32 字符十六进制)
    static std::string NewToken();
    /// @brief 占用名额(令牌级 ≤5、全局 ≤100),成功时登记一个在途名额
    /// @param token 令牌(名额归还时按它回落令牌级计数)
    /// @param rec 令牌记录
    /// @param slotOut [out] 成功时填名额句柄
    /// @return 0 = 成功;否则 HTTP 状态码
    int AcquireSlot(const std::string& token, Rec& rec, uint64_t& slotOut);
    /// @brief 查找记录(不加名额);不存在返回 false
    bool Find(const std::string& token, Rec& out);
    /// @brief 定向找令牌名(用于按 taskNo 吊销)
    void EraseLocked(const std::string& token);
    /// @brief 归还名额实现(调用方须持 m_mtx)
    void ReleaseSlotLocked(uint64_t slotId);

    ZmFileStoreModule* m_store = nullptr;
    ZmFileNodeModule*  m_node  = nullptr;
    ZmFileTaskModule*  m_task  = nullptr;

    /// 在途名额:一个下载请求一条,一一对应归还(旧实现按令牌登记,
    /// 同一令牌的多条连接会互相覆盖,导致除首条外的名额永不归还)
    struct Slot
    {
        std::string                           token;
        std::weak_ptr<trantor::TcpConnection> conn;
        int64_t                               issuedAt = 0;
    };

    std::mutex                           m_mtx;
    std::unordered_map<std::string, Rec> m_tokens;
    std::unordered_map<uint64_t, Slot>   m_slots;
    uint64_t                             m_slotSeq = 0; ///< 名额句柄发号
    int                                  m_activeDownloads = 0; ///< 全局在服连接数
};

#endif // ZM_MODULE_FILE_TOKEN_H
