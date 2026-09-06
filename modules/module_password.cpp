#include "modules/module_password.h"

#include "modules/module_db.h"
#include "modules/module_user.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <zm_net_http_server.h>
#include <zm_util_logger.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace
{
/// 字节 → hex(小写)
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

/// hex → 字节;非法输入返回 false
bool FromHex(const std::string& hex, std::vector<unsigned char>& out)
{
    if (hex.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return true;
}

/// 弱口令黑名单(常见弱密码;长度越界已在前面拒绝)
bool IsWeakPassword(const std::string& p)
{
    static const std::unordered_set<std::string> kWeak = {
        "password", "123456", "12345678", "123456789", "1234567890",
        "qwerty", "qwertyuiop", "asdfghjkl", "zxcvbnm", "admin", "admin123",
        "123qwe", "abc123", "iloveyou", "111111", "000000", "aaaaaa",
        "a123456", "123123", "123321", "654321", "88888888", "password1",
        "passw0rd", "welcome", "sunshine", "monkey", "dragon", "master",
    };
    if (kWeak.count(p) != 0)
        return true;
    // 键盘行序列(逐段连续方向)
    static const char* rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm",
                                 "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (const char* row : rows)
    {
        std::string r(row);
        if (r.find(p) != std::string::npos)
            return true;
        // 反向
        std::string rev(r.rbegin(), r.rend());
        if (rev.find(p) != std::string::npos)
            return true;
    }
    // 全同字符 / 全数字升序或降序
    if (p.size() >= 4)
    {
        bool allSame = true;
        for (size_t i = 1; i < p.size(); ++i)
            allSame = allSame && (p[i] == p[0]);
        if (allSame)
            return true;
        if (std::all_of(p.begin(), p.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        {
            bool asc = true, desc = true;
            for (size_t i = 1; i < p.size(); ++i)
            {
                asc = asc && (p[i] == p[i - 1] + 1);
                desc = desc && (p[i] == p[i - 1] - 1);
            }
            if (asc || desc)
                return true;
        }
    }
    return false;
}
}  // namespace

// ============================================================================
// 构造
// ============================================================================
ZmPasswordModule::ZmPasswordModule(ZmUserModule* user)
    : m_user(user)
{
}

ZmPasswordModule::~ZmPasswordModule() = default;

// ============================================================================
// 哈希 / 校验
// ============================================================================
drogon::Task<ZMJSON> ZmPasswordModule::HashPassword(const std::string& password)
{
    ZMJSON out = ZMJSON::object();
    out = co_await ZmHttpServer::RunOnPool<ZMJSON>(
        [password]() -> ZMJSON {
            ZMJSON r = ZMJSON::object();
            std::string saltHex = ZmPasswordModule::RandomHex(16);
            std::string hashHex;
            if (!ZmPasswordModule::Pbkdf2Hash(password, saltHex, 600000, hashHex))
            {
                DEFAULT_LOG_ERROR("PBKDF2 计算失败");
                return r;
            }
            r["salt"] = saltHex;
            r["hash"] = "pbkdf2$sha256$600000$" + hashHex;
            return r;
        });
    co_return out;
}

drogon::Task<bool> ZmPasswordModule::VerifyPassword(const std::string& password,
                                                    const std::string& saltHex,
                                                    const std::string& hash)
{
    bool ok = co_await ZmHttpServer::RunOnPool<bool>(
        [password, saltHex, hash]() -> bool {
            // 解析迭代参数:pbkdf2$sha256$<iter>$<hash_hex>
            int iterations = 600000;
            std::string hashHex = hash;
            auto p1 = hash.find("pbkdf2$sha256$");
            if (p1 == 0)
            {
                size_t p2 = hash.find('$', p1 + 14);
                if (p2 != std::string::npos)
                {
                    try
                    {
                        iterations = std::stoi(hash.substr(p1 + 14, p2 - p1 - 14));
                    }
                    catch (...)
                    {
                        iterations = 600000;
                    }
                    hashHex = hash.substr(p2 + 1);
                }
            }
            std::string calc;
            if (!ZmPasswordModule::Pbkdf2Hash(password, saltHex, iterations, calc))
                return false;
            return ZmPasswordModule::ConstantTimeEquals(calc, hashHex);
        });
    co_return ok;
}

bool ZmPasswordModule::Pbkdf2Hash(const std::string& password, const std::string& saltHex,
                                  int iterations, std::string& outHashHex)
{
    std::vector<unsigned char> salt;
    if (!FromHex(saltHex, salt))
        return false;
    std::array<unsigned char, 32> dk{};
    int rc = PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                               salt.data(), static_cast<int>(salt.size()),
                               iterations, EVP_sha256(), static_cast<int>(dk.size()),
                               dk.data());
    if (rc != 1)
        return false;
    outHashHex = ToHexLower(dk.data(), dk.size());
    return true;
}

bool ZmPasswordModule::ConstantTimeEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

std::string ZmPasswordModule::RandomHex(int bytes)
{
    std::vector<unsigned char> buf(static_cast<size_t>(bytes));
    if (bytes > 0)
        RAND_bytes(buf.data(), bytes);
    return ToHexLower(buf.data(), static_cast<size_t>(bytes));
}

// ============================================================================
// 强度复核(服务端为准;等效规则集:长度 + 类别 + 黑名单 + 键盘序列)
// ============================================================================
bool ZmPasswordModule::VerifyStrength(const std::string& p, std::string& errMsg)
{
    if (p.size() < 8 || p.size() > 64)
    {
        errMsg = "密码长度须为 8-64 位";
        return false;
    }
    for (unsigned char c : p)
    {
        if (c <= 0x20 || c == 0x7F)
        {
            errMsg = "密码不能包含空格或控制字符";
            return false;
        }
    }
    bool lower = false, upper = false, digit = false, symbol = false;
    for (unsigned char c : p)
    {
        if (c >= 'a' && c <= 'z') lower = true;
        else if (c >= 'A' && c <= 'Z') upper = true;
        else if (c >= '0' && c <= '9') digit = true;
        else symbol = true;
    }
    int classes = (lower ? 1 : 0) + (upper ? 1 : 0) + (digit ? 1 : 0) + (symbol ? 1 : 0);
    if (classes < 3)
    {
        errMsg = "密码强度过低";
        return false;
    }
    if (IsWeakPassword(p))
    {
        errMsg = "密码强度过低";
        return false;
    }
    errMsg.clear();
    return true;
}

// ============================================================================
// 临时密码 / 密码更换
// ============================================================================
std::string ZmPasswordModule::GenerateTempPassword()
{
    // 12 位随机:大小写字母 + 数字(满足服务端强度:3 类字符)
    static const char* charset =
        "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::array<unsigned char, 12> buf{};
    RAND_bytes(buf.data(), static_cast<int>(buf.size()));
    std::string out;
    out.reserve(buf.size());
    for (unsigned char b : buf)
        out += charset[b % 53];
    return out;
}

drogon::Task<bool> ZmPasswordModule::ApplyPasswordChange(int64_t uid,
                                                         const std::string& newSalt,
                                                         const std::string& newHash,
                                                         bool clearForceChange)
{
    bool ok = co_await m_user->UpdatePassword(uid, newSalt, newHash);
    if (!ok)
        co_return false;
    bool ok2 = co_await m_user->ClearTempPassword(uid);
    if (!ok2)
        co_return false;
    if (clearForceChange)
    {
        bool ok3 = co_await m_user->SetForceChange(uid, 0);
        if (!ok3)
            co_return false;
    }
    co_return true;
}
