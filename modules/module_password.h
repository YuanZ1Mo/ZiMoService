#ifndef ZM_MODULE_PASSWORD_H
#define ZM_MODULE_PASSWORD_H

// ============================================================================
// ZmPasswordModule:密码安全模块(设计文档 §3.3)
//  数据归属:users 的密码列(经 UserModule 落库)。
//  PBKDF2-HMAC-SHA256(600k 迭代,16B 随机盐,dklen 32),哈希串带参数格式:
//      pass_salt = <salt_hex>
//      pass_hash = pbkdf2$sha256$<iterations>$<hash_hex>   (迭代可演进,历史哈希不失效)
//  临时密码同规则落 temp_pass_*;服务端强度复核(等效规则集)。
//  哈希计算一律走工作池(事件循环不阻塞)。
// ============================================================================

#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <string>

class ZmUserModule;

class ZmPasswordModule
{
public:
    explicit ZmPasswordModule(ZmUserModule* user);
    ~ZmPasswordModule();

    /// 计算哈希(工作池内执行):返回 {salt, hash}
    drogon::Task<ZMJSON> HashPassword(const std::string& password);
    /// 恒定时间比较(工作池内执行)
    drogon::Task<bool> VerifyPassword(const std::string& password,
                                      const std::string& saltHex,
                                      const std::string& hash);
    /// 服务端强度复核(8-64、无空白/控制符、字符类别≥3、弱口令/键盘序列黑名单)
    bool VerifyStrength(const std::string& password, std::string& errMsg);
    /// 一次性临时密码(12 位随机,大小写+数字)
    static std::string GenerateTempPassword();
    /// 密码更换:更新哈希 + 清临时密码 + 按需清 force_change
    drogon::Task<bool> ApplyPasswordChange(int64_t uid, const std::string& newSalt,
                                           const std::string& newHash,
                                           bool clearForceChange);

    // ── 静态加密原语 ──
    static std::string RandomHex(int bytes);
    static bool Pbkdf2Hash(const std::string& password, const std::string& saltHex,
                           int iterations, std::string& outHashHex);
    static bool ConstantTimeEquals(const std::string& a, const std::string& b);

private:
    ZmUserModule* m_user = nullptr;
};

#endif // ZM_MODULE_PASSWORD_H
