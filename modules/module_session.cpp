#include "modules/module_session.h"

#include "modules/module_db.h"

#include <drogon/Cookie.h>
#include <drogon/HttpResponse.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <zm_util_logger.h>

#include <array>

namespace
{
std::string ToHexLower(const unsigned char* p, size_t len)
{
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        s += hex[p[i] >> 4];
        s += hex[p[i] & 0x0F];
    }
    return s;
}
}  // namespace

// ============================================================================
// 构造
// ============================================================================
ZmSessionModule::ZmSessionModule(ZmDbModule* db)
    : m_db(db)
{
}

ZmSessionModule::~ZmSessionModule() = default;

// ============================================================================
// 原语
// ============================================================================
std::string ZmSessionModule::Sha256Hex(const std::string& data)
{
    std::array<unsigned char, 32> md{};
    unsigned int mdLen = 0;
    EVP_Digest(data.data(), data.size(), md.data(), &mdLen, EVP_sha256(), nullptr);
    return ToHexLower(md.data(), mdLen);
}

std::string ZmSessionModule::RandomToken()
{
    // 32B 随机明文 token(hex 64 字符;落库存 SHA-256)
    std::array<unsigned char, 32> buf{};
    RAND_bytes(buf.data(), static_cast<int>(buf.size()));
    return ToHexLower(buf.data(), buf.size());
}

// ============================================================================
// 签发
// ============================================================================
drogon::Task<std::string> ZmSessionModule::Issue(int64_t uid, const std::string& ip,
                                                 const std::string& ua, bool secure,
                                                 const drogon::HttpResponsePtr& resp)
{
    std::string token = RandomToken();
    std::string tokenHash = Sha256Hex(token);
    int64_t now = ZmDbModule::Now();
    int64_t sliding = now + SlidingExpireSec();
    int64_t absolute = now + AbsoluteExpireSec();

    bool ok = co_await m_db->WithTx([&](ZmDbModule& db) -> bool {
        // 插入新会话
        if (!db.ExecSync(
                "INSERT INTO sessions(token_hash, uid, create_ip, ua, create_time, "
                "last_active, expire_time, absolute_expire) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                {tokenHash, std::to_string(uid), ip, ua, std::to_string(now),
                 std::to_string(now), std::to_string(sliding), std::to_string(absolute)}))
            return false;
        // LRU 上限:每账号 5,踢 last_active 最旧
        auto row = db.QueryRowSync(
            "SELECT last_active FROM sessions WHERE uid=?1 "
            "ORDER BY last_active DESC LIMIT 1 OFFSET 4;",
            {std::to_string(uid)});
        if (zm_json_has(row, "last_active"))
        {
            std::string cutoff = std::to_string(zm_json_get_int(row, "last_active", 0));
            if (!db.ExecSync(
                    "DELETE FROM sessions WHERE uid=?1 AND last_active < ?2;",
                    {std::to_string(uid), cutoff}))
                return false;
        }
        return true;
    });
    if (!ok)
    {
        DEFAULT_LOG_WARN("ZmSessionModule::Issue 失败 uid={}", uid);
        co_return "";
    }

    // Set-Cookie(与绝对过期对齐)
    drogon::Cookie c(CookieName(), token);
    c.setHttpOnly(true);
    c.setSameSite(drogon::Cookie::SameSite::kLax);
    if (secure)
        c.setSecure(true);
    c.setMaxAge(static_cast<int>(AbsoluteExpireSec()));
    c.setPath("/");
    if (resp)
        resp->addCookie(c);
    co_return token;
}

// ============================================================================
// 校验 + 续期
// ============================================================================
drogon::Task<ZmSessionCtx> ZmSessionModule::AuthAndTouch(const std::string& cookieValue,
                                                         const std::string& ip)
{
    ZmSessionCtx ctx;
    if (cookieValue.empty())
        co_return ctx;
    std::string tokenHash = Sha256Hex(cookieValue);
    auto row = co_await m_db->QueryRow(
        "SELECT s.uid, s.last_active, s.expire_time, s.absolute_expire, "
        "u.account, u.nickname, u.role_code, u.force_change, u.status, u.deleted, "
        "COALESCE(r.level,0) AS level "
        "FROM sessions s JOIN users u ON s.uid=u.uid "
        "LEFT JOIN roles r ON u.role_code=r.code "
        "WHERE s.token_hash=?1;",
        {tokenHash});
    if (!zm_json_has(row, "uid"))
    {
        DropCache(tokenHash);
        co_return ctx;   // 会话不存在(已吊销/过期清理)
    }
    int64_t now = ZmDbModule::Now();
    int64_t absolute = zm_json_get_int(row, "absolute_expire", 0);
    int64_t sliding = zm_json_get_int(row, "expire_time", 0);
    if (now > absolute)
    {
        co_await m_db->Exec("DELETE FROM sessions WHERE token_hash=?1;", {tokenHash});
        DropCache(tokenHash);
        co_return ctx;
    }
    if (now > sliding)
    {
        // 滑动过期(30 天未活跃):失效
        co_await m_db->Exec("DELETE FROM sessions WHERE token_hash=?1;", {tokenHash});
        DropCache(tokenHash);
        co_return ctx;
    }

    // 续期写库节流:last_active / expire_time 每 ≥5 分钟落库一次,窗口内仅内存判定
    bool needWrite = false;
    {
        std::lock_guard<std::mutex> lock(m_cacheMtx);
        auto it = m_lastWrite.find(tokenHash);
        int64_t lastW = (it != m_lastWrite.end()) ? it->second : 0;
        if (now - lastW >= 300)
        {
            m_lastWrite[tokenHash] = now;
            needWrite = true;
        }
    }
    if (needWrite)
    {
        int64_t newSliding = now + SlidingExpireSec();
        co_await m_db->Exec(
            "UPDATE sessions SET last_active=?1, expire_time=?2 WHERE token_hash=?3;",
            {std::to_string(now), std::to_string(newSliding), tokenHash});
    }

    ctx.valid = true;
    ctx.uid = zm_json_get_int(row, "uid", 0);
    ctx.account = zm_json_get_str(row, "account");
    ctx.nickname = zm_json_get_str(row, "nickname");
    ctx.roleCode = zm_json_get_str(row, "role_code");
    ctx.level = zm_json_get_int(row, "level", 0);
    ctx.forceChange = zm_json_get_int(row, "force_change", 0);
    ctx.status = zm_json_get_int(row, "status", 1);
    ctx.deleted = zm_json_get_int(row, "deleted", 0);
    ctx.tokenHash = tokenHash;
    ctx.ip = ip;
    co_return ctx;
}

// ============================================================================
// 吊销
// ============================================================================
drogon::Task<bool> ZmSessionModule::Revoke(const std::string& cookieValue)
{
    if (cookieValue.empty())
        co_return true;
    std::string tokenHash = Sha256Hex(cookieValue);
    bool ok = co_await m_db->Exec("DELETE FROM sessions WHERE token_hash=?1;",
                                  {tokenHash});
    DropCache(tokenHash);
    co_return ok;
}

drogon::Task<int> ZmSessionModule::RevokeAllByUid(int64_t uid)
{
    auto rows = co_await m_db->QueryRows(
        "SELECT token_hash FROM sessions WHERE uid=?1;", {std::to_string(uid)});
    int n = static_cast<int>(rows.size());
    if (n > 0)
    {
        bool ok = co_await m_db->Exec("DELETE FROM sessions WHERE uid=?1;",
                                      {std::to_string(uid)});
        if (!ok)
            co_return 0;
    }
    DropCacheByUid(uid);
    co_return n;
}

void ZmSessionModule::DropCache(const std::string& tokenHash)
{
    std::lock_guard<std::mutex> lock(m_cacheMtx);
    m_lastWrite.erase(tokenHash);
}

void ZmSessionModule::DropCacheByUid(int64_t uid)
{
    // 会话缓存键是 token_hash;吊销时无法反查 uid,简单全清(低频操作,开销可忽略)
    (void)uid;
    std::lock_guard<std::mutex> lock(m_cacheMtx);
    m_lastWrite.clear();
}
