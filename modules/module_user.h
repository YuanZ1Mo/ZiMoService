#ifndef MODULE_USER_H
#define MODULE_USER_H

#include "zm_net_req_loop_protocol.h"   // ZMJSON / ZmReqLoop / ZmReqLoopRest
#include "zm_net_http.h"                // ZmHttpdTask / evhttp_cmd_type

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "zm_util_sqlite.h"

class HttpFrontendManager;
class ZmEvBaseRunLoop;   // 后台周期维护事件循环(成员指针,前向声明)

// ============================================================================
// 用户系统常量(设计文档 2.9;联调测试期可临时改短,验收后复原)
// ============================================================================

/** 锁定:每 5 次失败升一档 */
static constexpr int64_t kUserLockTierStep = 5;
/** 锁定:档1 时长(秒) */
static constexpr int64_t kUserLockDurTier1 = 15 * 60;
/** 锁定:档2+ 时长(秒,封顶) */
static constexpr int64_t kUserLockDurTierN = 60 * 60;
/** 限流:每窗口允许次数 */
static constexpr int kUserRateLimit = 20;
/** 限流:窗口长度(秒) */
static constexpr int64_t kUserRateWindow = 60;
/** 会话:滑动有效期(秒,30 天) */
static constexpr int64_t kUserSessionSliding = 30LL * 24 * 3600;
/** 会话:绝对上限(秒,90 天) */
static constexpr int64_t kUserSessionAbsolute = 90LL * 24 * 3600;
/** 会话:每账号活跃会话上限 */
static constexpr int kUserMaxSessions = 5;
/** 昵称最大码点数(ValidateNickname 与注册默认昵称共用) */
static constexpr int kUserNicknameMaxCp = 20;
/** 管理员重置的临时密码长度 */
static constexpr int kUserTempPassLen = 12;
/** 用户列表:默认页大小 / 页大小上限 */
static constexpr int kUserPageSizeDefault = 20;
static constexpr int kUserPageSizeMax = 100;
/** 清理/校验线程轮询间隔(秒) */
static constexpr int kUserCleanPollSec = 60;
/** 用户唯一身份 user_id:起始值(10000001)/封顶值(99999999),注册时 MAX+1 递增生成 */
static constexpr int64_t kUserIdMin = 10000001;
static constexpr int64_t kUserIdMax = 99999999;
/** 会话:续期落库节流(秒) */
static constexpr int64_t kUserTouchThrottle = 60;
/** 密码学:PBKDF2 迭代次数 */
static constexpr int kUserPbkdf2Iter = 600000;
/** 清理:每日清理时刻(时) */
static constexpr int kUserCleanHour = 3;
/** 清理:锁定记录保留(秒,7 天;锁定期满计数保留供阶梯,仅清长期无动静) */
static constexpr int64_t kUserLockRetain = 7LL * 24 * 3600;
/** 清理:限流行保留(秒,7 天) */
static constexpr int64_t kUserRateRetain = 7LL * 24 * 3600;
/** 清理:操作日志保留(秒,90 天) */
static constexpr int64_t kUserAuditRetain = 90LL * 24 * 3600;
/** Cookie:会话 cookie 名 */
static constexpr const char* kUserCookieName = "zm_session";
/** Cookie:Max-Age(秒,90 天,覆盖会话绝对上限) */
static constexpr int64_t kUserCookieMaxAge = 90LL * 24 * 3600;

/**
 * @brief 用户账号模块(业务层)
 *
 * 承载用户系统:登录/注册/找回密码 + 会话管理 + 账号+IP 锁定 + 注册/重置限流 + 每日清理。
 * 双 SQLite 库:用户库(db/user/user.db:users/sessions/login_locks)+
 * 通用限流库(db/rate/rate.db:register_rate_limits/reset_rate_limits,后续模块可复用)。
 * 代码层建表(CREATE TABLE IF NOT EXISTS,Open 时执行)。
 *
 * 线程模型:请求经 ZmReqLoopPool 分发并发到达,库访问以各自 mutex 串行化;
 * PBKDF2 计算在锁外(先锁内取盐,锁外计算,锁内比较);每日清理为独立后台线程。
 * 路由挂接:service_portal.cpp 只增不改 —— RegisterHttpRoutes 由本模块自注册,
 * REST 分发经 DispatchRest 命中返回 true。
 */
class UserModule
{
public:
    /** @brief 注入已初始化连接(DbInitializer 打开并建表/补列后传入;本模块不拥有连接) */
    UserModule(zm::ZmSqliteConn& userDb, zm::ZmSqliteConn& rateDb, zm::ZmSqliteConn& auditDb);
    ~UserModule();

    /** @brief 启动:检查三库可用 + 起每日清理线程;失败返回 false 并记日志(服务可继续跑,仅用户系统不可用) */
    bool Open();

    /** @brief 停止钩子:置 gone + join 清理线程(库连接由 DbInitializer 统一关闭) */
    void Shutdown();

    /** @brief 模块自注册静态页路由 /login /register /reset(service_portal.cpp 调用一行) */
    void RegisterHttpRoutes(HttpFrontendManager* httpMgr);

    /**
     * @brief REST 分发入口(service_portal.cpp 的 else 链最前端调用)
     * @return true 本模块已处理(含错误响应);false 未命中,走 portal 原逻辑
     */
    bool DispatchRest(ZmReqLoop* loop, evhttp_cmd_type verb, const std::string& path,
                      ZmHttpdTask* task, const BYTE* body, size_t bodyLen);

    /** @brief 已认证用户信息(供后续模块复用:鉴权即续期) */
    struct UserInfo
    {
        uint64_t    userId = 0;
        std::string account;
        std::string nickname;
    };

    /**
     * @brief 后续模块通用鉴权:解析会话 cookie 并续期
     * @param task 请求上下文
     * @param out  命中时输出用户信息(可为空)
     * @return 命中返回 user_id(已续期);未认证返回 0
     */
    uint64_t AuthAndTouch(ZmHttpdTask* task, UserInfo* out = nullptr);

    /** @brief 查询用户角色(developer/admin/user),失败返回 false 并输出默认 user */
    bool GetUserRole(uint64_t userId, std::string& role);

    /** @brief 按 id 查询账号(展示用,如文件上传者);不存在返回 false */
    bool GetUserAccount(uint64_t userId, std::string& account);

    /** @brief 通用前置检查结果(调用方据此响应 401/403/500) */
    enum class AuthResult { Ok, Unauthed, Forbidden, Error };

    /**
     * @brief 通用鉴权 + 模块权限前置(替代各模块复制粘贴的 ~30 行前置块:
     *        鉴权 → 角色 → 可见模块须含 moduleCode)
     * @param task        请求上下文
     * @param moduleCode  所需模块 code(如 "serverAudioStream"/"filehub")
     * @param out         鉴权命中时输出用户信息(可为空)
     * @return Ok 已授权;Unauthed 会话无效;Forbidden 已登录但无该模块权限;Error 系统错误
     */
    AuthResult RequireModule(ZmHttpdTask* task, const char* moduleCode, UserInfo* out = nullptr);

    /**
     * @brief 批量按 id 查询账号(展示用,如文件上传者/创建者);
     *        一次 IN 查询替代逐行 GetUserAccount(N+1 消除)
     * @param out 输出 id → account;不存在的 id 不在 map 中
     */
    bool BatchGetUserAccounts(const std::vector<uint64_t>& ids,
                              std::map<uint64_t, std::string>& out);

    /** @brief 角色等级:developer=3 / admin=2 / user=1 / 未知=0(等级校验用) */
    static int GetUserRoleLevel(const std::string& role);

    /**
     * @brief 写入用户管理操作日志(业务日志库 db/audit/audit.db 的 user_manage_logs 表)
     * @param detail 操作细节 JSON 字符串(如 {role:"admin"}、{modules:[...]})
     * @return false 落库失败(已记 ERROR 日志,安全操作审计缺失可追溯)
     */
    bool WriteAuditLog(uint64_t operatorId, const std::string& operatorAccount,
                       const std::string& action, uint64_t targetId,
                       const std::string& targetAccount, const std::string& detail);

    /** @brief 业务日志表(枚举替代运行时表名拼接,防误传) */
    enum class BizLogTable { FileHubLogs };

    /**
     * @brief 写入业务操作日志(业务日志库,结构含 target_type 列)
     * @param table 表枚举(内部映射表名)
     * @param detail 操作细节 JSON 字符串
     */
    bool WriteBusinessLog(BizLogTable table, uint64_t opId, const std::string& opAccount,
                          const std::string& action, const std::string& targetType,
                          uint64_t targetId, const std::string& detail);

    /**
     * @brief 查询用户可见模块列表(developer 全量;admin/user 为 home + 授权项),按 sort 排序
     * @param modules 输出 [{code,name,url}, ...]
     * @return false 查询失败
     */
    bool GetUserModules(uint64_t userId, const std::string& role, std::vector<ZMJSON>& modules);

    // ========================================================================
    // 用户管理数据操作(由 PortalModule 路由/校验后调用)
    // ========================================================================

    /** 用户列表行 */
    struct UserRow
    {
        uint64_t    userId = 0;   ///< 用户唯一身份(users.user_id,非行号 id;对外映射一律用它)
        std::string account;
        std::string nickname;
        std::string role;
        bool        disabled = false;
        bool        deleted = false;
        time_t      registerTime = 0;
        time_t      lastLoginTime = 0;
    };

    /**
     * @brief 分页查询用户列表(不含软删除)
     * @param keyword 账号模糊关键字
     * @param role    角色筛选(空 = 全部;developer/admin/user)
     * @param status  状态筛选(0 = 全部;1 = 正常;2 = 已停用)
     */
    bool ListUsers(const std::string& keyword, const std::string& role, int status,
                   int page, int pageSize, int& total, std::vector<UserRow>& list);

    /** @brief 按 id 查询用户(含 deleted/disabled,供目标校验);不存在返回 false */
    bool GetUserRow(uint64_t userId, UserRow& out);

    /** @brief 停用/启用(disable 时吊销其全部会话) */
    bool SetUserDisabled(uint64_t userId, bool disabled);

    /** @brief 软删除/恢复(删除时吊销其全部会话) */
    bool SetUserDeleted(uint64_t userId, bool deleted);

    /**
     * @brief 管理员重置密码:生成 12 位随机临时密码 → 写 temp_pass + force_change=1 → 吊销全部会话
     * @param tempPassword 输出临时密码明文(仅此一次返回)
     */
    bool ResetUserPassword(uint64_t userId, std::string& tempPassword);

    /** @brief 修改昵称(NFC + 规则校验,失败返回具体文案);返回 false 且 errText 非空 */
    bool SetUserNickname(uint64_t userId, const std::string& nickname, std::string& errText);

    /** @brief replace 式写入授权模块(codes 需全部存在于 modules 表) */
    bool SetUserModules(uint64_t userId, const std::vector<std::string>& codes);

    bool SetUserRole(uint64_t userId, const std::string& role);

    /** @brief 吊销用户全部会话(用户管理操作后调用) */
    void RevokeAllSessions(uint64_t userId);

private:
    // ========================================================================
    // 密码学与编码
    // ========================================================================

    /** @brief NFC 归一化(UTF-8 → UTF-16 → NormalizeString(NFC) → UTF-8);失败 false */
    static bool NfcNormalize(const std::string& utf8, std::string& out);

    /** @brief UTF-8 码点计数;非法序列返回 false */
    static bool CountCodePoints(const std::string& utf8, size_t& count);

    /** @brief 码点是否可打印(排除控制/空白类,见设计 2.3) */
    static bool IsPrintableCodePoint(uint32_t cp);

    static std::string ToLowerAscii(const std::string& s);

    /** @brief PBKDF2-HMAC-SHA256(iter=kUserPbkdf2Iter,dklen=32) */
    static bool Pbkdf2(const std::string& password, const std::string& salt, std::string& out32);

    static bool GenRandomSalt(std::string& out16);

    /** @brief 生成会话 token:32B 随机 → hex64 */
    static bool GenSessionToken(std::string& hex64);

    static bool Sha256Hex(const std::string& in, std::string& hex64);

    // ========================================================================
    // 规则校验(NFC 后输入;账号/救援码成功时输出小写)
    // ========================================================================

    enum class FieldError { Ok, Empty, Length, Charset, Edge };

    static FieldError ValidateAccount(std::string& lowered);
    static FieldError ValidatePassword(const std::string& pwd);
    static FieldError ValidateNickname(const std::string& nick);
    static FieldError ValidateRescue(std::string& lowered);

    /** @brief 校验错误 → 400 具体文案(按字段) */
    static const char* FieldErrorText(FieldError e, const char* field);

    // ========================================================================
    // 锁定(login_locks,用户库;维度 = 账号+IP,阶梯递增)
    // ========================================================================

    /** @brief 锁定中(locked_until > now)?不校验、不计数 */
    bool IsLocked(const std::string& account, const std::string& ip, time_t now);

    /** @brief 失败 +1,每满 kUserLockTierStep 触发锁定(档1=15min,档2+=60min) */
    void RecordFail(const std::string& account, const std::string& ip, time_t now);

    void ClearLock(const std::string& account, const std::string& ip);

    // ========================================================================
    // 限流(rate 库;注册/重置各一表,窗口滑动,落库持久化)
    // ========================================================================

    /** @brief 限流检查;超限返回 false(计数保留,窗口滑过自然恢复) */
    bool CheckRate(const char* table, const std::string& ip, time_t now);

    // ========================================================================
    // 会话(sessions,用户库)
    // ========================================================================

    struct SessionRow
    {
        uint64_t    id = 0;
        uint64_t    userId = 0;
        std::string account;
        std::string nickname;
        std::string createIp;
        time_t      createTime = 0;
        time_t      lastActive = 0;
        std::string lastLoginIp;    ///< 账号最近一次登录 IP(users 表,账号维度,与当前会话解耦)
        time_t      lastLoginTime = 0;   ///< 账号最近一次登录时间
    };

    /** @brief 签发新会话(会话轮换:每次登录/注册/重置全新 token);超上限按 LRU 踢最旧 */
    bool CreateSession(const std::string& token, uint64_t userId, const std::string& ip, time_t now);

    /** @brief 按 token_hash 查有效会话(双上限 + 账号未停用/未删除)并 JOIN 用户信息 */
    bool FindSession(const std::string& token, time_t now, SessionRow& out);

    /** @brief 续期(last_active/expire_time);距上次 < kUserTouchThrottle 跳过落库 */
    void TouchSession(uint64_t sessionId, time_t now, time_t lastActive);

    /**
     * @brief 会话签发尾部(登录/注册/重置共用):签发全新 token → 种 cookie → 查回 →
     *        组装 user 响应;失败时已发出 500 响应,调用方直接 return
     * @param fallbackAccount/Nickname 查回缺失时的兜底(如登录请求刚注册的昵称);空=不兜底
     * @param forceChangeFlag 强制改密标记(前端据此跳改密页)
     */
    void RespondWithNewSession(ZmReqLoop* loop, ZmHttpdTask* task, uint64_t userId,
                               const std::string& ip, time_t now,
                               const std::string& fallbackAccount,
                               const std::string& fallbackNickname, bool forceChangeFlag);

    /** @brief 从 Cookie 头解析 zm_session= 值 */
    static bool ExtractCookieToken(ZmHttpdTask* task, std::string& token);

    // ========================================================================
    // 响应辅助
    // ========================================================================

    static void SetSessionCookie(ZmHttpdTask* task, const std::string& token);
    static void ClearSessionCookie(ZmHttpdTask* task);

    /** @brief 组装 user 信息 JSON(account/nickname/最后登录IP/时间/活动时间) */
    static void FillUserResult(ZMJSON& user, const SessionRow& s);

    // ========================================================================
    // API handlers(在 ZmReqLoop 线程执行)
    // ========================================================================

    void HandleLogin(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task, const std::string& ip);
    void HandleCompleteChange(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task);
    void HandleRegister(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task, const std::string& ip);
    void HandleReset(ZmReqLoop* loop, const ZMJSON& body, ZmHttpdTask* task, const std::string& ip);
    void HandleLogout(ZmReqLoop* loop, ZmHttpdTask* task);
    void HandleMe(ZmReqLoop* loop, ZmHttpdTask* task);
    void HandleHeartbeat(ZmReqLoop* loop, ZmHttpdTask* task);

    // ========================================================================
    // 后台周期维护(独立事件循环线程:过期会话/锁定/限流/操作日志清理)
    // ========================================================================

    /** @brief 定时器到期处理(事件循环线程):每日窗口检查 + 清理,异常隔离 */
    void MaintainTick();
    void DoCleanup();

private:
    zm::ZmSqliteConn& m_userDb;       ///< 用户库(users/sessions/login_locks)连接引用(归 DbInitializer 所有)
    zm::ZmSqliteConn& m_rateDb;       ///< 通用限流库(register/reset_rate_limits)
    zm::ZmSqliteConn& m_auditDb;      ///< 业务日志库(用户管理 user_manage_logs;文件中心 filehub_logs 复用本库)

    ZmEvBaseRunLoop* m_bgLoop = nullptr;   ///< 后台周期维护事件循环(Open 创建,Shutdown 停止)
    int m_lastTickDay = 0;                 ///< 上次维护日(窗口去重,事件循环线程独占)
    std::atomic<bool> m_gone {false};        ///< Shutdown 已开始:清理回调短路 + handler 短路
    std::atomic<bool> m_openOk {false};
};

#endif // MODULE_USER_H
