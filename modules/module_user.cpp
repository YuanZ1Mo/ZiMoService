#include "module_user.h"

#include "http_frontend_manager.h"
#include "zm_net_runloop.h"   // ZmEvBaseRunLoop(每日清理事件循环线程)

#include "zm_util_logger.h"

#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <windows.h>

#include <chrono>
#include <cstring>
#include <ctime>

namespace
{
// ============================================================================
// 内部小工具
// ============================================================================

// 公共库 sqlite 辅助(预处理语句/绑定):别名与 using 保持全部调用点不变
using Stmt = zm::ZmSqliteStmt;
using zm::BindText;
using zm::BindInt;

/** 计时均衡假盐:账号不存在时也跑一次 PBKDF2,消除存在性时间侧信道 */
const unsigned char kDummySalt[16] = {0x5F, 0x2C, 0x9E, 0x17, 0xA3, 0x41, 0x08, 0x6D,
                                      0xE1, 0x7B, 0xC4, 0x59, 0x30, 0x95, 0x42, 0xF8};

} // namespace

// ============================================================================
// 构造 / 析构 / 初始化
// ============================================================================

UserModule::UserModule(zm::ZmSqliteConn& userDb, zm::ZmSqliteConn& rateDb,
                       zm::ZmSqliteConn& auditDb)
    : m_userDb(userDb), m_rateDb(rateDb), m_auditDb(auditDb)
{
}

UserModule::~UserModule()
{
    Shutdown();
}

bool UserModule::Open()
{
    if (m_openOk.load())
        return true;

    // 库由 DbInitializer 统一打开并建表/补列;此处仅检查可用性
    if (!m_userDb.IsOpen() || !m_rateDb.IsOpen() || !m_auditDb.IsOpen())
    {
        DEFAULT_LOG_ERROR("[User] 数据库不可用,用户系统不可用");
        return false;
    }

    m_openOk.store(true);
    DEFAULT_LOG_INFO("[User] 用户系统初始化完成");

    // 后台周期维护:独立事件循环线程 + 60s 周期定时器(替代原 for+sleep 轮询线程;
    // 启动后首个周期完成首轮清理,之后每日 03:00 窗口;回调内 m_gone 短路)
    m_bgLoop = new ZmEvBaseRunLoop("UserBgLoop");
    if (!m_bgLoop->Loop())
    {
        DEFAULT_LOG_ERROR("[User] 后台维护事件循环启动失败(维护功能不可用)");
        delete m_bgLoop;
        m_bgLoop = nullptr;
    }
    else
    {
        m_bgLoop->SetTimerCallback([this]() { MaintainTick(); });
        m_bgLoop->StartTimer(kUserCleanPollSec);
    }
    return true;
}

void UserModule::Shutdown()
{
    m_gone.store(true);
    if (m_bgLoop)
    {
        m_bgLoop->Stop();   // 优雅退出:等待当前定时器回调完成(回调入口查 m_gone 短路)
        delete m_bgLoop;
        m_bgLoop = nullptr;
    }
    // 库连接由 DbInitializer 统一关闭
}

// ============================================================================
// 路由注册(REST 分发在 DispatchRest)
// ============================================================================

void UserModule::RegisterHttpRoutes(HttpFrontendManager* httpMgr)
{
    if (!httpMgr)
        return;

    auto& router = httpMgr->GetRouter();
    router.Get("/login", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        return httpMgr->ServeStaticFile(task, "/html/login.html");
    });
    router.Get("/register", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        return httpMgr->ServeStaticFile(task, "/html/register.html");
    });
    router.Get("/reset", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        return httpMgr->ServeStaticFile(task, "/html/reset.html");
    });
    router.Get("/force-reset", [httpMgr](ZmHttpdTask* task, const BYTE*, size_t) {
        return httpMgr->ServeStaticFile(task, "/html/force-reset.html");
    });
}

// ============================================================================
// REST 分发
// ============================================================================

bool UserModule::DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                              ZmHttpdTask* task, const BYTE* body, size_t bodyLen)
{
    // 仅处理 /auth/* 前缀,其余放行(门户已由 PortalModule 独立处理)
    if (path.size() < 6 || path.compare(0, 6, "/auth/") != 0)
        return false;

    if (!m_openOk.load())
    {
        ZmReqLoopRest::ResponseError(loop, 503, "用户系统未初始化");
        return true;
    }

    const std::string ip = task->Ip() ? task->Ip() : "";

    // POST 端点解析 JSON body(GET 端点无 body;/auth/logout 无 body)
    ZMJSON req;
    if (verb == EVHTTP_REQ_POST && path != "/auth/logout")
    {
        std::string bodyStr(body ? reinterpret_cast<const char*>(body) : "", bodyLen);
        std::string err;
        req = zm_json_parse(bodyStr, err);
        if (!err.empty())
        {
            ZmReqLoopRest::ResponseError(loop, 400, "请求体不是合法 JSON");
            return true;
        }
    }

    if (verb == EVHTTP_REQ_POST && path == "/auth/login")
    {
        HandleLogin(loop, req, task, ip);
        return true;
    }
    if (verb == EVHTTP_REQ_POST && path == "/auth/register")
    {
        HandleRegister(loop, req, task, ip);
        return true;
    }
    if (verb == EVHTTP_REQ_POST && path == "/auth/reset")
    {
        HandleReset(loop, req, task, ip);
        return true;
    }
    if (verb == EVHTTP_REQ_POST && path == "/auth/logout")
    {
        HandleLogout(loop, task);
        return true;
    }
    if (verb == EVHTTP_REQ_GET && path == "/auth/me")
    {
        HandleMe(loop, task);
        return true;
    }
    if (verb == EVHTTP_REQ_GET && path == "/auth/heartbeat")
    {
        HandleHeartbeat(loop, task);
        return true;
    }
    if (verb == EVHTTP_REQ_POST && path == "/auth/complete-change")
    {
        HandleCompleteChange(loop, req, task);
        return true;
    }

    // 已挂前缀但未匹配:404
    ZmReqLoopRest::ResponseError(loop, 404, "Not found: " + path);
    return true;
}

// ============================================================================
// 通用鉴权(后续模块复用)
// ============================================================================

uint64_t UserModule::AuthAndTouch(ZmHttpdTask* task, UserInfo* out)
{
    if (!m_openOk.load() || m_gone.load())
        return 0;

    std::string token;
    if (!ExtractCookieToken(task, token))
        return 0;

    time_t now = time(nullptr);
    SessionRow sr;
    if (!FindSession(token, now, sr))
        return 0;

    TouchSession(sr.id, now, sr.lastActive);

    if (out)
    {
        out->userId = sr.userId;
        out->account = sr.account;
        out->nickname = sr.nickname;
    }
    return sr.userId;
}

// ============================================================================
// 密码学与编码
// ============================================================================

bool UserModule::NfcNormalize(const std::string& utf8, std::string& out)
{
    if (utf8.empty())
    {
        out.clear();
        return true;
    }

    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring ws((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), (int)utf8.size(), &ws[0], wlen);

    // NFC 预组合(组合序列 → 预组合字符)。
    // 注:曾用 NormalizeString(NormalizationC),在本机实测(kernel32,导入表与 GetProcAddress
    // 两种方式一致)返回 3 倍长度、输出每字符后补 2 个 NUL,行为异常;
    // FoldStringW(MAP_PRECOMPOSED) 行为正确(实测 11 字符往返一致),语义等价 NFC。
    int nlen = FoldStringW(MAP_PRECOMPOSED, ws.c_str(), wlen, nullptr, 0);
    if (nlen <= 0)
        return false;
    std::wstring nws((size_t)nlen, L'\0');
    FoldStringW(MAP_PRECOMPOSED, ws.c_str(), wlen, &nws[0], nlen);

    // UTF-16 → UTF-8
    int blen = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        nws.c_str(), nlen, nullptr, 0, nullptr, nullptr);
    if (blen <= 0)
        return false;
    out.resize((size_t)blen);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        nws.c_str(), nlen, &out[0], blen, nullptr, nullptr);
    return true;
}

bool UserModule::CountCodePoints(const std::string& utf8, size_t& count)
{
    count = 0;
    size_t i = 0, n = utf8.size();
    while (i < n)
    {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80)
        {
            cp = c; len = 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F; len = 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F; len = 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07; len = 4;
        }
        else
        {
            return false;   // 非法首字节
        }

        if (i + len > n)
            return false;
        for (size_t k = 1; k < len; ++k)
        {
            unsigned char cc = (unsigned char)utf8[i + k];
            if ((cc & 0xC0) != 0x80)
                return false;   // 非法续字节
            cp = (cp << 6) | (cc & 0x3F);
        }
        // 过短编码 / 超范围 / 代理区检查
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;

        ++count;
        i += len;
    }
    return true;
}

bool UserModule::IsPrintableCodePoint(uint32_t cp)
{
    // C0/C1 控制字符
    if (cp <= 0x1F || (cp >= 0x7F && cp <= 0x9F))
        return false;
    // 空白类(空格/不换行空格/行与段分隔/各类空格)
    if (cp == 0x20 || cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202F || cp == 0x205F || cp == 0x3000)
        return false;
    if (cp >= 0x2000 && cp <= 0x200A)
        return false;
    return true;
}

std::string UserModule::ToLowerAscii(const std::string& s)
{
    std::string r = s;
    for (char& c : r)
    {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
    }
    return r;
}

bool UserModule::Pbkdf2(const std::string& password, const std::string& salt, std::string& out32)
{
    out32.resize(32);
    int rc = PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(),
        reinterpret_cast<const unsigned char*>(salt.data()), (int)salt.size(),
        kUserPbkdf2Iter, EVP_sha256(), 32, reinterpret_cast<unsigned char*>(&out32[0]));
    return rc == 1;
}

bool UserModule::GenRandomSalt(std::string& out16)
{
    out16.resize(16);
    return RAND_bytes(reinterpret_cast<unsigned char*>(&out16[0]), 16) == 1;
}

bool UserModule::GenSessionToken(std::string& hex64)
{
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        return false;
    static const char* hexc = "0123456789abcdef";
    hex64.clear();
    hex64.reserve(64);
    for (unsigned char b : buf)
    {
        hex64 += hexc[(b >> 4) & 0xF];
        hex64 += hexc[b & 0xF];
    }
    return true;
}

bool UserModule::Sha256Hex(const std::string& in, std::string& hex64)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, in.data(), in.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, md, &mdlen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return false;

    static const char* hexc = "0123456789abcdef";
    hex64.clear();
    hex64.reserve(mdlen * 2);
    for (unsigned int i = 0; i < mdlen; ++i)
    {
        hex64 += hexc[(md[i] >> 4) & 0xF];
        hex64 += hexc[md[i] & 0xF];
    }
    return true;
}

// ============================================================================
// 规则校验(NFC 后输入)
// ============================================================================

UserModule::FieldError UserModule::ValidateAccount(std::string& lowered)
{
    lowered = ToLowerAscii(lowered);
    if (lowered.empty())
        return FieldError::Empty;

    size_t n = 0;
    if (!CountCodePoints(lowered, n))
        return FieldError::Charset;
    if (n < 4 || n > 30)
        return FieldError::Length;

    for (char c : lowered)
    {
        bool okc = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!okc)
            return FieldError::Charset;
    }
    if (lowered.front() == '_' || lowered.front() == '-' ||
        lowered.back() == '_' || lowered.back() == '-')
        return FieldError::Edge;
    return FieldError::Ok;
}

UserModule::FieldError UserModule::ValidatePassword(const std::string& pwd)
{
    if (pwd.empty())
        return FieldError::Empty;

    size_t n = 0;
    if (!CountCodePoints(pwd, n))
        return FieldError::Charset;
    if (n < 8 || n > 64)
        return FieldError::Length;

    // 可打印检查(逐码点;CountCodePoints 已保证编码合法,这里直接解码取值)
    size_t i = 0;
    while (i < pwd.size())
    {
        unsigned char c = (unsigned char)pwd[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80)
        {
            cp = c; len = 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F; len = 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F; len = 3;
        }
        else
        {
            cp = c & 0x07; len = 4;
        }
        for (size_t k = 1; k < len; ++k)
            cp = (cp << 6) | ((unsigned char)pwd[i + k] & 0x3F);
        if (!IsPrintableCodePoint(cp))
            return FieldError::Charset;
        i += len;
    }
    return FieldError::Ok;
}

UserModule::FieldError UserModule::ValidateNickname(const std::string& nick)
{
    if (nick.empty())
        return FieldError::Empty;

    size_t n = 0;
    if (!CountCodePoints(nick, n))
        return FieldError::Charset;
    if (n < 1 || n > (size_t)kUserNicknameMaxCp)
        return FieldError::Length;

    size_t i = 0;
    while (i < nick.size())
    {
        unsigned char c = (unsigned char)nick[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80)
        {
            cp = c; len = 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F; len = 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F; len = 3;
        }
        else
        {
            cp = c & 0x07; len = 4;
        }
        for (size_t k = 1; k < len; ++k)
            cp = (cp << 6) | ((unsigned char)nick[i + k] & 0x3F);
        if (!IsPrintableCodePoint(cp))
            return FieldError::Charset;
        i += len;
    }
    return FieldError::Ok;
}

UserModule::FieldError UserModule::ValidateRescue(std::string& lowered)
{
    lowered = ToLowerAscii(lowered);
    if (lowered.empty())
        return FieldError::Empty;

    size_t n = 0;
    if (!CountCodePoints(lowered, n))
        return FieldError::Charset;
    if (n < 8 || n > 16)
        return FieldError::Length;

    for (char c : lowered)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return FieldError::Charset;
    }
    return FieldError::Ok;
}

const char* UserModule::FieldErrorText(FieldError e, const char* field)
{
    switch (e)
    {
    case FieldError::Empty:
        return "参数不能为空";
    case FieldError::Length:
        if (std::strcmp(field, "account") == 0)   return "账号长度需为 4-30 位";
        if (std::strcmp(field, "password") == 0)  return "密码长度需为 8-64 位";
        if (std::strcmp(field, "nickname") == 0)  return "昵称长度需为 1-20 位";
        return "救援码长度需为 8-16 位";
    case FieldError::Charset:
        if (std::strcmp(field, "account") == 0)   return "账号仅支持字母、数字、下划线或短横线";
        if (std::strcmp(field, "password") == 0)  return "密码不能包含空格或控制字符";
        if (std::strcmp(field, "nickname") == 0)  return "昵称不能包含空白字符";
        return "救援码仅支持数字或字母";
    case FieldError::Edge:
        return "账号首尾不能是下划线或短横线";
    default:
        return "参数不合法";
    }
}

// ============================================================================
// 锁定(login_locks;维度 = 账号+IP,阶梯递增)
// ============================================================================

bool UserModule::IsLocked(const std::string& account, const std::string& ip, time_t now)
{
    if (account.empty())
        return false;

    const std::string lip = ToLowerAscii(ip);   // IP 归一化:防 IPv6 大小写变体绕计数/绕锁定
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "SELECT locked_until FROM login_locks WHERE account=? AND ip=?");
    if (!st.p)
    {
        // 检查失败 = 无法确认未锁定 → fail-closed(按锁定处理)
        DEFAULT_LOG_ERROR("[User] IsLocked prepare 失败(按锁定处理): account={}", account);
        return true;
    }
    BindText(st.p, 1, account);
    BindText(st.p, 2, lip);
    if (sqlite3_step(st.p) == SQLITE_ROW)
        return sqlite3_column_int64(st.p, 0) > (int64_t)now;
    return false;
}

void UserModule::RecordFail(const std::string& account, const std::string& ip, time_t now)
{
    if (account.empty())
        return;

    const std::string lip = ToLowerAscii(ip);   // IP 归一化:与 IsLocked 键一致
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());

    // 读取当前计数(锁定期内请求已被 IsLocked 挡掉,不会走到这里;锁期满后计数继续累计)
    int64_t failCount = 0;
    {
        Stmt st(m_userDb, "SELECT fail_count FROM login_locks WHERE account=? AND ip=?");
        if (!st.p)
        {
            DEFAULT_LOG_ERROR("[User] RecordFail prepare 失败(计数未记录,锁定可能失效): account={}",
                              account);
            return;
        }
        BindText(st.p, 1, account);
        BindText(st.p, 2, lip);
        if (sqlite3_step(st.p) == SQLITE_ROW)
            failCount = sqlite3_column_int64(st.p, 0);
    }

    failCount += 1;
    int64_t lockedUntil = 0;
    if (failCount % kUserLockTierStep == 0)
        lockedUntil = now + (failCount == kUserLockTierStep ? kUserLockDurTier1 : kUserLockDurTierN);

    Stmt st(m_userDb,
        "INSERT INTO login_locks(account,ip,fail_count,last_fail_at,locked_until) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(account,ip) DO UPDATE SET "
        "fail_count=excluded.fail_count, last_fail_at=excluded.last_fail_at, "
        "locked_until=excluded.locked_until");
    if (!st.p)
    {
        DEFAULT_LOG_ERROR("[User] RecordFail upsert prepare 失败(计数未记录,锁定可能失效): account={}",
                          account);
        return;
    }
    BindText(st.p, 1, account);
    BindText(st.p, 2, lip);
    BindInt(st.p, 3, failCount);
    BindInt(st.p, 4, (int64_t)now);
    BindInt(st.p, 5, lockedUntil);
    if (sqlite3_step(st.p) != SQLITE_DONE)
        DEFAULT_LOG_ERROR("[User] RecordFail upsert 失败(计数未记录,锁定可能失效): account={}",
                          account);
}

void UserModule::ClearLock(const std::string& account, const std::string& ip)
{
    if (account.empty())
        return;

    const std::string lip = ToLowerAscii(ip);   // IP 归一化:与 IsLocked 键一致
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "DELETE FROM login_locks WHERE account=? AND ip=?");
    if (!st.p)
    {
        DEFAULT_LOG_ERROR("[User] ClearLock prepare 失败(残留锁定记录): account={}", account);
        return;
    }
    BindText(st.p, 1, account);
    BindText(st.p, 2, lip);
    sqlite3_step(st.p);
}

// ============================================================================
// 限流(rate 库;窗口滑动,落库持久化,重启不失效)
// ============================================================================

bool UserModule::CheckRate(const char* table, const std::string& ip, time_t now)
{
    if (ip.empty())
        return false;   // 拿不到 IP 时保守拒绝

    const std::string lip = ToLowerAscii(ip);   // IP 归一化:IPv6 大小写变体不再绕计数
    std::lock_guard<std::mutex> lk(m_rateDb.Mutex());

    int64_t windowStart = 0, count = 0;
    bool haveRow = false;
    {
        Stmt st(m_rateDb, ("SELECT window_start,count FROM " + std::string(table) + " WHERE ip=?").c_str());
        if (!st.p)
        {
            DEFAULT_LOG_ERROR("[User] rate select prepare failed: {}", table);
            return false;
        }
        BindText(st.p, 1, lip);
        if (sqlite3_step(st.p) == SQLITE_ROW)
        {
            haveRow = true;
            windowStart = sqlite3_column_int64(st.p, 0);
            count = sqlite3_column_int64(st.p, 1);
        }
    }

    if (!haveRow || now - windowStart >= kUserRateWindow)
    {
        // 窗口滑过(或无行):重置为 1(本次请求计入新窗口)
        Stmt st(m_rateDb,
            ("INSERT INTO " + std::string(table) + "(ip,window_start,count) VALUES(?,?,1) "
             "ON CONFLICT(ip) DO UPDATE SET window_start=excluded.window_start, count=1").c_str());
        if (!st.p)
        {
            DEFAULT_LOG_ERROR("[User] rate upsert prepare failed: {}", table);
            return false;
        }
        BindText(st.p, 1, lip);
        BindInt(st.p, 2, (int64_t)now);
        sqlite3_step(st.p);
        return true;
    }

    if (count >= kUserRateLimit)
        return false;   // 超限:计数保留,窗口滑过自然恢复

    Stmt st(m_rateDb, ("UPDATE " + std::string(table) + " SET count=count+1 WHERE ip=?").c_str());
    if (!st.p)
        return false;
    BindText(st.p, 1, lip);
    sqlite3_step(st.p);
    return true;
}

// ============================================================================
// 会话(sessions,用户库)
// ============================================================================

bool UserModule::CreateSession(const std::string& token, uint64_t userId, const std::string& ip, time_t now)
{
    std::string tokenHash;
    if (!Sha256Hex(token, tokenHash))
        return false;

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    // 事务:计数 + 踢最旧 + 插入原子化,崩溃不留"会话数超限"中间态
    if (!m_userDb.Begin())
        return false;

    // 活跃会话数(双上限有效)
    int active = 0;
    {
        Stmt st(m_userDb,
            "SELECT COUNT(*) FROM sessions WHERE user_id=? AND expire_time>? AND absolute_expire>?");
        if (!st.p)
        {
            m_userDb.Rollback();
            return false;
        }
        BindInt(st.p, 1, (int64_t)userId);
        BindInt(st.p, 2, (int64_t)now);
        BindInt(st.p, 3, (int64_t)now);
        if (sqlite3_step(st.p) == SQLITE_ROW)
            active = sqlite3_column_int(st.p, 0);
    }

    // 超上限:按 last_active 升序踢最旧 1 条(LRU;一次签发最多超 1 条)
    if (active >= kUserMaxSessions)
    {
        Stmt st(m_userDb,
            "DELETE FROM sessions WHERE id=(SELECT id FROM sessions WHERE user_id=? "
            "AND expire_time>? AND absolute_expire>? ORDER BY last_active ASC LIMIT 1)");
        if (!st.p)
        {
            m_userDb.Rollback();
            return false;
        }
        BindInt(st.p, 1, (int64_t)userId);
        BindInt(st.p, 2, (int64_t)now);
        BindInt(st.p, 3, (int64_t)now);
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            m_userDb.Rollback();
            return false;
        }
    }

    Stmt st(m_userDb,
        "INSERT INTO sessions(token_hash,user_id,create_ip,"
        "create_time,last_active,expire_time,absolute_expire) "
        "VALUES(?,?,?,?,?,?,?)");
    if (!st.p)
    {
        m_userDb.Rollback();
        return false;
    }
    BindText(st.p, 1, tokenHash);
    BindInt(st.p, 2, (int64_t)userId);
    BindText(st.p, 3, ip);
    BindInt(st.p, 4, (int64_t)now);
    BindInt(st.p, 5, (int64_t)now);
    BindInt(st.p, 6, (int64_t)now + kUserSessionSliding);
    BindInt(st.p, 7, (int64_t)now + kUserSessionAbsolute);
    if (sqlite3_step(st.p) != SQLITE_DONE)
    {
        m_userDb.Rollback();
        return false;
    }
    if (!m_userDb.Commit())
    {
        m_userDb.Rollback();
        return false;
    }
    return true;
}

bool UserModule::FindSession(const std::string& token, time_t now, SessionRow& out)
{
    std::string tokenHash;
    if (!Sha256Hex(token, tokenHash))
        return false;

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb,
        "SELECT s.id, s.user_id, u.account, u.nickname, s.create_ip, s.create_time, s.last_active, "
        "u.last_login_ip, u.last_login_time "
        "FROM sessions s JOIN users u ON u.user_id=s.user_id "
        "WHERE s.token_hash=? AND s.expire_time>? AND s.absolute_expire>? "
        "AND u.disabled=0 AND u.deleted=0");
    if (!st.p)
        return false;
    BindText(st.p, 1, tokenHash);
    BindInt(st.p, 2, (int64_t)now);
    BindInt(st.p, 3, (int64_t)now);
    if (sqlite3_step(st.p) != SQLITE_ROW)
        return false;

    out.id = (uint64_t)sqlite3_column_int64(st.p, 0);      // 会话行 id(TouchSession 用)
    out.userId = (uint64_t)sqlite3_column_int64(st.p, 1);  // 用户唯一身份 user_id
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
    out.account = ac ? ac : "";
    const char* nick = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
    out.nickname = nick ? nick : "";
    const char* cip = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 4));
    out.createIp = cip ? cip : "";
    out.createTime = (time_t)sqlite3_column_int64(st.p, 5);
    out.lastActive = (time_t)sqlite3_column_int64(st.p, 6);
    const char* lip = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 7));
    out.lastLoginIp = lip ? lip : "";
    out.lastLoginTime = (time_t)sqlite3_column_int64(st.p, 8);
    return true;
}

void UserModule::TouchSession(uint64_t sessionId, time_t now, time_t lastActive)
{
    if (now - lastActive < kUserTouchThrottle)
        return;   // 续期节流:距上次落库不足 60s 不写

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "UPDATE sessions SET last_active=?, expire_time=? WHERE id=?");
    if (!st.p)
        return;
    BindInt(st.p, 1, (int64_t)now);
    BindInt(st.p, 2, (int64_t)now + kUserSessionSliding);
    BindInt(st.p, 3, (int64_t)sessionId);
    sqlite3_step(st.p);
}

void UserModule::RespondWithNewSession(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t userId,
                                       const std::string& ip, time_t now,
                                       const std::string& fallbackAccount,
                                       const std::string& fallbackNickname,
                                       bool forceChangeFlag)
{
    // 签发全新会话(防会话固定)并返回用户信息
    std::string token;
    if (!GenSessionToken(token) || !CreateSession(token, userId, ip, now))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    SetSessionCookie(task, token);

    SessionRow sr;
    FindSession(token, now, sr);   // 取完整会话行组装
    if (!fallbackAccount.empty() && sr.account.empty())
        sr.account = fallbackAccount;
    if (!fallbackNickname.empty() && sr.nickname.empty())
        sr.nickname = fallbackNickname;
    ZMJSON rsp;
    FillUserResult(rsp["result"]["user"], sr);
    if (forceChangeFlag)
        rsp["result"]["forceChange"] = true;   // 前端据此跳强制改密页
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

void UserModule::RevokeAllSessions(uint64_t userId)
{
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "DELETE FROM sessions WHERE user_id=?");
    if (!st.p)
        return;
    BindInt(st.p, 1, (int64_t)userId);
    sqlite3_step(st.p);
}

bool UserModule::ExtractCookieToken(ZmHttpdTask* task, std::string& token)
{
    const char* cookie = task->GetRequestHeader("Cookie", "");
    if (!cookie || !*cookie)
        return false;

    const std::string prefix = std::string(kUserCookieName) + "=";
    const std::string cs = cookie;
    size_t pos = 0;
    while (pos <= cs.size())
    {
        size_t semi = cs.find(';', pos);
        std::string item = cs.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t b = item.find_first_not_of(" \t");
        if (b != std::string::npos)
        {
            size_t e = item.find_last_not_of(" \t");
            item = item.substr(b, e - b + 1);
        }
        if (item.compare(0, prefix.size(), prefix) == 0)
        {
            token = item.substr(prefix.size());
            return !token.empty();
        }
        if (semi == std::string::npos)
            break;
        pos = semi + 1;
    }
    return false;
}

// ============================================================================
// 响应辅助
// ============================================================================

void UserModule::SetSessionCookie(ZmHttpdTask* task, const std::string& token)
{
    std::string val = std::string(kUserCookieName) + "=" + token +
        "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + std::to_string(kUserCookieMaxAge);
    if (task->IsHttps())
        val += "; Secure";
    task->PutReplyHeader("Set-Cookie", val);
}

void UserModule::ClearSessionCookie(ZmHttpdTask* task)
{
    std::string val = std::string(kUserCookieName) + "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
    if (task->IsHttps())
        val += "; Secure";
    task->PutReplyHeader("Set-Cookie", val);
}

void UserModule::FillUserResult(ZMJSON& user, const SessionRow& s)
{
    user["account"] = s.account;
    user["nickname"] = s.nickname;
    // 最后登录 IP/时间 = 账号维度(users 表,最近一次登录),与当前会话解耦;
    // 最后活动时间 = 当前会话最近续期
    user["lastLoginIp"] = s.lastLoginIp;
    user["lastLoginTime"] = (int64_t)s.lastLoginTime;
    user["lastActiveTime"] = (int64_t)s.lastActive;
}

// ============================================================================
// API handlers
// ============================================================================

void UserModule::HandleLogin(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task,
                             const std::string& ip)
{
    time_t now = time(nullptr);
    std::string account = zm_json_get_str(body, "account");
    std::string password = zm_json_get_str(body, "password");

    // ① 锁定检查(账号+IP;锁定键统一小写,锁定中直接拒绝,不校验不计数)
    if (IsLocked(ToLowerAscii(account), ip, now))
    {
        ZmReqLoopRest::ResponseError(loop, 429, "登录操作已被锁定,请稍后再试");
        return;
    }

    // ② 参数规则校验(NFC 后;400 具体文案,不涉及账号枚举)
    std::string naccount, npassword;
    if (!NfcNormalize(account, naccount) || !NfcNormalize(password, npassword))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    FieldError ae = ValidateAccount(naccount);
    if (ae != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(ae, "account"));
        return;
    }
    FieldError pe = ValidatePassword(npassword);
    if (pe != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(pe, "password"));
        return;
    }

    // ③ 业务校验(锁内取盐,锁外 PBKDF2 计时均衡,锁内比较)
    uint64_t userId = 0;
    std::string salt, hash, nickname, tempSalt, tempHash;
    bool userExists = false;
    bool disabledFlag = false, deletedFlag = false, forceChange = false;
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb,
            "SELECT user_id, pass_salt, pass_hash, nickname, disabled, deleted, force_change, "
            "temp_pass_salt, temp_pass_hash FROM users WHERE account=?");
        if (st.p)
        {
            BindText(st.p, 1, naccount);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                userExists = true;
                userId = (uint64_t)sqlite3_column_int64(st.p, 0);
                const char* s = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 1));
                int slen = sqlite3_column_bytes(st.p, 1);
                salt.assign(s ? s : "", slen > 0 ? slen : 0);
                const char* h = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 2));
                int hlen = sqlite3_column_bytes(st.p, 2);
                hash.assign(h ? h : "", hlen > 0 ? hlen : 0);
                const char* nc = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
                nickname = nc ? nc : "";
                disabledFlag = sqlite3_column_int(st.p, 4) != 0;
                deletedFlag = sqlite3_column_int(st.p, 5) != 0;
                forceChange = sqlite3_column_int(st.p, 6) != 0;
                const char* ts = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 7));
                int tslen = sqlite3_column_bytes(st.p, 7);
                tempSalt.assign(ts ? ts : "", tslen > 0 ? tslen : 0);
                const char* th = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 8));
                int thlen = sqlite3_column_bytes(st.p, 8);
                tempHash.assign(th ? th : "", thlen > 0 ? thlen : 0);
            }
        }
    }

    // 停用/软删除账号:直接拒绝(统一文案,不区分;不计入锁定)
    if (userExists && (disabledFlag || deletedFlag))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "账号已被停用");
        return;
    }

    // 密码验证:force_change 时按临时密码比对(改密完成前有效)
    std::string pwdHash;
    Pbkdf2(npassword,
        userExists ? (forceChange && !tempSalt.empty() ? tempSalt : salt)
            : std::string(reinterpret_cast<const char*>(kDummySalt), sizeof(kDummySalt)),
        pwdHash);
    const std::string& targetHash = forceChange ? tempHash : hash;
    bool pwdOk = userExists && pwdHash.size() == targetHash.size() &&
                 CRYPTO_memcmp(pwdHash.data(), targetHash.data(), pwdHash.size()) == 0;

    if (!pwdOk)
    {
        // 账号不存在也记录锁定(account 允许不存在于 users,防探测)
        RecordFail(naccount, ip, now);
        ZmReqLoopRest::ResponseError(loop, 401, "账号或密码错误");
        return;
    }

    ClearLock(naccount, ip);

    // ④ 更新账号最后登录 IP/时间(账号维度,任意设备可见)
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "UPDATE users SET last_login_ip=?, last_login_time=? WHERE user_id=?");
        if (st.p)
        {
            BindText(st.p, 1, ip);
            BindInt(st.p, 2, (int64_t)now);
            BindInt(st.p, 3, (int64_t)userId);
            sqlite3_step(st.p);
        }
    }

    // ⑤ 签发全新会话(防会话固定)并返回用户信息
    RespondWithNewSession(loop, task, userId, ip, now, naccount, nickname, forceChange);
}

void UserModule::HandleCompleteChange(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task)
{
    UserInfo ui;
    uint64_t uid = AuthAndTouch(task, &ui);
    if (!uid)
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }

    std::string password = zm_json_get_str(body, "password");
    std::string rescue = zm_json_get_str(body, "rescue");

    std::string npassword, nrescue;
    if (!NfcNormalize(password, npassword) || !NfcNormalize(rescue, nrescue))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    FieldError pe = ValidatePassword(npassword);
    if (pe != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(pe, "password"));
        return;
    }
    FieldError re = ValidateRescue(nrescue);
    if (re != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(re, "rescue"));
        return;
    }

    // 必须处于强制改密状态
    bool forceChange = false;
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "SELECT force_change FROM users WHERE user_id=?");
        if (st.p)
        {
            BindInt(st.p, 1, (int64_t)uid);
            if (sqlite3_step(st.p) == SQLITE_ROW)
                forceChange = sqlite3_column_int(st.p, 0) != 0;
        }
    }
    if (!forceChange)
    {
        ZmReqLoopRest::ResponseError(loop, 400, "无待完成的改密流程");
        return;
    }

    // 新盐新哈希(密码 + 救援码),清临时密码与强制改密标记
    std::string passSalt, passHash, rescueSalt, rescueHash;
    if (!GenRandomSalt(passSalt) || !GenRandomSalt(rescueSalt) ||
        !Pbkdf2(npassword, passSalt, passHash) ||
        !Pbkdf2(nrescue, rescueSalt, rescueHash))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    // 当前会话 id(改密后保留;其余会话——含临时密码签发期间建立的全部会话——一律吊销,
    // 防临时密码泄露场景下旧会话继续有效)
    uint64_t keepSessionId = 0;
    {
        std::string token;
        if (ExtractCookieToken(task, token))
        {
            SessionRow sr;
            if (FindSession(token, time(nullptr), sr))
                keepSessionId = sr.id;
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb,
            "UPDATE users SET pass_salt=?, pass_hash=?, rescue_salt=?, rescue_hash=?, "
            "temp_pass_salt=NULL, temp_pass_hash=NULL, force_change=0 WHERE user_id=?");
        if (!st.p)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        BindText(st.p, 1, passSalt);
        BindText(st.p, 2, passHash);
        BindText(st.p, 3, rescueSalt);
        BindText(st.p, 4, rescueHash);
        BindInt(st.p, 5, (int64_t)uid);
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        // 吊销其他会话(与改密同锁同语句序列,失败仅记日志不影响改密结果)
        Stmt stDel(m_userDb, "DELETE FROM sessions WHERE user_id=? AND id!=?");
        if (stDel.p)
        {
            BindInt(stDel.p, 1, (int64_t)uid);
            BindInt(stDel.p, 2, (int64_t)keepSessionId);
            if (sqlite3_step(stDel.p) != SQLITE_DONE)
                DEFAULT_LOG_ERROR("[User] complete-change 吊销其他会话失败: uid={}", uid);
        }
    }

    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void UserModule::HandleRegister(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task,
                                const std::string& ip)
{
    time_t now = time(nullptr);

    // ① IP 限流(落库)
    if (!CheckRate("register_rate_limits", ip, now))
    {
        ZmReqLoopRest::ResponseError(loop, 429, "请求过于频繁,请稍后再试");
        return;
    }

    std::string account = zm_json_get_str(body, "account");
    std::string password = zm_json_get_str(body, "password");
    std::string nickname = zm_json_get_str(body, "nickname");
    std::string rescue = zm_json_get_str(body, "rescue");

    // ② 参数规则校验(NFC 后)
    std::string naccount, npassword, nnickname, nrescue;
    if (!NfcNormalize(account, naccount) || !NfcNormalize(password, npassword) ||
        !NfcNormalize(nickname, nnickname) || !NfcNormalize(rescue, nrescue))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    FieldError ae = ValidateAccount(naccount);
    if (ae != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(ae, "account"));
        return;
    }
    FieldError pe = ValidatePassword(npassword);
    if (pe != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(pe, "password"));
        return;
    }
    // 昵称可选:不填则默认与账号一致(账号超过昵称上限时按码点截断,保持自校验规则成立)
    if (nnickname.empty())
    {
        size_t cps = 0;
        if (CountCodePoints(naccount, cps) && cps > kUserNicknameMaxCp)
        {
            size_t i = 0, cp = 0;
            while (i < naccount.size() && cp < (size_t)kUserNicknameMaxCp)
            {
                unsigned char c = (unsigned char)naccount[i];
                i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                ++cp;
            }
            nnickname = naccount.substr(0, i);
        }
        else
        {
            nnickname = naccount;
        }
    }
    else
    {
        FieldError ne = ValidateNickname(nnickname);
        if (ne != FieldError::Ok)
        {
            ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(ne, "nickname"));
            return;
        }
    }
    FieldError re = ValidateRescue(nrescue);
    if (re != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(re, "rescue"));
        return;
    }

    // ③ 账号唯一性(先查;唯一索引兜底并发)
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "SELECT 1 FROM users WHERE account=?");
        if (st.p)
        {
            BindText(st.p, 1, naccount);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                ZmReqLoopRest::ResponseError(loop, 400, "账号已被注册");
                return;
            }
        }
    }

    // ④ 盐 + 哈希(锁外计算)
    std::string passSalt, passHash, rescueSalt, rescueHash;
    if (!GenRandomSalt(passSalt) || !GenRandomSalt(rescueSalt) ||
        !Pbkdf2(npassword, passSalt, passHash) ||
        !Pbkdf2(nrescue, rescueSalt, rescueHash))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    uint64_t userId = 0;
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        // 首个注册用户自动成为开发者(系统所有者,最高等级)
        std::string role = "user";
        {
            Stmt cnt(m_userDb, "SELECT COUNT(*) FROM users");
            if (cnt.p && sqlite3_step(cnt.p) == SQLITE_ROW && sqlite3_column_int(cnt.p, 0) == 0)
                role = "developer";
        }
        // 生成 user_id:MAX+1 递增(10000001 起,99999999 封顶;与 INSERT 同锁,无并发碰撞)
        int64_t newUserId = kUserIdMin;
        {
            Stmt mx(m_userDb, "SELECT COALESCE(MAX(user_id),0) FROM users");
            if (mx.p && sqlite3_step(mx.p) == SQLITE_ROW)
                newUserId = sqlite3_column_int64(mx.p, 0) + 1;
            if (newUserId < kUserIdMin)
                newUserId = kUserIdMin;
            if (newUserId > kUserIdMax)
            {
                ZmReqLoopRest::ResponseError(loop, 500, "用户数已达上限");
                return;
            }
        }
        Stmt st(m_userDb,
            "INSERT INTO users(user_id,account,nickname,pass_salt,pass_hash,rescue_salt,rescue_hash,"
            "register_ip,create_time,last_login_ip,last_login_time,role) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!st.p)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        BindInt(st.p, 1, newUserId);
        BindText(st.p, 2, naccount);
        BindText(st.p, 3, nnickname);
        BindText(st.p, 4, passSalt);
        BindText(st.p, 5, passHash);
        BindText(st.p, 6, rescueSalt);
        BindText(st.p, 7, rescueHash);
        BindText(st.p, 8, ip);
        BindInt(st.p, 9, (int64_t)now);
        BindText(st.p, 10, ip);
        BindInt(st.p, 11, (int64_t)now);
        BindText(st.p, 12, role);
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            if (m_userDb.ExtendedErrorCode() == SQLITE_CONSTRAINT_UNIQUE)
                ZmReqLoopRest::ResponseError(loop, 400, "账号已被注册");
            else
                ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        userId = (uint64_t)newUserId;   // 对外身份 = user_id(不再用行号 id)
    }

    // ⑤ 自动登录:签发全新会话
    RespondWithNewSession(loop, task, userId, ip, now, "", "", false);
}

void UserModule::HandleReset(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task,
                             const std::string& ip)
{
    time_t now = time(nullptr);

    // ① IP 限流(落库)
    if (!CheckRate("reset_rate_limits", ip, now))
    {
        ZmReqLoopRest::ResponseError(loop, 429, "请求过于频繁,请稍后再试");
        return;
    }

    std::string account = zm_json_get_str(body, "account");
    std::string password = zm_json_get_str(body, "password");
    std::string rescue = zm_json_get_str(body, "rescue");

    // ② 锁定检查(账号+IP;场景文案"重置操作")
    if (IsLocked(ToLowerAscii(account), ip, now))
    {
        ZmReqLoopRest::ResponseError(loop, 429, "重置操作已被锁定,请稍后再试");
        return;
    }

    // ③ 参数规则校验(NFC 后)
    std::string naccount, npassword, nrescue;
    if (!NfcNormalize(account, naccount) || !NfcNormalize(password, npassword) ||
        !NfcNormalize(rescue, nrescue))
    {
        ZmReqLoopRest::ResponseError(loop, 400, "参数不合法");
        return;
    }
    FieldError ae = ValidateAccount(naccount);
    if (ae != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(ae, "account"));
        return;
    }
    FieldError pe = ValidatePassword(npassword);
    if (pe != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(pe, "password"));
        return;
    }
    FieldError re = ValidateRescue(nrescue);
    if (re != FieldError::Ok)
    {
        ZmReqLoopRest::ResponseError(loop, 400, FieldErrorText(re, "rescue"));
        return;
    }

    // ④ 救援码校验(锁内取盐,锁外 PBKDF2 计时均衡,锁内比较)
    uint64_t userId = 0;
    std::string salt, hash;
    bool userExists = false;
    bool disabledFlag = false;
    bool deletedFlag = false;
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb,
            "SELECT user_id, rescue_salt, rescue_hash, disabled, deleted FROM users WHERE account=?");
        if (st.p)
        {
            BindText(st.p, 1, naccount);
            if (sqlite3_step(st.p) == SQLITE_ROW)
            {
                userExists = true;
                userId = (uint64_t)sqlite3_column_int64(st.p, 0);
                const char* s = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 1));
                int slen = sqlite3_column_bytes(st.p, 1);
                salt.assign(s ? s : "", slen > 0 ? slen : 0);
                const char* h = reinterpret_cast<const char*>(sqlite3_column_blob(st.p, 2));
                int hlen = sqlite3_column_bytes(st.p, 2);
                hash.assign(h ? h : "", hlen > 0 ? hlen : 0);
                disabledFlag = sqlite3_column_int(st.p, 3) != 0;
                deletedFlag = sqlite3_column_int(st.p, 4) != 0;
            }
        }
    }

    // 停用/软删除账号:直接拒绝(与登录一致;不计入锁定)
    if (userExists && (disabledFlag || deletedFlag))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "账号已被停用");
        return;
    }

    std::string rescueHash;
    Pbkdf2(nrescue, userExists ? salt
        : std::string(reinterpret_cast<const char*>(kDummySalt), sizeof(kDummySalt)), rescueHash);
    bool rescueOk = userExists && rescueHash.size() == hash.size() &&
                    CRYPTO_memcmp(rescueHash.data(), hash.data(), rescueHash.size()) == 0;

    if (!rescueOk)
    {
        // 防枚举:账号不存在与救援码错误统一文案,且同样计入锁定
        RecordFail(naccount, ip, now);
        ZmReqLoopRest::ResponseError(loop, 401, "账号或救援码错误");
        return;
    }

    ClearLock(naccount, ip);

    // ⑤ 吊销该账号全部会话(内部自持锁)→ 更新密码(新盐新哈希)→ 签发新会话自动登录
    RevokeAllSessions(userId);

    std::string newSalt, newHash;
    if (!GenRandomSalt(newSalt) || !Pbkdf2(npassword, newSalt, newHash))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb,
            "UPDATE users SET pass_salt=?, pass_hash=?, last_login_ip=?, last_login_time=? WHERE user_id=?");
        if (!st.p)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        BindText(st.p, 1, newSalt);
        BindText(st.p, 2, newHash);
        BindText(st.p, 3, ip);
        BindInt(st.p, 4, (int64_t)now);
        BindInt(st.p, 5, (int64_t)userId);
        if (sqlite3_step(st.p) != SQLITE_DONE)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
    }

    RespondWithNewSession(loop, task, userId, ip, now, "", "", false);
}

void UserModule::HandleLogout(ZmReqLoop* loop, ZmHttpdTask* task)
{
    // 无论命中与否都清浏览器 cookie
    ClearSessionCookie(task);

    std::string token;
    if (!ExtractCookieToken(task, token))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }
    std::string tokenHash;
    if (!Sha256Hex(token, tokenHash))
    {
        ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
        return;
    }

    int64_t deleted = 0;
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "DELETE FROM sessions WHERE token_hash=?");
        if (!st.p)
        {
            ZmReqLoopRest::ResponseError(loop, 500, "服务器内部错误");
            return;
        }
        BindText(st.p, 1, tokenHash);
        sqlite3_step(st.p);
        deleted = m_userDb.Changes();
    }

    if (deleted == 0)
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }

    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

void UserModule::HandleMe(ZmReqLoop* loop, ZmHttpdTask* task)
{
    std::string token;
    if (!ExtractCookieToken(task, token))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }

    time_t now = time(nullptr);
    SessionRow sr;
    if (!FindSession(token, now, sr))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }
    TouchSession(sr.id, now, sr.lastActive);

    ZMJSON rsp;
    FillUserResult(rsp["result"]["user"], sr);
    // 强制改密状态(改密页据此判断是否拦截)
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "SELECT force_change FROM users WHERE user_id=?");
        if (st.p)
        {
            BindInt(st.p, 1, (int64_t)sr.userId);
            if (sqlite3_step(st.p) == SQLITE_ROW)
                rsp["result"]["forceChange"] = sqlite3_column_int(st.p, 0) != 0;
        }
    }
    ZmReqLoopRest::ResponseJson(loop, 200, rsp);
}

void UserModule::HandleHeartbeat(ZmReqLoop* loop, ZmHttpdTask* task)
{
    if (!AuthAndTouch(task, nullptr))
    {
        ZmReqLoopRest::ResponseError(loop, 401, "会话已失效");
        return;
    }
    ZmReqLoopRest::ResponseJson(loop, 200, {{"result", {{"ok", true}}}});
}

// ============================================================================
// 门户权限(modules / user_modules)
// ============================================================================

bool UserModule::GetUserRole(uint64_t userId, std::string& role)
{
    role = "user";
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "SELECT role FROM users WHERE user_id=?");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)userId);
    if (sqlite3_step(st.p) == SQLITE_ROW)
    {
        const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        role = r ? r : "user";
        return true;
    }
    return false;   // 行不存在:输出默认 "user",由调用方按返回值处理
}

bool UserModule::GetUserAccount(uint64_t userId, std::string& account)
{
    account.clear();
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "SELECT account FROM users WHERE user_id=?");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)userId);
    if (sqlite3_step(st.p) == SQLITE_ROW)
    {
        const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        account = a ? a : "";
    }
    return true;
}

bool UserModule::BatchGetUserAccounts(const std::vector<uint64_t>& ids,
                                      std::map<uint64_t, std::string>& out)
{
    out.clear();
    if (ids.empty())
        return true;

    std::string ph;
    for (size_t i = 0; i < ids.size(); ++i)
        ph += i ? ",?" : "?";

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, ("SELECT user_id, account FROM users WHERE user_id IN (" + ph + ")").c_str());
    if (!st.p)
        return false;
    for (size_t i = 0; i < ids.size(); ++i)
        BindInt(st.p, (int)i + 1, (int64_t)ids[i]);
    while (sqlite3_step(st.p) == SQLITE_ROW)
    {
        uint64_t id = (uint64_t)sqlite3_column_int64(st.p, 0);
        const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        out[id] = a ? a : "";
    }
    return true;
}

UserModule::AuthResult UserModule::RequireModule(ZmHttpdTask* task, const char* moduleCode,
                                                 UserInfo* out)
{
    if (!m_openOk.load())
        return AuthResult::Error;

    UserInfo ui;
    uint64_t uid = AuthAndTouch(task, &ui);
    if (!uid)
        return AuthResult::Unauthed;

    std::string role;
    GetUserRole(uid, role);

    // 可见模块须含 moduleCode(developer 全量,GetUserModules 已含)
    std::vector<ZMJSON> mods;
    if (!GetUserModules(uid, role, mods))
        return AuthResult::Error;
    bool has = false;
    for (const auto& m : mods)
    {
        if (zm_json_get_str(m, "code") == moduleCode)
        {
            has = true;
            break;
        }
    }
    if (!has)
        return AuthResult::Forbidden;

    if (out)
        *out = ui;
    return AuthResult::Ok;
}

int UserModule::GetUserRoleLevel(const std::string& role)
{
    if (role == "developer") return 3;
    if (role == "admin")     return 2;
    if (role == "user")      return 1;
    return 0;
}

bool UserModule::WriteAuditLog(uint64_t operatorId, const std::string& operatorAccount,
                               const std::string& action, uint64_t targetId,
                               const std::string& targetAccount, const std::string& detail)
{
    std::lock_guard<std::mutex> lk(m_auditDb.Mutex());
    Stmt st(m_auditDb,
        "INSERT INTO user_manage_logs(operator_id,operator_account,action,target_id,target_account,"
        "detail,create_time) VALUES(?,?,?,?,?,?,?)");
    if (!st.p)
    {
        DEFAULT_LOG_ERROR("[User] 审计日志落库失败(prepare): action={} target={}",
                          action, targetId);
        return false;
    }
    BindInt(st.p, 1, (int64_t)operatorId);
    BindText(st.p, 2, operatorAccount);
    BindText(st.p, 3, action);
    BindInt(st.p, 4, (int64_t)targetId);
    BindText(st.p, 5, targetAccount);
    BindText(st.p, 6, detail);
    BindInt(st.p, 7, (int64_t)time(nullptr));
    if (sqlite3_step(st.p) != SQLITE_DONE)
    {
        DEFAULT_LOG_ERROR("[User] 审计日志落库失败(step): action={} target={}",
                          action, targetId);
        return false;
    }
    return true;
}

bool UserModule::WriteBusinessLog(BizLogTable table, uint64_t opId,
                                  const std::string& opAccount, const std::string& action,
                                  const std::string& targetType, uint64_t targetId,
                                  const std::string& detail)
{
    // 枚举 → 表名(代码内常量映射,杜绝运行时拼接)
    const char* tableName = "filehub_logs";
    switch (table)
    {
    case BizLogTable::FileHubLogs: tableName = "filehub_logs"; break;
    }

    std::lock_guard<std::mutex> lk(m_auditDb.Mutex());
    Stmt st(m_auditDb,
        ("INSERT INTO " + std::string(tableName) +
         "(operator_id,operator_account,action,target_type,target_id,detail,create_time) "
         "VALUES(?,?,?,?,?,?,?)").c_str());
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)opId);
    BindText(st.p, 2, opAccount);
    BindText(st.p, 3, action);
    BindText(st.p, 4, targetType);
    BindInt(st.p, 5, (int64_t)targetId);
    BindText(st.p, 6, detail);
    BindInt(st.p, 7, (int64_t)time(nullptr));
    if (sqlite3_step(st.p) != SQLITE_DONE)
    {
        DEFAULT_LOG_ERROR("[User] 业务日志落库失败: table={} action={} target={}",
                          tableName, action, targetId);
        return false;
    }
    return true;
}

bool UserModule::GetUserModules(uint64_t userId, const std::string& role,
                                std::vector<ZMJSON>& modules)
{
    modules.clear();
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb,
        "SELECT DISTINCT m.code, m.name, m.url FROM modules m "
        "LEFT JOIN user_modules um ON um.module_code=m.code AND um.user_id=? "
        "WHERE m.enabled=1 AND (m.code='home' OR ?='developer' "
        "OR um.user_id IS NOT NULL) "
        "ORDER BY m.sort");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)userId);
    BindText(st.p, 2, role);
    while (sqlite3_step(st.p) == SQLITE_ROW)
    {
        ZMJSON m;
        const char* code = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        const char* url = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
        m["code"] = code ? code : "";
        m["name"] = name ? name : "";
        m["url"] = url ? url : "";
        modules.push_back(std::move(m));
    }
    return true;
}

// ============================================================================
// 用户管理数据操作(PortalModule 路由/校验后调用)
// ============================================================================

bool UserModule::ListUsers(const std::string& keyword, const std::string& role, int status,
                           int page, int pageSize, int& total, std::vector<UserRow>& list)
{
    // keyword 转义 LIKE 通配符(\ % _),防用户输入通配符扩大匹配面
    std::string esc;
    esc.reserve(keyword.size());
    for (char c : keyword)
    {
        if (c == '\\' || c == '%' || c == '_')
            esc += '\\';
        esc += c;
    }
    const std::string like = "%" + esc + "%";
    if (page < 1) page = 1;
    if (pageSize < 1 || pageSize > kUserPageSizeMax) pageSize = kUserPageSizeDefault;

    // 动态筛选条件:账号模糊 + 角色 + 状态
    std::string cond = " deleted=0 AND account LIKE ? ESCAPE '\\'";
    if (!role.empty())
        cond += " AND role=?";
    if (status == 1)
        cond += " AND disabled=0";
    else if (status == 2)
        cond += " AND disabled=1";

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());

    total = 0;
    {
        Stmt cnt(m_userDb, ("SELECT COUNT(*) FROM users WHERE" + cond).c_str());
        if (!cnt.p)
            return false;
        BindText(cnt.p, 1, like);
        if (!role.empty())
            BindText(cnt.p, 2, role);
        if (sqlite3_step(cnt.p) == SQLITE_ROW)
            total = sqlite3_column_int(cnt.p, 0);
    }

    list.clear();
    Stmt st(m_userDb,
        ("SELECT user_id, account, nickname, role, disabled, create_time, last_login_time "
         "FROM users WHERE" + cond + " ORDER BY id LIMIT ? OFFSET ?").c_str());
    if (!st.p)
        return false;
    BindText(st.p, 1, like);
    int bindIdx = 2;
    if (!role.empty())
        BindText(st.p, bindIdx++, role);
    BindInt(st.p, bindIdx++, pageSize);
    BindInt(st.p, bindIdx, (int64_t)(page - 1) * pageSize);
    while (sqlite3_step(st.p) == SQLITE_ROW)
    {
        UserRow row;
        row.userId = (uint64_t)sqlite3_column_int64(st.p, 0);
        const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
        row.account = ac ? ac : "";
        const char* nick = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
        row.nickname = nick ? nick : "";
        const char* role = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
        row.role = role ? role : "user";
        row.disabled = sqlite3_column_int(st.p, 4) != 0;
        row.registerTime = (time_t)sqlite3_column_int64(st.p, 5);
        row.lastLoginTime = (time_t)sqlite3_column_int64(st.p, 6);
        list.push_back(std::move(row));
    }
    return true;
}

bool UserModule::GetUserRow(uint64_t userId, UserRow& out)
{
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb,
        "SELECT user_id, account, nickname, role, disabled, deleted, create_time, last_login_time "
        "FROM users WHERE user_id=?");
    if (!st.p)
        return false;
    BindInt(st.p, 1, (int64_t)userId);
    if (sqlite3_step(st.p) != SQLITE_ROW)
        return false;

    out.userId = (uint64_t)sqlite3_column_int64(st.p, 0);
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 1));
    out.account = ac ? ac : "";
    const char* nick = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 2));
    out.nickname = nick ? nick : "";
    const char* role = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 3));
    out.role = role ? role : "user";
    out.disabled = sqlite3_column_int(st.p, 4) != 0;
    out.deleted = sqlite3_column_int(st.p, 5) != 0;
    out.registerTime = (time_t)sqlite3_column_int64(st.p, 6);
    out.lastLoginTime = (time_t)sqlite3_column_int64(st.p, 7);
    return true;
}

bool UserModule::SetUserDisabled(uint64_t userId, bool disabled)
{
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "UPDATE users SET disabled=? WHERE user_id=?");
        if (!st.p)
            return false;
        BindInt(st.p, 1, disabled ? 1 : 0);
        BindInt(st.p, 2, (int64_t)userId);
        sqlite3_step(st.p);
        if (m_userDb.Changes() == 0)
            return false;
    }
    if (disabled)
        RevokeAllSessions(userId);   // 锁外调用(内部自持锁)
    return true;
}

bool UserModule::SetUserDeleted(uint64_t userId, bool deleted)
{
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb, "UPDATE users SET deleted=? WHERE user_id=?");
        if (!st.p)
            return false;
        BindInt(st.p, 1, deleted ? 1 : 0);
        BindInt(st.p, 2, (int64_t)userId);
        sqlite3_step(st.p);
        if (m_userDb.Changes() == 0)
            return false;
    }
    if (deleted)
        RevokeAllSessions(userId);   // 锁外调用(内部自持锁)
    return true;
}

bool UserModule::ResetUserPassword(uint64_t userId, std::string& tempPassword)
{
    // kUserTempPassLen 位随机可打印字符(a-zA-Z0-9)
    static const char kCharset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char buf[kUserTempPassLen];
    if (RAND_bytes(buf, sizeof(buf)) != 1)
        return false;
    tempPassword.clear();
    tempPassword.reserve(sizeof(buf));
    for (unsigned char b : buf)
        tempPassword += kCharset[b % (sizeof(kCharset) - 1)];

    std::string salt, hash;
    if (!GenRandomSalt(salt) || !Pbkdf2(tempPassword, salt, hash))
        return false;

    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        Stmt st(m_userDb,
            "UPDATE users SET temp_pass_salt=?, temp_pass_hash=?, force_change=1 WHERE user_id=?");
        if (!st.p)
            return false;
        BindText(st.p, 1, salt);
        BindText(st.p, 2, hash);
        BindInt(st.p, 3, (int64_t)userId);
        sqlite3_step(st.p);
        if (m_userDb.Changes() == 0)
            return false;
    }
    RevokeAllSessions(userId);   // 锁外调用(内部自持锁)
    return true;
}

bool UserModule::SetUserNickname(uint64_t userId, const std::string& nickname,
                                 std::string& errText)
{
    std::string nn;
    if (!NfcNormalize(nickname, nn))
    {
        errText = "参数不合法";
        return false;
    }
    FieldError ne = ValidateNickname(nn);
    if (ne != FieldError::Ok)
    {
        errText = FieldErrorText(ne, "nickname");
        return false;
    }

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "UPDATE users SET nickname=? WHERE user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, nn);
    BindInt(st.p, 2, (int64_t)userId);
    sqlite3_step(st.p);
    return m_userDb.Changes() > 0;
}

bool UserModule::SetUserModules(uint64_t userId, const std::vector<std::string>& codes)
{
    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    // 事务:replace 式写入原子化,崩溃不留"授权已清空但新授权未写入"中间态
    if (!m_userDb.Begin())
        return false;

    // 校验模块 code 均存在
    for (const auto& code : codes)
    {
        Stmt chk(m_userDb, "SELECT 1 FROM modules WHERE code=? AND enabled=1");
        if (!chk.p)
        {
            m_userDb.Rollback();
            return false;
        }
        BindText(chk.p, 1, code);
        if (sqlite3_step(chk.p) != SQLITE_ROW)
        {
            m_userDb.Rollback();
            return false;
        }
    }

    // replace 式写入:清旧 + 批量插入
    {
        Stmt del(m_userDb, "DELETE FROM user_modules WHERE user_id=?");
        if (!del.p)
        {
            m_userDb.Rollback();
            return false;
        }
        BindInt(del.p, 1, (int64_t)userId);
        sqlite3_step(del.p);
    }
    for (const auto& code : codes)
    {
        Stmt ins(m_userDb,
            "INSERT OR IGNORE INTO user_modules(user_id,module_code) VALUES(?,?)");
        if (!ins.p)
        {
            m_userDb.Rollback();
            return false;
        }
        BindInt(ins.p, 1, (int64_t)userId);
        BindText(ins.p, 2, code);
        sqlite3_step(ins.p);
    }

    if (!m_userDb.Commit())
    {
        m_userDb.Rollback();
        return false;
    }
    return true;
}

bool UserModule::SetUserRole(uint64_t userId, const std::string& role)
{
    // 角色白名单(模块内防御,不依赖调用方校验)
    if (role != "user" && role != "admin" && role != "developer")
        return false;

    std::lock_guard<std::mutex> lk(m_userDb.Mutex());
    Stmt st(m_userDb, "UPDATE users SET role=? WHERE user_id=?");
    if (!st.p)
        return false;
    BindText(st.p, 1, role);
    BindInt(st.p, 2, (int64_t)userId);
    sqlite3_step(st.p);
    if (m_userDb.Changes() == 0)
        return false;

    // 授权制配套:提升为 admin 时自动授予「用户管理」「文件中心管理」(管理职能保证);
    // 降级(不再是 admin)时回收管理类授权
    if (role == "admin")
    {
        Stmt grant(m_userDb,
            "INSERT OR IGNORE INTO user_modules(user_id,module_code) VALUES(?,'userManager')");
        if (grant.p)
        {
            BindInt(grant.p, 1, (int64_t)userId);
            sqlite3_step(grant.p);
        }
        Stmt grantAdmin(m_userDb,
            "INSERT OR IGNORE INTO user_modules(user_id,module_code) VALUES(?,'filehubAdmin')");
        if (grantAdmin.p)
        {
            BindInt(grantAdmin.p, 1, (int64_t)userId);
            sqlite3_step(grantAdmin.p);
        }
    }
    else
    {
        Stmt revoke(m_userDb,
            "DELETE FROM user_modules WHERE user_id=? AND module_code IN ('userManager','filehubAdmin')");
        if (revoke.p)
        {
            BindInt(revoke.p, 1, (int64_t)userId);
            sqlite3_step(revoke.p);
        }
    }
    return true;
}

// ============================================================================
// 每日清理(独立线程:启动即执行一次 + 每日 03:00 窗口)
// ============================================================================

void UserModule::MaintainTick()
{
    // Shutdown 短路;窗口去重状态 m_lastTickDay 由事件循环线程独占
    if (m_gone.load())
        return;

    time_t now = time(nullptr);
    if (now > 0)
    {
        struct tm tmv;
        localtime_s(&tmv, &now);
        bool inWindow = (tmv.tm_hour >= kUserCleanHour && tmv.tm_hour < kUserCleanHour + 1);
        if ((m_lastTickDay == 0) || (inWindow && m_lastTickDay != tmv.tm_mday))
        {
            // 异常隔离:回调异常会击穿 event_base_loop 栈致定时器失效(替代原裸线程直接 terminate)
            try
            {
                DoCleanup();
            }
            catch (const std::exception& e)
            {
                DEFAULT_LOG_ERROR("[User] 每日清理异常(已隔离): {}", e.what());
            }
            catch (...)
            {
                DEFAULT_LOG_ERROR("[User] 每日清理未知异常(已隔离)");
            }
            m_lastTickDay = tmv.tm_mday;
        }
    }
}

void UserModule::DoCleanup()
{
    time_t now = time(nullptr);
    if (!m_openOk.load())
        return;

    // 用户库:过期会话 + 长期无动静锁定记录
    {
        std::lock_guard<std::mutex> lk(m_userDb.Mutex());
        // 拆两条独立 DELETE(OR 条件无法利用 idx_sessions_expire,全表扫;
        // 拆分后 expire_time 分支走索引,absolute_expire 分支表小可接受)
        Stmt st1(m_userDb, "DELETE FROM sessions WHERE expire_time<=?");
        if (st1.p)
        {
            BindInt(st1.p, 1, (int64_t)now);
            sqlite3_step(st1.p);
        }
        Stmt st1b(m_userDb, "DELETE FROM sessions WHERE absolute_expire<=?");
        if (st1b.p)
        {
            BindInt(st1b.p, 1, (int64_t)now);
            sqlite3_step(st1b.p);
        }
        Stmt st2(m_userDb, "DELETE FROM login_locks WHERE last_fail_at<?");
        if (st2.p)
        {
            BindInt(st2.p, 1, (int64_t)(now - kUserLockRetain));
            sqlite3_step(st2.p);
        }
    }

    // 限流库:长期无访问的 IP 行
    {
        std::lock_guard<std::mutex> lk(m_rateDb.Mutex());
        Stmt st1(m_rateDb, "DELETE FROM register_rate_limits WHERE window_start<?");
        if (st1.p)
        {
            BindInt(st1.p, 1, (int64_t)(now - kUserRateRetain));
            sqlite3_step(st1.p);
        }
        Stmt st2(m_rateDb, "DELETE FROM reset_rate_limits WHERE window_start<?");
        if (st2.p)
        {
            BindInt(st2.p, 1, (int64_t)(now - kUserRateRetain));
            sqlite3_step(st2.p);
        }
    }

    // 审计库:90 天前的操作日志
    {
        std::lock_guard<std::mutex> lk(m_auditDb.Mutex());
        Stmt st1(m_auditDb, "DELETE FROM user_manage_logs WHERE create_time<?");
        if (st1.p)
        {
            BindInt(st1.p, 1, (int64_t)(now - kUserAuditRetain));
            sqlite3_step(st1.p);
        }
        Stmt st2(m_auditDb, "DELETE FROM filehub_logs WHERE create_time<?");
        if (st2.p)
        {
            BindInt(st2.p, 1, (int64_t)(now - kUserAuditRetain));
            sqlite3_step(st2.p);
        }
    }

    DEFAULT_LOG_INFO("[User] 每日清理完成");
}
