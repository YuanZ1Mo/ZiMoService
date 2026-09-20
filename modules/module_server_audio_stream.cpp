#include "modules/module_server_audio_stream.h"

#include "modules/module_gate.h"
#include "modules/module_permission.h"
#include "modules/module_session.h"
#include "modules/util/audio_capturer.h"
#include "modules/util/webm_muxer.h"

#include <drogon/HttpAppFramework.h>

#include "zm_net_http_server.h"
#include "zm_net_http_restful_server.h"
#include "zm_util_logger.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

// ============================================================================
// 内部常量与工具
// ============================================================================
namespace
{
/// 采集与下发的固定形态
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 2;
constexpr uint32_t kBitrate = 64000;
/// 分片时长(毫秒;与 ZmWebmMuxer 的每片帧数对齐 —— 改封装参数这里自动跟随)
constexpr uint32_t kSegmentMs = ZmWebmMuxer::kFramesPerSegment * ZmWebmMuxer::kFrameMs;
/// 存活扫描间隔(毫秒;采集线程每周期回调一次)
constexpr int64_t kSweepIntervalMs = 200;
/// 引导完成前的临时注册键前缀(收到 sync 后改写为 uid:client)
constexpr const char* kBootKeyPrefix = "__conn";

constexpr const char* kPermCode = "serverAudioStream";
constexpr const char* kRoutePrefix = "/zimo/api/serverAudioStream";

/// @return 当前时刻(毫秒;仅用于相对比较)
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief 生成流标识(采集启动时重新生成,客户端据此识别换流)
std::string NewStreamId()
{
    static std::atomic<uint32_t> counter{0};
    const uint64_t               ms = static_cast<uint64_t>(NowMs());
    char                         buf[32] = {0};
    std::snprintf(buf, sizeof(buf), "%llx-%u", static_cast<unsigned long long>(ms),
                  counter.fetch_add(1) + 1);
    return std::string(buf);
}

/// @brief 客户端标识校验:1~32 个 [A-Za-z0-9_-]
bool ValidClientId(const std::string& s)
{
    if (s.empty() || s.size() > 32)
        return false;
    for (char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/// @brief 追加 32 位小端整数(分片批的封装格式)
void PutLE32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

/// @brief 追加 64 位小端整数(binary 帧头里的起始片号)
void PutLE64(std::vector<uint8_t>& v, uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

} // namespace

// ============================================================================
// 构造 / 析构 / 注册
// ============================================================================
ZmServerAudioStreamModule::ZmServerAudioStreamModule(ZmHttpRestfulServer* rest,
                                                     ZmSessionModule* session,
                                                     ZmPermissionModule* permission,
                                                     ZmAuthGateModule* gate)
    : m_rest(rest), m_session(session), m_permission(permission), m_gate(gate)
{
}

ZmServerAudioStreamModule::~ZmServerAudioStreamModule()
{
    Shutdown();
}

void ZmServerAudioStreamModule::RegisterRoutes()
{
    if (!m_rest)
    {
        DEFAULT_LOG_ERROR("ZmServerAudioStreamModule: RESTful 面为空,路由未注册");
        return;
    }

    // ── 推流端点:同一 HTTPS 监听器(同端口同证书),客户端 cookie 随升级请求携带 ──
    ZmHttpServer::WsCallbacks cb;
    cb.onAuth = [](const drogon::HttpRequestPtr& req) { return HasSessionCookie(req); };
    cb.onOpen = [this](const drogon::WebSocketConnectionPtr& c, const drogon::HttpRequestPtr& req)
    { BootstrapWs(c, req); };
    cb.onMessage = [this](const drogon::WebSocketConnectionPtr& c, std::string&& m,
                          drogon::WebSocketMessageType t) { OnWsMessage(c, std::move(m), t); };
    cb.onClose = [this](const drogon::WebSocketConnectionPtr& c) { OnWsClose(c); };
    m_rest->RegisterWebSocket(std::string(kRoutePrefix) + "/ws", cb);

    // ── 状态与收听信息(非媒体面) ──
    m_rest->RegisterCoro(std::string(kRoutePrefix) + "/status", drogon::HttpMethod::Get,
                         [this](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr>
                         { return HandleStatus(std::move(req)); });

    // ── 周期鉴权复检:推流下客户端不再周期性发请求,权限撤销/停用必须靠复检收敛 ──
    drogon::app().registerBeginningAdvice(
        [this]()
        {
            trantor::EventLoop* loop = drogon::app().getLoop();
            if (!loop)
                return;
            loop->runEvery(kRecheckSec,
                           [this]()
                           {
                               drogon::async_run([this]() -> drogon::Task<void>
                                                 { co_await RecheckAll(); });
                           });
            DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 鉴权复检已注册(每 {} 秒)",
                             static_cast<int>(kRecheckSec));
        });

    DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 推流端点与状态接口已注册({}/ws, {}/status)",
                     kRoutePrefix, kRoutePrefix);
}

void ZmServerAudioStreamModule::RegisterPermissions()
{
    if (!m_permission)
        return;
    ZMJSON perm = ZMJSON::object();
    perm["code"] = kPermCode;
    perm["name"] = "服务器音频";
    perm["module"] = "portal";
    perm["url"] = "/portal/server-audio-stream";
    perm["type"] = 0;    // 门户模块:进侧边栏
    perm["index"] = 3;   // 用户主页(1)、文件中心(2)之后
    perm["sort"] = 6;
    perm["enabled"] = 1;
    perm["description"] = "服务器音频门户模块";
    // 默认只授予开发者;其余角色由管理员在授权弹窗按需授予
    if (!m_permission->RegisterPermCodeSync(perm, {"developer"}))
        DEFAULT_LOG_ERROR("ZmServerAudioStreamModule: 权限点登记失败({})", kPermCode);
    else
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 权限点已登记({})", kPermCode);
}

void ZmServerAudioStreamModule::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_shuttingDown)
            return;   // 幂等
        m_shuttingDown = true;
        m_stopRequested = true;
        for (auto& [key, wc] : m_wsClients)
            SendFrameThenClose(wc.conn, wc.loop, OutFrame{}, drogon::CloseCode::kEndpointGone,
                               "server shutting down");
        m_wsClients.clear();
    }
    StopCapture();
}

// ============================================================================
// 鉴权:升级快判 + 完整判定(引导与周期复检共用)
// ============================================================================
bool ZmServerAudioStreamModule::HasSessionCookie(const drogon::HttpRequestPtr& req)
{
    if (!req)
        return false;
    // 同步回调不能查库:只验形态(明文 token 为 64 位小写 hex),真正的判定交给引导
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    if (cookie.size() < 16 || cookie.size() > 128)
        return false;
    for (char c : cookie)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

drogon::Task<ZmGateResult> ZmServerAudioStreamModule::AuthorizeByCookie(const std::string& cookie,
                                                                         const std::string& ip)
{
    ZmGateResult r;
    r.ctx              = co_await m_session->AuthAndTouch(cookie, ip);
    const ZmGateResult g = ZmAuthGateModule::CheckCtxSync(r.ctx, kRoutePrefix);
    if (!g.ok)
    {
        r.status  = g.status;
        r.code    = g.code;
        r.message = g.message;
        co_return r;
    }
    if (!(co_await m_permission->HasPermission(r.ctx.uid, std::string(kPermCode))))
    {
        r.status  = 403;
        r.code    = "PERM_DENIED";
        r.message = "无权限访问";
        co_return r;
    }
    r.ok = true;
    co_return r;
}

drogon::Task<ZmGateResult> ZmServerAudioStreamModule::Authorize(const drogon::HttpRequestPtr& req)
{
    const std::string cookie = req->getCookie(ZmSessionModule::CookieName());
    co_return co_await AuthorizeByCookie(cookie, ZmAuthGateModule::ClientIp(req));
}

// ============================================================================
// 状态与收听信息(不计入活跃度;明细按角色下发)
// ============================================================================
drogon::Task<drogon::HttpResponsePtr> ZmServerAudioStreamModule::HandleStatus(
    drogon::HttpRequestPtr req)
{
    const ZmGateResult gate = co_await Authorize(req);
    if (!gate.ok)
        co_return ZmAuthGateModule::ApiError(gate.status, gate.code, gate.message);

    ZMJSON out = ZMJSON::object();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        int                         listeners = 0;

        out["capturing"]         = m_running;
        out["streamId"]          = m_streamId;
        out["sampleRate"]        = kSampleRate;
        out["channels"]          = kChannels;
        out["bitrate"]           = kBitrate;
        out["segmentDurationMs"] = kSegmentMs;
        out["segmentWindowSec"]  = static_cast<int>(kRingSegments * kSegmentMs / 1000);
        out["bufferFillSec"]     = m_running && m_liveSeq >= m_startSeq && m_liveSeq > 0
                                       ? static_cast<double>((m_liveSeq - m_startSeq + 1) * kSegmentMs) / 1000.0
                                       : 0.0;

        // 明细只在管理员/开发者视角下发:避免普通用户看到他人的账号与内网地址
        ZMJSON arr = ZMJSON::array();
        for (const auto& [key, wc] : m_wsClients)
        {
            if (!wc.listening)
                continue;
            ++listeners;
            if (gate.ctx.level < kDetailLevel)
                continue;
            ZMJSON item = ZMJSON::object();
            item["uid"]        = wc.uid;
            item["account"]    = wc.account;
            item["nickname"]   = wc.nickname;
            item["ip"]         = wc.ip;
            item["client"]     = wc.client;
            item["level"]      = wc.level;
            item["sinceMs"]    = wc.sinceMs;
            item["lastSeenMs"] = wc.lastPongMs;
            item["paused"]     = wc.paused;
            // 落后量取"上报那一刻算好的值"(见 OnWsMessage 的 pong 分支);-1 = 尚未上报
            item["lagMs"]      = wc.lastLagMs;
            arr.push_back(item);
        }
        out["listenerCount"] = listeners;
        if (gate.ctx.level >= kDetailLevel)
            out["listeners"] = arr;
    }
    co_return ZmAuthGateModule::ApiOk(out);
}

// ============================================================================
// 帧组装
// ============================================================================
ZmServerAudioStreamModule::OutFrame ZmServerAudioStreamModule::TextFrame(const ZMJSON& json)
{
    OutFrame f;
    f.binary = false;
    const std::string s = zm_json_dump(json);
    f.body = std::make_shared<const std::vector<uint8_t>>(s.begin(), s.end());
    return f;
}

ZmServerAudioStreamModule::OutFrame ZmServerAudioStreamModule::BinaryFrame(
    uint8_t type, uint64_t startSeq, const std::vector<uint8_t>& payload)
{
    // [类型 u8][起始片号 u64 LE][载荷]:片号显式给出,客户端据此把片落到时间轴上
    OutFrame                       f;
    std::vector<uint8_t>           body;
    body.reserve(9 + payload.size());
    body.push_back(type);
    PutLE64(body, startSeq);
    body.insert(body.end(), payload.begin(), payload.end());
    f.binary = true;
    f.body   = std::make_shared<const std::vector<uint8_t>>(std::move(body));
    return f;
}

void ZmServerAudioStreamModule::SendFrame(const drogon::WebSocketConnectionPtr& conn,
                                          trantor::EventLoop*                   loop,
                                          OutFrame                              frame)
{
    if (!conn || !frame.body || frame.body->empty())
        return;
    auto body   = frame.body;
    auto binary = frame.binary;
    if (!loop)
    {
        // 正常路径不会走到:onOpen 必然在事件循环线程上调用
        conn->send(reinterpret_cast<const char*>(body->data()), body->size(),
                   binary ? drogon::WebSocketMessageType::Binary : drogon::WebSocketMessageType::Text);
        return;
    }
    loop->queueInLoop(
        [conn, body, binary]()
        {
            conn->send(reinterpret_cast<const char*>(body->data()), body->size(),
                       binary ? drogon::WebSocketMessageType::Binary
                              : drogon::WebSocketMessageType::Text);
        });
}

ZmServerAudioStreamModule::OutFrame ZmServerAudioStreamModule::BatchFrame(
    uint64_t startSeq, const std::vector<const std::vector<uint8_t>*>& segs)
{
    // 帧头与批载荷一次成型:[类型 1][起始片号 u64][片数 u32][每片:长度 u32 + 字节]
    OutFrame             f;
    std::vector<uint8_t> body;
    size_t               need = 9 + 4;
    for (const auto* s : segs)
        need += 4 + s->size();
    body.reserve(need);
    body.push_back(1);
    PutLE64(body, startSeq);
    PutLE32(body, static_cast<uint32_t>(segs.size()));
    for (const auto* s : segs)
    {
        PutLE32(body, static_cast<uint32_t>(s->size()));
        body.insert(body.end(), s->begin(), s->end());
    }
    f.binary = true;
    f.body   = std::make_shared<const std::vector<uint8_t>>(std::move(body));
    return f;
}

void ZmServerAudioStreamModule::SendFrameThenClose(const drogon::WebSocketConnectionPtr& conn,
                                                   trantor::EventLoop* loop, OutFrame frame,
                                                   drogon::CloseCode code, const char* reason)
{
    if (!conn)
        return;
    auto body   = frame.body;
    auto binary = frame.binary;
    auto why    = std::string(reason ? reason : "");
    const bool hasBody = body && !body->empty();

    if (!loop)
    {
        // 正常路径不会走到(连接建立时必然记录了所属事件循环):退回直接调用
        if (hasBody)
            conn->send(reinterpret_cast<const char*>(body->data()), body->size(),
                       binary ? drogon::WebSocketMessageType::Binary
                              : drogon::WebSocketMessageType::Text);
        conn->shutdown(code, why);
        return;
    }
    // 帧(如果有)与关闭都排进连接自己的事件循环:帧先于关闭帧发出,客户端才看得到原因;
    // 断开动作这一侧同样不能在采集线程/关停线程上直接碰连接对象
    if (!hasBody)
    {
        loop->queueInLoop([conn, code, why]() { conn->shutdown(code, why); });
        return;
    }
    loop->queueInLoop(
        [conn, body, binary, code, why]()
        {
            conn->send(reinterpret_cast<const char*>(body->data()), body->size(),
                       binary ? drogon::WebSocketMessageType::Binary
                              : drogon::WebSocketMessageType::Text);
            conn->shutdown(code, why);
        });
}

// ============================================================================
// WebSocket:建连引导
// ============================================================================
void ZmServerAudioStreamModule::BootstrapWs(const drogon::WebSocketConnectionPtr& conn,
                                            const drogon::HttpRequestPtr&         req)
{
    if (!conn)
        return;

    trantor::EventLoop* loop   = trantor::EventLoop::getEventLoopOfCurrentThread();
    const std::string   cookie = req ? req->getCookie(ZmSessionModule::CookieName()) : std::string();
    const std::string   ip     = req ? ZmAuthGateModule::ClientIp(req) : std::string();

    std::string key;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // 引导完成前用临时键登记:心跳与"全部离场才停采"的判定都覆盖到它
        key = std::string(kBootKeyPrefix) + std::to_string(++m_connSeq);
        WsClient wc;
        wc.conn      = conn;
        wc.loop      = loop;
        wc.cookie    = cookie;
        wc.ip        = ip;
        wc.sinceMs   = NowMs();
        wc.lastPongMs = wc.sinceMs;
        m_wsClients.emplace(key, std::move(wc));
    }
    conn->setContext(std::make_shared<std::string>(key));

    // 引导整体离核:鉴权查库 + EnsureReady 的设备枚举/启动都是阻塞动作
    drogon::async_run(
        [this, conn, key]() -> drogon::Task<void> { co_await BootstrapTask(conn, key); });
}

drogon::Task<void> ZmServerAudioStreamModule::BootstrapTask(drogon::WebSocketConnectionPtr conn,
                                                            std::string                   key)
{
    std::string cookie;
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end())
            co_return;   // 已被摘除(断开/超时)
        cookie = it->second.cookie;
        ip     = it->second.ip;
    }

    // ── ① 会话 + 账号状态 + 权限点 ──
    const ZmGateResult gate = co_await AuthorizeByCookie(cookie, ip);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end())
            co_return;
        if (!gate.ok)
        {
            ZMJSON j     = ZMJSON::object();
            j["type"]    = "auth_failed";
            j["code"]    = gate.code;
            j["message"] = gate.message;
            SendFrameThenClose(it->second.conn, it->second.loop, TextFrame(j),
                               drogon::CloseCode::kViolation, "auth failed");
            co_return;   // onClose 负责摘除
        }
        it->second.uid      = gate.ctx.uid;
        it->second.account  = gate.ctx.account;
        it->second.nickname = gate.ctx.nickname;
        it->second.ip       = gate.ctx.ip;
        it->second.level    = gate.ctx.level;
    }

    // ── ② 采集就绪(放工作池:设备枚举与引擎启动是数百毫秒的系统调用) ──
    std::string err;
    bool        transient = false;
    const bool  ok = co_await ZmHttpServer::RunOnPool<bool>(
        [this, &err, &transient]() -> bool { return EnsureReady(err, transient); });
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end())
            co_return;
        if (!ok)
        {
            ZMJSON j     = ZMJSON::object();
            j["type"]    = "error";
            // 暂时不可用(设备正在释放/启动超时)让客户端重试;确实没有设备才让它停
            j["code"]    = transient ? "AUDIO_BUSY" : "AUDIO_NO_DEVICE";
            j["message"] = err.empty() ? "服务器无可用音频设备" : err;
            SendFrameThenClose(it->second.conn, it->second.loop, TextFrame(j),
                               drogon::CloseCode::kEndpointGone, "capture unavailable");
            co_return;
        }
        it->second.bootstrapped = true;
        // 引导完成前到达的 sync:此刻统一补做"改键 + 重定基"(与置位同一临界区,无竞态)。
        // 这里必须走 RebindLocked:少了改键,条目会一直挂在临时键上,同键替换就失效了
        if (it->second.pendingSync)
        {
            it->second.pendingSync = false;
            if (!it->second.client.empty())
                RebindLocked(it, it->second.client);
        }
    }
    co_return;
}

// ============================================================================
// WebSocket:消息处理
// ============================================================================
void ZmServerAudioStreamModule::OnWsMessage(const drogon::WebSocketConnectionPtr& conn,
                                            std::string&&                         msg,
                                            drogon::WebSocketMessageType          type)
{
    if (type != drogon::WebSocketMessageType::Text)
        return;   // 客户端只发文本控制帧

    const std::string key = KeyOfConn(conn);
    if (key.empty())
        return;

    std::string errMsg;
    ZMJSON      j = zm_json_parse(msg, errMsg);
    if (!errMsg.empty() || !j.is_object())
        return;
    const std::string kind = zm_json_get_str(j, "type");

    // ── 心跳应答:登记游标与存活时刻(兼作 /status 的落后量来源) ──
    if (kind == "pong")
    {
        const int seq = zm_json_get_int(j, "seq", 0);
        const int pos = zm_json_get_int(j, "pos", 0);   // 客户端播放头对应的片号
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end() || it->second.conn != conn)
            return;
        // 落后量必须在上报的这一刻算好:直播边缘此后仍在前进,拿"此刻边缘"去减一个
        // 十几秒前的快照,量到的其实是上报间隔(界面显示 15 秒、耳朵只落后 0.3 秒)
        const uint64_t base = pos > 0 ? static_cast<uint64_t>(pos) : (seq > 0 ? static_cast<uint64_t>(seq) : 0);
        it->second.lastSeq    = seq > 0 ? static_cast<uint64_t>(seq) : 0;
        it->second.lastLagMs  = base > 0
                                    ? static_cast<int64_t>(m_liveSeq > base ? m_liveSeq - base : 0) * kSegmentMs
                                    : -1;
        it->second.lastPongMs = NowMs();
        return;
    }

    // ── 暂停:保留连接、停止推送(仍回 pong),超过 kPauseHoldMs 由心跳摘除 ──
    if (kind == "pause")
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end() || it->second.conn != conn)
            return;
        it->second.paused     = true;
        it->second.pausedAtMs = NowMs();
        m_lastActivityMs      = NowMs();
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 听众暂停 uid={} client={}", it->second.uid,
                         it->second.client);
        return;
    }

    // ── 同步/重定基:首次同步即登记为听众;此后任何一次同步都从直播边缘重新起播 ──
    if (kind == "sync")
    {
        const std::string client = zm_json_get_str(j, "client");
        if (!ValidClientId(client))
            return;

        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        it = m_wsClients.find(key);
        if (it == m_wsClients.end() || it->second.conn != conn)
            return;

        m_lastActivityMs = NowMs();
        if (!it->second.bootstrapped)
        {
            // 引导未完成(鉴权/设备还在准备):先记身份,由引导收尾统一做"改键 + 重定基"
            it->second.client      = client;
            it->second.pendingSync = true;
            return;
        }
        RebindLocked(it, client);   // 改键 + 重定基(失败时内部已下发错误帧并断开)
        return;
    }
}

void ZmServerAudioStreamModule::OnWsClose(const drogon::WebSocketConnectionPtr& conn)
{
    const std::string key = KeyOfConn(conn);
    if (key.empty())
        return;

    std::lock_guard<std::mutex> lk(m_mutex);
    auto                        it = m_wsClients.find(key);
    if (it == m_wsClients.end())
        return;
    // 归属校验:同一键可能已被重连的新连接占用(半开旧连接迟到),此时不能删别人的条目
    if (it->second.conn != conn)
        return;

    if (it->second.listening)
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 听众离场 uid={} client={}(连接关闭)",
                         it->second.uid, it->second.client);
    m_wsClients.erase(it);
}

// ============================================================================
// WebSocket:注册表操作与重定基
// ============================================================================
std::string ZmServerAudioStreamModule::KeyOf(int64_t uid, const std::string& client)
{
    return std::to_string(uid) + ":" + client;
}

std::string ZmServerAudioStreamModule::KeyOfConn(const drogon::WebSocketConnectionPtr& conn)
{
    if (!conn)
        return std::string();
    const auto p = conn->getContext<std::string>();
    return p ? *p : std::string();
}

std::unordered_map<std::string, ZmServerAudioStreamModule::WsClient>::iterator
ZmServerAudioStreamModule::MoveToKeyLocked(const std::string& oldKey, const std::string& newKey)
{
    auto it = m_wsClients.find(oldKey);
    if (it == m_wsClients.end())
        return m_wsClients.end();
    if (oldKey == newKey)
        return it;

    // 同一个 uid:client 的另一条连接可能还挂着(网络掉线但心跳未到期):先断开再顶掉,
    // 否则它的 onClose 迟到时会按同一个键把新条目删掉
    KickLocked(newKey, "replaced", drogon::CloseCode::kEndpointGone);

    it = m_wsClients.find(oldKey);   // Kick 会改动容器,迭代器作废,重新取
    if (it == m_wsClients.end())
        return m_wsClients.end();

    WsClient moved = std::move(it->second);
    m_wsClients.erase(it);
    auto res = m_wsClients.emplace(newKey, std::move(moved));
    if (res.second && res.first->second.conn)
        res.first->second.conn->setContext(std::make_shared<std::string>(newKey));
    return res.first;
}

bool ZmServerAudioStreamModule::RebindLocked(
    std::unordered_map<std::string, WsClient>::iterator it, const std::string& client)
{
    it->second.client        = client;
    const std::string oldKey = it->first;
    const std::string newKey = KeyOf(it->second.uid, client);
    if (newKey != oldKey)
    {
        it = MoveToKeyLocked(oldKey, newKey);   // 内部先断开占用同键的半开旧连接
        if (it == m_wsClients.end())
            return false;
    }
    if (!RebaseLocked(it->second))
    {
        ZMJSON f     = ZMJSON::object();
        f["type"]    = "error";
        f["code"]    = "AUDIO_BUSY";
        f["message"] = "音频采集未就绪,请稍后重试";
        SendFrameThenClose(it->second.conn, it->second.loop, TextFrame(f),
                           drogon::CloseCode::kEndpointGone, "capture unavailable");
    }
    return true;
}

bool ZmServerAudioStreamModule::RebaseLocked(WsClient& wc)
{
    if (!m_running || m_streamId.empty() || m_initSegment.empty())
        return false;

    const uint64_t live = m_liveSeq;
    uint64_t       from = (live > kBackfillSegs) ? (live - kBackfillSegs + 1) : 1;
    if (from < m_startSeq)
        from = m_startSeq;   // 缓冲窗口不足(采集刚起):按现有最旧的来

    // meta:客户端据此识别换流(streamId 变化即重建媒体源)
    ZMJSON meta       = ZMJSON::object();
    meta["type"]      = "meta";
    meta["streamId"]  = m_streamId;
    meta["segmentMs"] = kSegmentMs;
    meta["sampleRate"] = kSampleRate;
    meta["channels"]  = kChannels;
    meta["bitrate"]   = kBitrate;
    meta["baseSeq"]   = from;
    SendFrame(wc.conn, wc.loop, TextFrame(meta));

    // 初始化段:起始片号与 meta 的 baseSeq 一致
    SendFrame(wc.conn, wc.loop, BinaryFrame(0, from, m_initSegment));

    // 起播补发批:只给进入播放态所需的最小量(客户端不会回头要更多)
    std::vector<const std::vector<uint8_t>*> segs;
    for (uint64_t s = from; s <= live; ++s)
    {
        const size_t idx = static_cast<size_t>(s - m_startSeq);
        if (idx >= m_segments.size())
            break;
        segs.push_back(&m_segments[idx].second);
    }
    if (!segs.empty())
        SendFrame(wc.conn, wc.loop, BatchFrame(from, segs));

    // 与上面三个 SendFrame 同处一个临界区:实时推送只推 live 之后的新片,不重不漏、
    // 也不会插到补发批前面(入队与登记同序 —— 见头文件 SendFrame 的说明)
    wc.pushedUptoSeq = live;
    wc.paused        = false;
    wc.pausedAtMs    = 0;
    wc.listening     = true;
    return true;
}

// ============================================================================
// 心跳、存活与鉴权复检
// ============================================================================
void ZmServerAudioStreamModule::HeartbeatLocked(int64_t nowMs)
{
    const bool pingDue = (nowMs - m_lastPingMs >= kPingIntervalMs);
    if (pingDue)
        m_lastPingMs = nowMs;
    ZMJSON ping      = ZMJSON::object();
    ping["type"]     = "ping";
    const OutFrame pf = TextFrame(ping);

    std::vector<std::string> kick;
    for (auto& [key, wc] : m_wsClients)
    {
        if (pingDue)
            SendFrame(wc.conn, wc.loop, pf);
        // 无应答:NAT/代理静默断链,或页面被系统冻结后再没回来
        if (nowMs - wc.lastPongMs > kHeartbeatTimeoutMs)
            kick.push_back(key);
        // 建连后迟迟不发 sync:不放着不管
        else if (!wc.bootstrapped && nowMs - wc.sinceMs > kBootTimeoutMs)
            kick.push_back(key);
        // 暂停保持上限:超时即释放音频设备(界面上的"保留连接"只到这一步)
        else if (wc.paused && wc.pausedAtMs && nowMs - wc.pausedAtMs > kPauseHoldMs)
            kick.push_back(key);
    }
    for (const auto& k : kick)
        KickLocked(k, "超时", drogon::CloseCode::kEndpointGone);
}

void ZmServerAudioStreamModule::KickLocked(const std::string& key, const char* reason,
                                           drogon::CloseCode code, OutFrame frame)
{
    auto it = m_wsClients.find(key);
    if (it == m_wsClients.end())
        return;
    if (it->second.listening)
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 断开连接 uid={} client={}({})", it->second.uid,
                         it->second.client, reason);
    if (it->second.conn)
        SendFrameThenClose(it->second.conn, it->second.loop, frame, code, reason);
    // 先摘除再让 onClose 走一遍:onClose 找不到条目即无操作(幂等)
    m_wsClients.erase(it);
}

drogon::Task<void> ZmServerAudioStreamModule::RecheckAll()
{
    struct Item
    {
        std::string                    key;
        std::string                    cookie;
        std::string                    ip;
        int64_t                        uid = 0;
        drogon::WebSocketConnectionPtr conn;
    };
    std::vector<Item> items;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_shuttingDown)
            co_return;
        for (auto& [key, wc] : m_wsClients)
        {
            if (!wc.listening || wc.cookie.empty())
                continue;
            items.push_back(Item{key, wc.cookie, wc.ip, wc.uid, wc.conn});
        }
    }
    if (items.empty())
        co_return;

    for (auto& it : items)
    {
        // 锁外逐个复检:会话续期 + 账号状态 + 权限点(与引导同一份实现)
        const ZmGateResult gate = co_await AuthorizeByCookie(it.cookie, it.ip);
        std::lock_guard<std::mutex> lk(m_mutex);
        auto                        found = m_wsClients.find(it.key);
        if (found == m_wsClients.end() || found->second.conn != it.conn)
            continue;   // 期间已断开或被新连接顶掉
        if (gate.ok)
        {
            // 账号信息可能变了(昵称/等级):顺手刷新 /status 的展示口径
            found->second.account  = gate.ctx.account;
            found->second.nickname = gate.ctx.nickname;
            found->second.level    = gate.ctx.level;
            continue;
        }
        ZMJSON j     = ZMJSON::object();
        j["type"]    = "auth_failed";
        j["code"]    = gate.code;
        j["message"] = gate.message;
        KickLocked(it.key, "复检未通过", drogon::CloseCode::kViolation, TextFrame(j));
    }
    co_return;
}

// ============================================================================
// 采集生命周期(与传输无关:采集器/封装器沿用既有实现)
// ============================================================================
bool ZmServerAudioStreamModule::EnsureReady(std::string& errMsg, bool& transient)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    transient = true;   // 默认按"暂时不可用"处理:调用方据此让客户端重试

    if (m_shuttingDown)
    {
        errMsg    = "服务正在停止";
        transient = false;
        return false;
    }
    if (m_running && m_ready && !m_stopRequested)
        return true;   // 已在跑且有片可下发

    // 正在启动中(另一个连接已经把采集拉起来,首片还没到):等它就绪 ——
    // 不能按"设备正在释放"处理,否则多端同时首次收听时后来者会被直接踢掉
    if (m_running && !m_stopRequested)
    {
        m_readyCv.wait_for(lk, std::chrono::milliseconds(kReadyWaitMs),
                           [this] { return m_ready || !m_running || m_stopRequested; });
        if (m_ready && !m_stopRequested)
            return true;
        // 落到这里说明那一次启动失败或已在收尾:继续走下面的"等它退完再重启"
    }

    // 收尾中(停采请求已发出):等它退完再重启,避免与释放设备的过程交错。
    // 这里是"刚听完又点开始"的常见时序,等待给足(设备释放通常只有几十毫秒,
    // 等不到就是真出问题了);等不到时报错也不要报成"无可用设备"
    if (m_running)
    {
        m_readyCv.wait_for(lk, std::chrono::milliseconds(kReleaseWaitMs),
                           [this] { return !m_running; });
        if (m_running)
        {
            errMsg = "音频设备正在释放,请稍后重试";
            return false;
        }
    }

    // 回收上一次已退出的采集对象(线程已结束,join 立即返回;不持锁等待)
    if (m_capturer)
    {
        lk.unlock();
        m_capturer->StopAndJoin();
        lk.lock();
        m_capturer.reset();
    }

    // 新一次采集:新流标识、空缓冲、初始化段(封装器参数取自编码器)
    m_muxer       = std::make_unique<ZmWebmMuxer>(kSampleRate, kChannels, ZmAudioCapturer::GetPreSkip());
    m_initSegment = m_muxer->BuildInitSegment();
    m_streamId    = NewStreamId();
    m_segments.clear();
    m_startSeq      = 0;
    m_liveSeq       = 0;
    m_nextSeq       = 1;
    m_ready         = false;
    m_stopRequested = false;
    m_captureErr.clear();
    m_lastActivityMs = NowMs();
    m_lastSweepMs    = 0;
    m_lastPingMs     = NowMs();

    auto        cap = std::make_unique<ZmAudioCapturer>();
    std::string startErr;
    const bool  started = cap->Start(
        [this](const uint8_t* data, size_t len, uint64_t ptsMs) { OnFrame(data, len, ptsMs); },
        [this]() { OnTick(); }, [this]() { OnCaptureDone(); }, startErr);
    if (!started)
    {
        m_captureErr = startErr;
        m_muxer.reset();
        m_initSegment.clear();
        m_streamId.clear();
        errMsg    = startErr;
        transient = false;   // 设备枚举/格式协商失败:重试同样会失败
        DEFAULT_LOG_WARN("ZmServerAudioStreamModule: 采集启动失败:{}", startErr);
        return false;
    }
    m_capturer = std::move(cap);
    m_running  = true;
    m_lastActivityMs = NowMs();

    // 等首片:静音也出片,正常在 200ms 内到
    if (!m_readyCv.wait_for(lk, std::chrono::milliseconds(kReadyWaitMs),
                            [this] { return m_ready || !m_running; }))
    {
        m_stopRequested = true;
        if (m_capturer)
            m_capturer->RequestStop();
        errMsg = "音频采集启动超时";
        DEFAULT_LOG_WARN("ZmServerAudioStreamModule: 等待首片超时");
        return false;
    }
    if (!m_ready)
    {
        errMsg = m_captureErr.empty() ? "音频采集未能产出数据" : m_captureErr;
        return false;
    }
    return true;
}

void ZmServerAudioStreamModule::OnFrame(const uint8_t* data, size_t len, uint64_t ptsMs)
{
    std::vector<std::pair<drogon::WebSocketConnectionPtr, trantor::EventLoop*>> pushes;
    OutFrame                                                                    frame;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_muxer || !m_running)
            return;

        std::vector<uint8_t> seg = m_muxer->PushFrame(data, len, ptsMs);
        if (seg.empty())
            return;

        const uint64_t seq = m_nextSeq;
        m_segments.emplace_back(seq, std::move(seg));
        while (m_segments.size() > kRingSegments)
            m_segments.pop_front();
        m_startSeq = m_segments.front().first;
        m_liveSeq  = seq;
        ++m_nextSeq;

        if (!m_ready)
        {
            m_ready = true;
            m_readyCv.notify_all();
        }

        // 帧体一次组装、多连接共享(单片批 ≈0.9KB);本函数在采集线程上,不得做网络 I/O
        const std::vector<const std::vector<uint8_t>*> one{&m_segments.back().second};
        frame = BatchFrame(seq, one);

        for (auto& [key, wc] : m_wsClients)
        {
            // 未听完引导/暂停/已由补发覆盖的都不推(判据与登记同处锁内,顺序不会错)
            if (!wc.listening || wc.paused || seq <= wc.pushedUptoSeq)
                continue;
            wc.pushedUptoSeq = seq;
            pushes.emplace_back(wc.conn, wc.loop);
        }
    }
    // 锁外:只把闭包排进各连接自己的事件循环(非阻塞),真正的写 socket 由该循环串行完成
    for (auto& [conn, loop] : pushes)
        SendFrame(conn, loop, frame);
}

void ZmServerAudioStreamModule::OnTick()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_running)
        return;

    const int64_t now = NowMs();
    if (now - m_lastSweepMs < kSweepIntervalMs)
        return;
    m_lastSweepMs = now;

    HeartbeatLocked(now);

    // 按需采集:无连接且过了宽限期(覆盖"刚建连还没 sync"的窗口)→ 停采释放设备
    if (m_wsClients.empty() && !m_stopRequested && now - m_lastActivityMs >= kIdleGraceMs)
    {
        m_stopRequested = true;
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 已无听众,停止采集");
        if (m_capturer)
            m_capturer->RequestStop();   // 只请求不 join(本函数就在采集线程上)
    }
}

void ZmServerAudioStreamModule::OnCaptureDone()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const bool                  requested = m_stopRequested;
    const size_t                left      = m_wsClients.size();

    m_running       = false;
    m_ready         = false;
    m_stopRequested = false;
    m_segments.clear();
    m_startSeq = 0;
    m_liveSeq  = 0;
    m_nextSeq  = 1;
    m_streamId.clear();
    m_initSegment.clear();
    m_muxer.reset();
    m_captureErr = requested ? "" : "音频设备失效";

    // 设备失效:如实告知并断开(客户端重连时会重新走引导;若是停采则是自己人走了)
    if (!requested)
    {
        ZMJSON j     = ZMJSON::object();
        j["type"]    = "error";
        j["code"]    = "AUDIO_DEVICE_LOST";
        j["message"] = "音频设备已失效";
        const OutFrame f = TextFrame(j);
        for (auto& [key, wc] : m_wsClients)
            SendFrameThenClose(wc.conn, wc.loop, f, drogon::CloseCode::kEndpointGone, "capture stopped");
    }
    m_wsClients.clear();
    m_readyCv.notify_all();

    if (requested)
        DEFAULT_LOG_INFO("ZmServerAudioStreamModule: 采集已停止,释放音频设备(离场连接 {})", left);
    else
        DEFAULT_LOG_WARN("ZmServerAudioStreamModule: 音频设备失效,采集已停止(离场连接 {})", left);
}

void ZmServerAudioStreamModule::StopCapture()
{
    ZmAudioCapturer* cap = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stopRequested = true;
        cap             = m_capturer.get();
    }
    if (cap)
        cap->StopAndJoin();   // join 期间不持锁:采集线程的回调会取锁

    std::lock_guard<std::mutex> lk(m_mutex);
    m_running       = false;
    m_ready         = false;
    m_stopRequested = false;
    m_segments.clear();
    m_streamId.clear();
    m_initSegment.clear();
    m_muxer.reset();
    m_wsClients.clear();
}
