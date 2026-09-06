#ifndef ZM_MODULE_SESSION_H
#define ZM_MODULE_SESSION_H

// ============================================================================
// ZmSessionModule:会话模块(设计文档 §3.4)
//  数据归属:sessions。签发(SHA-256 落库 + cookie zm_session)、校验、续期
//  (滑动 30 天 + 绝对 90 天;续期写库节流 ≥5min,10s 心跳"只读不写")、
//  LRU 上限(每账号 5,踢 last_active 最旧)、吊销。
//  cookie:HttpOnly + SameSite=Lax + Secure(HTTPS 面) + Max-Age(与绝对过期对齐)。
// ============================================================================

#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

class ZmDbModule;
namespace drogon
{
class HttpResponse;
using HttpResponsePtr = std::shared_ptr<HttpResponse>;
}

/// 会话上下文(门禁与业务 handler 取用)
struct ZmSessionCtx
{
    bool valid = false;
    int64_t uid = 0;
    std::string account;
    std::string nickname;
    std::string roleCode;
    int level = 0;
    int forceChange = 0;
    int status = 0;
    int deleted = 0;
    std::string tokenHash;
    std::string ip;

    ZMJSON ToJson() const
    {
        ZMJSON j = ZMJSON::object();
        j["uid"] = uid;
        j["account"] = account;
        j["nickname"] = nickname;
        j["roleCode"] = roleCode;
        j["level"] = level;
        j["forceChange"] = forceChange;
        j["status"] = status;
        j["deleted"] = deleted;
        return j;
    }
};

class ZmSessionModule
{
public:
    explicit ZmSessionModule(ZmDbModule* db);
    ~ZmSessionModule();

    /// 签发新会话:生成 32B 随机明文 token → SHA-256 落库 → LRU 清理 →
    /// Set-Cookie(zm_session,HttpOnly/SameSite=Lax/Secure/Max-Age)。返回明文 token。
    /// @param secure HTTPS 面为 true(cookie 加 Secure)
    drogon::Task<std::string> Issue(int64_t uid, const std::string& ip,
                                    const std::string& ua, bool secure,
                                    const drogon::HttpResponsePtr& resp);
    /// 校验 + 续期(双上限 + 写库节流);无效返回 valid=false
    drogon::Task<ZmSessionCtx> AuthAndTouch(const std::string& cookieValue,
                                            const std::string& ip);
    /// 吊销单会话
    drogon::Task<bool> Revoke(const std::string& cookieValue);
    /// 吊销某用户全部会话(停用/强制重置用);返回吊销数
    drogon::Task<int> RevokeAllByUid(int64_t uid);

    // ── 原语 ──
    static std::string Sha256Hex(const std::string& data);
    static std::string RandomToken();
    static const char* CookieName() { return "zm_session"; }
    /// 会话时长常量
    static int64_t SlidingExpireSec() { return 30LL * 86400; }
    static int64_t AbsoluteExpireSec() { return 90LL * 86400; }

private:
    void DropCache(const std::string& tokenHash);
    void DropCacheByUid(int64_t uid);

    ZmDbModule* m_db = nullptr;
    // 续期写库节流:token_hash → 最近落库时间(unix 秒)
    std::mutex m_cacheMtx;
    std::unordered_map<std::string, int64_t> m_lastWrite;
};

#endif // ZM_MODULE_SESSION_H
