#ifndef ZM_MODULE_SERVER_AUDIO_STREAM_H
#define ZM_MODULE_SERVER_AUDIO_STREAM_H

// ============================================================================
// ZmServerAudioStreamModule:服务器音频(权限点 serverAudioStream)
//  两条接口:
//    WS   GET /zimo/api/serverAudioStream/ws      推流(客户端 → 服务端:sync/pause/pong)
//    GET      /zimo/api/serverAudioStream/status  状态与收听信息(明细按角色下发)
//  传输模型:采集每 100ms 产一片,立即推给所有在线连接;客户端从不回拉历史 ——
//  每次(重)连接一律"从直播边缘回退 kBackfillSegs 片"起播(见设计 §3.4)。
//  采集按需启停:首个听众建连才启采,全部离场(或暂停超时)后停采并释放音频设备。
//  线程(设计 §4.4):
//    - 采集线程:OnFrame/OnTick;推送只做"锁内登记 + 锁外入队",socket 写入由每条
//      连接自己所属的事件循环完成(采集线程回调不得阻塞,且跨线程发送会乱序)。
//    - 工作池:BootstrapWs 的完整鉴权与 EnsureReady(设备枚举/启动是数百毫秒的系统调用)。
//    - 事件循环:WS 回调(onOpen/onMessage/onClose)、/status、每 30 秒的鉴权复检。
//  设计见《2026-09-21-服务器音频WebSocket推流设计.md》。
// ============================================================================

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <zm_util_json.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class ZmHttpRestfulServer;
class ZmSessionModule;
class ZmPermissionModule;
class ZmAuthGateModule;
class ZmAudioCapturer;
class ZmWebmMuxer;
struct ZmGateResult;
struct ZmSessionCtx;

namespace trantor
{
class EventLoop;
}

class ZmServerAudioStreamModule
{
  public:
    /**
     * @brief 构造并注入依赖(不启动采集;采集在首次建连引导时懒启动)
     *
     * @param rest       RESTful 面(路由与 WS 端点注册用)
     * @param session    会话模块(引导鉴权与周期复检)
     * @param permission 权限模块(判定 serverAudioStream)
     * @param gate       门禁模块(账号状态边界与错误响应助手)
     */
    ZmServerAudioStreamModule(ZmHttpRestfulServer* rest, ZmSessionModule* session,
                              ZmPermissionModule* permission, ZmAuthGateModule* gate);
    ~ZmServerAudioStreamModule();

    ZmServerAudioStreamModule(const ZmServerAudioStreamModule&) = delete;
    ZmServerAudioStreamModule& operator=(const ZmServerAudioStreamModule&) = delete;

    /// @brief 注册 WS 端点与 /status,并挂上鉴权复检定时器(须先于 ZmHttpServer::Open)
    void RegisterRoutes();

    /// @brief 登记权限点 serverAudioStream(幂等;默认挂 developer)
    void RegisterPermissions();

    /// @brief 业务收尾:断开全部连接、停采集线程并释放音频设备(须在销毁网络层之前调用)
    void Shutdown();

    // ── 策略常量(设计 §4.9) ──
    static constexpr int     kReadyWaitMs        = 1000;    ///< 握手等待首片就绪上限(毫秒)
    static constexpr int     kReleaseWaitMs      = 3000;    ///< 等待上一次采集释放设备上限(毫秒)
    static constexpr int64_t kIdleGraceMs        = 15000;   ///< 无人时的停采宽限(毫秒)
    static constexpr int64_t kPingIntervalMs     = 15000;   ///< 心跳问询间隔(毫秒)
    static constexpr int64_t kHeartbeatTimeoutMs = 120000;  ///< 无应答断开阈值(毫秒)
    static constexpr int64_t kPauseHoldMs        = 60000;   ///< 暂停保持上限(毫秒;超时释放设备)
    static constexpr int64_t kBootTimeoutMs      = 10000;   ///< 引导(含 sync)完成上限(毫秒)
    static constexpr double  kRecheckSec         = 30.0;    ///< 鉴权复检周期(秒)
    static constexpr uint64_t kBackfillSegs      = 5;       ///< 起播补发片数(≈200ms;客户端起播门槛 ≥0.12s,5 片过线)
    static constexpr size_t  kRingSegments       = 25;      ///< 起播缓冲窗口(≈1 秒:片长变了这里要跟着变)
    static constexpr int     kDetailLevel        = 2;       ///< /status 明细可见等级(admin=2)

  private:
    /// @brief 待发送的一帧(binary = false 时为 UTF-8 文本帧)
    struct OutFrame
    {
        bool                                 binary = true;
        std::shared_ptr<const std::vector<uint8_t>> body;   ///< 帧体(含 binary 头 + 载荷)
    };

    /**
     * @brief 一条在线连接
     *
     * 引导完成前用临时键登记(只收心跳),收到 sync 后改写为 "uid:client" 并置 listening。
     */
    struct WsClient
    {
        drogon::WebSocketConnectionPtr conn;
        trantor::EventLoop*            loop = nullptr;   ///< 连接所属事件循环(发送只排到这里)
        int64_t     uid = 0;
        std::string account;
        std::string nickname;
        std::string ip;
        std::string cookie;                              ///< 复检用;不写日志
        std::string client;                              ///< 客户端标识(来自 sync)
        int         level = 0;
        bool        listening = false;                   ///< 是否已收 sync(未收则只心跳、不推送)
        bool        bootstrapped = false;                ///< 引导(鉴权 + 采集就绪)是否已完成
        bool        pendingSync = false;                 ///< 引导完成前收到的 sync(待补做重定基)
        bool        paused = false;                      ///< 暂停:保留连接但不推送
        uint64_t    pushedUptoSeq = 0;                   ///< 已排入发送队列的最新片号(防重复推)
        uint64_t    lastSeq = 0;                         ///< pong 报告的最新片号(仅参考)
        int64_t     lastLagMs = -1;                      ///< 上报那一刻算好的落后量;-1 = 尚未上报
        int64_t     lastPongMs = 0;                      ///< 最近一次 pong(心跳兜底)
        int64_t     pausedAtMs = 0;                      ///< 暂停起点(超 kPauseHoldMs 断开)
        int64_t     sinceMs = 0;                         ///< 建连时刻
    };

    // ── 路由 handler(请求按值收:协程懒启动,引用形参会悬空) ──
    drogon::Task<drogon::HttpResponsePtr> HandleStatus(drogon::HttpRequestPtr req);

    // ── 鉴权 ──
    /// @brief 升级快判:只验 zm_session 形态(同步回调,不能查库)
    static bool HasSessionCookie(const drogon::HttpRequestPtr& req);
    /// @brief 会话 + 账号状态 + 权限点 serverAudioStream(引导与周期复检共用)
    drogon::Task<ZmGateResult> AuthorizeByCookie(const std::string& cookie, const std::string& ip);
    /// @brief 同上,取 cookie 与来源 IP 后转发(HTTP 面用)
    drogon::Task<ZmGateResult> Authorize(const drogon::HttpRequestPtr& req);

    // ── WS 生命周期(连接所属事件循环线程) ──
    void BootstrapWs(const drogon::WebSocketConnectionPtr& conn, const drogon::HttpRequestPtr& req);
    /// @brief 引导协程:完整鉴权 → 采集就绪 →(若 sync 已到)重定基
    drogon::Task<void> BootstrapTask(drogon::WebSocketConnectionPtr conn, std::string key);
    void OnWsMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& msg,
                     drogon::WebSocketMessageType type);
    void OnWsClose(const drogon::WebSocketConnectionPtr& conn);

    /// @brief 重定基(锁内,含入队):选定发起点、下发 meta/init/补发批,并让实时推送接上
    /// @return true 成功;false = 采集未跑(调用方发 error 帧并断开)
    bool RebaseLocked(WsClient& wc);

    /**
     * @brief 把一帧排入该连接所属的事件循环(非阻塞)
     *
     * 只做"把闭包放进目标循环的任务队列",真正的 socket 写入由该循环串行完成 ——
     * 这是同一连接发送保序的前提(设计 §4.4 约束 3)。
     */
    static void SendFrame(const drogon::WebSocketConnectionPtr& conn, trantor::EventLoop* loop,
                          OutFrame frame);
    /**
     * @brief 先发一帧再断开(帧一定先于关闭帧发出)
     *
     * 两者都排进连接所属的事件循环:若直接 shutdown,关闭帧会当场发出,而排在队列里的
     * 业务帧(如 auth_failed)反而落在后面 —— 客户端只能看到"连接被关",看不到原因。
     */
    static void SendFrameThenClose(const drogon::WebSocketConnectionPtr& conn,
                                   trantor::EventLoop* loop, OutFrame frame,
                                   drogon::CloseCode code, const char* reason);
    /// @brief 组装文本帧(JSON)
    static OutFrame TextFrame(const ZMJSON& json);
    /// @brief 组装 binary 帧(类型字节 + 起始片号 + 载荷)
    static OutFrame BinaryFrame(uint8_t type, uint64_t startSeq, const std::vector<uint8_t>& payload);
    /// @brief 组装分片批帧(帧头与批载荷一次成型,不做二次拷贝)
    static OutFrame BatchFrame(uint64_t startSeq, const std::vector<const std::vector<uint8_t>*>& segs);

    // ── 采集生命周期(内部加锁;EnsureReady 会阻塞,须在工作池线程调用) ──
    /**
     * @brief 确保采集在跑且首片就绪(必要时启动;失败给出原因)
     *
     * 设备枚举、格式协商与引擎启动是数百毫秒的系统调用,调用方须经工作池执行,
     * 不得在事件循环与采集线程上直接调用。
     *
     * @param errMsg    [out] 失败原因(展示给用户)
     * @param transient [out] true = 暂时不可用(设备正在释放/启动超时)应让客户端重试;
     *                        false = 服务端确实没有可用设备,重试无意义
     * @return true 采集在跑且有片可下发
     */
    bool EnsureReady(std::string& errMsg, bool& transient);
    /// @brief 帧回调(采集线程):封装成簇 → 入起播缓冲 → 登记待推片
    void OnFrame(const uint8_t* data, size_t len, uint64_t ptsMs);
    /// @brief 周期回调(采集线程):心跳/存活扫描 + 判断停采
    void OnTick();
    /// @brief 采集线程收尾回调(采集线程):复位状态并唤醒等待者
    void OnCaptureDone();
    /// @brief 停采集并等待线程退出(幂等;不得持 m_mutex 调用,也不得在采集线程上调用)
    void StopCapture();

    // ── 心跳、存活与复检 ──
    /// @brief 心跳与超时扫描(锁内):发 ping、断开无应答/暂停超时/引导超时的连接
    void HeartbeatLocked(int64_t nowMs);
    /// @brief 摘除一条连接(锁内):从注册表删除并断开(原因仅日志用)
    /// @param frame 非空时先把它发出去再断开(保证客户端看得到原因)
    void KickLocked(const std::string& key, const char* reason, drogon::CloseCode code,
                    OutFrame frame = OutFrame{});
    /// @brief 周期鉴权复检:不通过的连接发 auth_failed 后断开(事件循环定时器驱动)
    drogon::Task<void> RecheckAll();

    /// @brief 按连接取回其注册键(连接上下文里存;未登记返回空串)
    static std::string KeyOfConn(const drogon::WebSocketConnectionPtr& conn);
    /// @brief 注册键:uid:client
    static std::string KeyOf(int64_t uid, const std::string& client);
    /**
     * @brief 把连接从临时键改写到 uid:client(锁内)
     *
     * 同一个 uid:client 的另一条连接可能还挂着(网络掉线但心跳未到期),先断开它再顶掉 ——
     * 否则它的 onClose 迟到时会按同一个键把新条目删掉。
     *
     * @return 新键上的迭代器;失败返回 m_wsClients.end()
     */
    std::unordered_map<std::string, WsClient>::iterator MoveToKeyLocked(const std::string& oldKey,
                                                                        const std::string& newKey);
    /**
     * @brief 绑定听众身份并重定基(锁内):改注册键为 uid:client → 从直播边缘重定基
     *
     * 首次同步(引导完成前到达的 sync,由引导收尾补做)与之后的任何一次同步(重连、
     * 暂停恢复、自查重建)都走这里。改键与重定基必须同处一个临界区:同键的半开旧连接
     * 要在此刻被踢掉(否则它会在表里躺到心跳超时,既被重复计数又白吃推流),
     * 实时推送也只能推补发之后的新片。
     *
     * @param it     当前条目(引导期为临时键;函数内可能因改键而移动)
     * @param client 客户端标识(来自 sync;调用方已校验形态)
     * @return true 已绑定;false = 条目已消失(调用方直接返回)
     */
    bool RebindLocked(std::unordered_map<std::string, WsClient>::iterator it,
                      const std::string& client);

    ZmHttpRestfulServer* m_rest = nullptr;
    ZmSessionModule*     m_session = nullptr;
    ZmPermissionModule*  m_permission = nullptr;
    ZmAuthGateModule*    m_gate = nullptr;

    mutable std::mutex      m_mutex;
    std::condition_variable m_readyCv;   ///< 首片就绪/采集退出的通知

    // 流状态(全部仅锁内访问)
    std::string          m_streamId;      ///< 流标识(每次采集启动重新生成)
    std::vector<uint8_t> m_initSegment;   ///< WebM 初始化段(采集就绪后填充)
    std::unique_ptr<ZmWebmMuxer> m_muxer; ///< 存片期间的封装器
    /// 起播缓冲:<片号, 簇字节>(尾部最新;窗口见 kRingSegments)
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> m_segments;
    uint64_t m_startSeq = 0;              ///< 窗口头部片号
    uint64_t m_liveSeq = 0;               ///< 直播边缘片号
    uint64_t m_nextSeq = 1;               ///< 下一个待分配的片号
    bool     m_running = false;           ///< 采集在跑
    bool     m_ready = false;             ///< 已有可下发的片
    bool     m_stopRequested = false;     ///< 已请求停采(采集线程收尾中)
    bool     m_shuttingDown = false;      ///< 模块正在关停(不再启动采集)
    std::string m_captureErr;             ///< 最近一次采集失败原因
    int64_t  m_lastActivityMs = 0;        ///< 最近一次"有人要听"的时刻
    int64_t  m_lastSweepMs = 0;           ///< 最近一次存活扫描的时刻
    int64_t  m_lastPingMs = 0;            ///< 最近一次发心跳的时刻

    std::unique_ptr<ZmAudioCapturer> m_capturer;                  ///< 采集器(退出后由下次启动回收)
    std::unordered_map<std::string, WsClient> m_wsClients;        ///< 键 uid:client(引导期为临时键)
    uint64_t m_connSeq = 0;                                       ///< 临时键自增(仅锁内)
};

#endif // ZM_MODULE_SERVER_AUDIO_STREAM_H
