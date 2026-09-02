# 广播模块（Broadcast）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ZiMoPublic/net/ 中实现基于 libevent + ZmEvBaseRunLoop 的 TCP 一对多消息推送服务端。

**Architecture:** 4 文件结构 — base 层（状态枚举、帧协议、公共数据结构）+ server 层（ZmBroadcastServer 类）。服务端使用 evconnlistener 监听连接，bufferevent 管理客户端 I/O，事件驱动保证线程安全。所有 libevent 操作统一在 ZmEvBaseRunLoop 线程中执行。

**Tech Stack:** C++17, libevent (evconnlistener/bufferevent/event), nlohmann/json (via zm_json), ZmEvBaseRunLoop, ZmThread

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `ZiMoPublic/net/zm_net_broadcast_base.h` | 状态枚举、消息结构、客户端信息、帧协议函数声明 |
| `ZiMoPublic/net/zm_net_broadcast_base.cpp` | 帧协议编解码实现、UUID 生成、时间戳工具 |
| `ZiMoPublic/net/zm_net_broadcast_server.h` | ZmBroadcastServer 类声明、配置/回调结构体、内部 BcClient 结构 |
| `ZiMoPublic/net/zm_net_broadcast_server.cpp` | 服务端全部实现（监听、握手、心跳、tag、发送、线程安全） |

---

### Task 1: 创建 base 层头文件

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_base.h`

- [ ] **Step 1: 写入 zm_net_broadcast_base.h 完整内容**

```cpp
/**
 * @file zm_net_broadcast_base.h
 * @brief 广播模块公共定义
 *
 * 提供 TCP 广播服务端的公共类型定义：
 *   - 服务端状态枚举
 *   - 消息结构体
 *   - 客户端信息结构体
 *   - 帧协议编解码函数声明
 */

#ifndef ZM_NET_BROADCAST_BASE_H
#define ZM_NET_BROADCAST_BASE_H

#include <cstdint>
#include <string>
#include <vector>

struct bufferevent;
struct evbuffer;

// ============================================================================
// 状态枚举
// ============================================================================

/**
 * @brief 广播服务端状态枚举
 *
 * 状态流转: IDLE → STARTING → LISTENING → STOPPING → STOPPED
 * 任意阶段可进入 ERROR
 */
enum ZM_BROADCAST_STATE
{
    ZM_BC_STATE_IDLE      = 0,  ///< 未启动
    ZM_BC_STATE_STARTING  = 1,  ///< 启动中（正在绑定监听）
    ZM_BC_STATE_LISTENING = 2,  ///< 已监听，可接受连接
    ZM_BC_STATE_STOPPING  = 3,  ///< 停止中（正在关闭连接、清理 pending）
    ZM_BC_STATE_STOPPED   = 4,  ///< 已停止
    ZM_BC_STATE_ERROR     = 5,  ///< 错误
};

// ============================================================================
// 消息结构
// ============================================================================

/**
 * @brief 广播消息结构体
 *
 * tag 仅用于服务端过滤匹配，不进入线上 JSON。
 * 线上 JSON 格式: {"id":"...","timestamp":"...","topic":"...","content":...}
 */
struct BcMessage
{
    std::string id;         ///< 消息唯一ID（UUID v4）
    std::string topic;      ///< 主题
    std::string tag;        ///< 过滤标签（仅用于匹配，不序列化到线上）
    std::string content;    ///< 内容（JSON 任意类型字符串）
    std::string timestamp;  ///< 发送时 ISO-8601 时间戳
};

// ============================================================================
// 客户端信息
// ============================================================================

/**
 * @brief 客户端信息快照，用于查询和回调
 */
struct BcClientInfo
{
    std::string clientId;              ///< 服务端分配的唯一 ID
    std::string ip;                    ///< 对端 IP 地址
    uint16_t    port;                  ///< 对端端口号
    uint64_t    connectTime;           ///< 连接建立时间戳（毫秒）
    std::vector<std::string> tags;     ///< 当前订阅的 tag 列表
    size_t      queuePending;          ///< 队列中待发送消息数
    uint64_t    lastActiveTime;        ///< 最后活跃时间戳（毫秒）
    uint64_t    sentCount;             ///< 已发送消息数
};

// ============================================================================
// 帧协议
// ============================================================================

/**
 * @brief 从 evbuffer 中解码一帧数据
 *
 * 帧格式: 4 字节大端长度前缀 + JSON body。
 * 先 peek 4 字节解出长度，若 evbuffer 数据不足则返回空字符串等待更多数据。
 *
 * @param input  bufferevent 的输入 evbuffer
 * @return       解码后的 JSON 字符串，数据不足时返回空字符串
 */
std::string BcFrameDecode(struct evbuffer* input);

/**
 * @brief 将 JSON 字符串编码为帧并写入 bufferevent 输出缓冲区
 *
 * @param bev   目标 bufferevent
 * @param json  待发送的 JSON 字符串
 * @return      true 成功，false 失败
 */
bool BcFrameEncode(struct bufferevent* bev, const std::string& json);

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief 生成 UUID v4 格式的唯一 ID 字符串
 * @return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" 格式字符串
 */
std::string BcGenerateUUID();

/**
 * @brief 获取当前 UTC 时间的 ISO-8601 格式字符串
 * @return "2026-06-15T12:00:00Z" 格式字符串
 */
std::string BcNowTimestamp();

/**
 * @brief 获取当前毫秒级 Unix 时间戳
 * @return 毫秒时间戳
 */
uint64_t BcNowMillis();

#endif // ZM_NET_BROADCAST_BASE_H
```

- [ ] **Step 2: 验证文件无语法错误**

头文件无实现代码，手动检查 include 守卫、类型定义完整性。继续下一步。

---

### Task 2: 实现 base 层（帧协议 + 工具函数）

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_base.cpp`

- [ ] **Step 1: 写入 zm_net_broadcast_base.cpp 完整内容**

```cpp
/**
 * @file zm_net_broadcast_base.cpp
 * @brief 广播模块公共实现 — 帧协议编解码与工具函数
 */

#include "zm_net_broadcast_base.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include <random>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cstring>

// ============================================================================
// 帧协议实现
// ============================================================================

std::string BcFrameDecode(struct evbuffer* input)
{
    // 需要至少 4 字节长度头
    size_t avail = evbuffer_get_length(input);
    if (avail < 4)
        return std::string();

    // peek 4 字节大端长度，不消耗数据
    uint8_t lenBuf[4];
    evbuffer_copyout(input, lenBuf, 4);
    uint32_t bodyLen = ((uint32_t)lenBuf[0] << 24)
                     | ((uint32_t)lenBuf[1] << 16)
                     | ((uint32_t)lenBuf[2] << 8)
                     | ((uint32_t)lenBuf[3]);

    // body 数据不足则等待
    if (avail < 4 + (size_t)bodyLen)
        return std::string();

    // 消耗 4 字节长度头
    evbuffer_drain(input, 4);

    // 读取 body
    std::string body(bodyLen, '\0');
    evbuffer_remove(input, &body[0], bodyLen);

    return body;
}

bool BcFrameEncode(struct bufferevent* bev, const std::string& json)
{
    if (!bev || json.empty())
        return false;

    uint32_t bodyLen = (uint32_t)json.size();

    // 构造 4 字节大端长度前缀
    uint8_t lenBuf[4];
    lenBuf[0] = (bodyLen >> 24) & 0xFF;
    lenBuf[1] = (bodyLen >> 16) & 0xFF;
    lenBuf[2] = (bodyLen >> 8)  & 0xFF;
    lenBuf[3] =  bodyLen        & 0xFF;

    // 写入长度前缀
    if (bufferevent_write(bev, lenBuf, 4) != 0)
        return false;

    // 写入 body
    if (bufferevent_write(bev, json.data(), json.size()) != 0)
        return false;

    return true;
}

// ============================================================================
// 工具函数实现
// ============================================================================

std::string BcGenerateUUID()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    uint32_t a = dis(gen);
    uint32_t b = dis(gen);
    uint32_t c = dis(gen);
    uint32_t d = dis(gen);

    // UUID v4: 设置 variant 和 version 位
    // b 的第 19-16 位 = 0x4 (version 4)
    b = (b & 0xFFFF0FFF) | 0x00004000;
    // c 的第 31-30 位 = 0b10 (variant 1)
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a, b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d);
    return std::string(buf);
}

std::string BcNowTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_s(&tm, &tt);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

uint64_t BcNowMillis()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return (uint64_t)ms.count();
}
```

- [ ] **Step 2: 验证编译**

当前只有 .cpp 无调用方，暂不单独编译。在后续 Task 中随 server.cpp 一并编译验证。

---

### Task 3: 创建 server 层头文件

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.h`

- [ ] **Step 1: 写入 zm_net_broadcast_server.h 完整内容**

```cpp
/**
 * @file zm_net_broadcast_server.h
 * @brief TCP 广播服务端
 *
 * 基于 libevent + ZmEvBaseRunLoop 实现的一对多消息推送服务端。
 *
 * 核心特性:
 *   - TCP 监听（可配置地址/端口，端口绑定失败无限重试）
 *   - 客户端握手（settings → confirm_settings → 分配 client_id）
 *   - 双向活动检测心跳（服务端主导 ping/pong）
 *   - Tag 过滤订阅（subscribe / unsubscribe）
 *   - 立即/延时/定时消息发送（线程安全）
 *   - 每客户端独立消息队列（溢出丢弃最旧）
 *   - 连接数限制、按 client_id 踢出
 */

#ifndef ZM_NET_BROADCAST_SERVER_H
#define ZM_NET_BROADCAST_SERVER_H

#include "zm_net_broadcast_base.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct event_base;
struct evconnlistener;
struct bufferevent;
struct event;
class ZmEvBaseRunLoop;

// ============================================================================
// 服务端配置
// ============================================================================

/**
 * @brief 广播服务端配置
 */
struct BcServerConfig
{
    std::string listenIp;           ///< 监听地址，默认 "0.0.0.0"
    uint16_t    listenPort;         ///< 监听端口，0 = 随机分配
    int         maxConnections;     ///< 最大连接数，0 = 不限制
    int         heartbeatTime;      ///< 心跳超时秒数，默认 60
    int         handshakeTimeout;   ///< 握手超时秒数，默认 10
    size_t      clientQueueMaxSize; ///< 每客户端消息队列上限，默认 1024
    ZmEvBaseRunLoop* evLoop;        ///< 事件循环线程（必填）

    BcServerConfig()
        : listenIp("0.0.0.0")
        , listenPort(0)
        , maxConnections(0)
        , heartbeatTime(60)
        , handshakeTimeout(10)
        , clientQueueMaxSize(1024)
        , evLoop(nullptr)
    {}
};

// ============================================================================
// 服务端回调
// ============================================================================

/**
 * @brief 广播服务端事件回调集合
 */
struct BcServerCallbacks
{
    /// 监听成功回调，参数为实际监听端口
    std::function<void(uint16_t port)> onListenSuccess;

    /// 监听失败回调，参数为错误描述
    std::function<void(const std::string& error)> onListenFailed;

    /// 监听停止回调
    std::function<void()> onListenStopped;

    /// 错误回调
    std::function<void(const std::string& error)> onError;

    /// 客户端上线回调（握手完成 + 分配 client_id 后）
    std::function<void(const BcClientInfo&)> onClientOnline;

    /// 客户端离线回调（断开前最后一次快照）
    std::function<void(const BcClientInfo&)> onClientOffline;
};

// ============================================================================
// ZmBroadcastServer
// ============================================================================

/**
 * @brief TCP 广播服务端
 *
 * 使用方法:
 * @code
 *   BcServerConfig cfg;
 *   cfg.listenPort = 8080;
 *   cfg.evLoop = myRunLoop;
 *
 *   BcServerCallbacks cbs;
 *   cbs.onClientOnline = [](const BcClientInfo& info) { ... };
 *
 *   ZmBroadcastServer server(cfg, cbs);
 *   server.Start();
 *   // ...
 *   server.Broadcast("alert", "{\"msg\":\"hello\"}", "all");
 *   // ...
 *   server.Stop();
 * @endcode
 */
class ZmBroadcastServer
{
public:
    // --- 构造 / 析构 ---

    /**
     * @brief 构造广播服务端
     * @param config  服务端配置（evLoop 必填）
     * @param cbs     事件回调集合
     */
    ZmBroadcastServer(const BcServerConfig& config, const BcServerCallbacks& cbs);

    /** @brief 析构时自动调用 Stop() */
    ~ZmBroadcastServer();

    // 禁止拷贝 / 移动
    ZmBroadcastServer(const ZmBroadcastServer&) = delete;
    ZmBroadcastServer& operator=(const ZmBroadcastServer&) = delete;

    // --- 生命周期 ---

    /**
     * @brief 启动监听
     *
     * 异步操作: 内部向事件循环投递绑定任务，结果通过 onListenSuccess / onListenFailed 回调通知。
     *
     * @return true  任务已投递
     * @return false 状态不允许启动（非 IDLE/STOPPED）或 evLoop 未设置
     */
    bool Start();

    /** @brief 停止服务端，直接断开所有客户端连接并释放资源 */
    void Stop();

    // --- 状态查询（线程安全） ---

    /** @brief 获取当前服务端状态 */
    ZM_BROADCAST_STATE GetState() const;

    /** @brief 获取实际监听端口号（仅在 LISTENING 状态下有效） */
    uint16_t GetPort() const;

    /** @brief 获取当前已连接客户端数 */
    int GetConnectionCount() const;

    /** @brief 获取最大连接数限制 */
    int GetMaxConnections() const;

    /** @brief 获取当前全局发送队列中待处理的消息数 */
    size_t GetGlobalQueueSize() const;

    /** @brief 获取服务已运行秒数（从 LISTENING 开始计时） */
    uint64_t GetRunningTime() const;

    /** @brief 获取累计成功发送消息数 */
    uint64_t GetSentCount() const;

    /** @brief 获取累计丢弃消息数（队列满时丢弃） */
    uint64_t GetDiscardCount() const;

    // --- 客户端查询（线程安全） ---

    /**
     * @brief 获取指定客户端的信息快照
     * @param clientId  客户端 ID
     * @return 客户端信息，若不存在则 clientId 为空
     */
    BcClientInfo GetClientInfo(const std::string& clientId) const;

    /** @brief 获取所有已连接客户端的信息快照 */
    std::vector<BcClientInfo> GetAllClients() const;

    // --- 立即发送（线程安全） ---

    /**
     * @brief 向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     消息主题
     * @param content   消息内容（JSON 字符串）
     * @param tag       过滤标签（仅当客户端订阅了此 tag 时送达，空字符串表示无条件发送）
     * @return true 任务已投递，false 参数无效
     */
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);

    /**
     * @brief 向所有匹配 tag 的客户端广播消息
     * @param topic     消息主题
     * @param content   消息内容（JSON 字符串）
     * @param tag       过滤标签（仅推送给订阅此 tag 的客户端，空字符串表示全部推送）
     * @return true 任务已投递，false 参数无效
     */
    bool Broadcast(const std::string& topic, const std::string& content,
                   const std::string& tag);

    // --- 延时发送（线程安全） ---

    /**
     * @brief 延时向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     消息主题
     * @param content   消息内容
     * @param tag       过滤标签
     * @param delayMs   延迟毫秒数
     * @return true 任务已投递
     */
    bool SendDelayed(const std::string& clientId, const std::string& topic,
                     const std::string& content, const std::string& tag, uint32_t delayMs);

    /**
     * @brief 延时广播
     * @param delayMs  延迟毫秒数
     */
    bool BroadcastDelayed(const std::string& topic, const std::string& content,
                          const std::string& tag, uint32_t delayMs);

    // --- 定时发送（线程安全） ---

    /**
     * @brief 在指定时间点向指定客户端发送消息
     * @param timestampMs  Unix 毫秒时间戳，小于等于当前时间则立即发送
     */
    bool SendAt(const std::string& clientId, const std::string& topic,
                const std::string& content, const std::string& tag, uint64_t timestampMs);

    /**
     * @brief 在指定时间点广播消息
     * @param timestampMs  Unix 毫秒时间戳
     */
    bool BroadcastAt(const std::string& topic, const std::string& content,
                     const std::string& tag, uint64_t timestampMs);

    // --- 客户端管理（线程安全） ---

    /**
     * @brief 踢出指定客户端
     * @param clientId  客户端 ID
     * @return true 客户端存在且已发起断开，false 客户端不存在
     */
    bool KickClient(const std::string& clientId);

    // --- 运行时配置修改 ---

    /** @brief 修改最大连接数限制 */
    void SetMaxConnections(int max);

    /** @brief 修改心跳超时秒数（对已有客户端下次检查生效） */
    void SetHeartbeatTime(int seconds);

    /** @brief 修改每客户端队列上限 */
    void SetClientQueueMaxSize(size_t max);

private:
    // ============================================================
    // 内部客户端状态
    // ============================================================

    /**
     * @brief 单个客户端的完整运行时状态
     */
    struct BcClient
    {
        std::string clientId;               ///< 唯一 ID
        struct bufferevent* bev;            ///< libevent bufferevent
        BcClientInfo info;                  ///< 对外快照信息
        std::deque<std::string> msgQueue;   ///< 已帧编码的待发送消息队列
        struct event* heartbeatTimer;       ///< 心跳检测定时器
        struct event* handshakeTimer;       ///< 握手超时定时器
        bool handshakeDone;                 ///< 握手是否已完成
        uint64_t lastActiveTime;            ///< 最后活跃时间戳（毫秒）
        uint64_t lastDataSentTime;          ///< 最后发送数据时间戳（毫秒）

        BcClient()
            : bev(nullptr)
            , heartbeatTimer(nullptr)
            , handshakeTimer(nullptr)
            , handshakeDone(false)
            , lastActiveTime(0)
            , lastDataSentTime(0)
        {
            info.port = 0;
            info.connectTime = 0;
            info.queuePending = 0;
            info.lastActiveTime = 0;
            info.sentCount = 0;
        }
    };

    // ============================================================
    // 线程安全调度
    // ============================================================

    /** @brief 调度任务到事件循环线程执行的类型 */
    enum BcTaskType
    {
        BC_TASK_START       = 1,  ///< 执行 Start 绑定监听
        BC_TASK_STOP        = 2,  ///< 执行 Stop 清理
        BC_TASK_SEND        = 3,  ///< 执行消息发送
        BC_TASK_KICK        = 4,  ///< 执行踢出客户端
    };

    /** @brief 跨线程调度的任务数据 */
    struct BcScheduledTask
    {
        BcTaskType type;
        BcMessage  message;
        std::string clientId;       ///< Send 目标 / Kick 目标
        uint32_t   delayMs;         ///< 延迟毫秒（0 = 立即）
        uint64_t   timestampMs;     ///< 定时时间戳（0 = 不使用）
        bool       isBroadcast;     ///< true = Broadcast, false = Send
    };

    /**
     * @brief 将任务投递到事件循环线程执行
     * @param task  要执行的任务
     */
    void ScheduleTask(const BcScheduledTask& task);

    /** @brief 事件循环线程中实际执行调度的任务 */
    void ExecuteTask(const BcScheduledTask& task);

    // ============================================================
    // 内部方法（仅在事件循环线程中调用）
    // ============================================================

    /** @brief 在事件循环线程中执行绑定监听 */
    void DoStart();

    /** @brief 在事件循环线程中执行停止清理 */
    void DoStop();

    /** @brief 在事件循环线程中执行消息发送 */
    void DoSend(const BcMessage& msg, const std::string& clientId, bool isBroadcast,
                uint32_t delayMs, uint64_t timestampMs);

    /** @brief 向单个客户端投递消息帧 */
    void DeliverToClient(BcClient* client, const std::string& frameJson);

    /** @brief 清理客户端所有资源并通知离线 */
    void RemoveClient(const std::string& clientId);

    // ============================================================
    // libevent 静态回调
    // ============================================================

    /** @brief evconnlistener 新连接回调 */
    static void OnAcceptConnCB(struct evconnlistener* listener,
                               evutil_socket_t fd, struct sockaddr* addr,
                               int socklen, void* ctx);

    /** @brief 客户端 bufferevent 可读回调 */
    static void OnClientReadCB(struct bufferevent* bev, void* ctx);

    /** @brief 客户端 bufferevent 事件回调（EOF/ERROR） */
    static void OnClientEventCB(struct bufferevent* bev, short events, void* ctx);

    /** @brief 客户端写完成回调（从队列取下一帧发送） */
    static void OnClientWriteCB(struct bufferevent* bev, void* ctx);

    /** @brief 握手超时定时器回调 */
    static void OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 心跳检测定时器回调 */
    static void OnHeartbeatCheckCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 跨线程调度控制事件回调 */
    static void OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx);

    /** @brief 延时/定时发送到期回调 */
    static void OnDelayedSendCB(evutil_socket_t fd, short what, void* ctx);

    // ============================================================
    // 成员变量
    // ============================================================

    BcServerConfig m_config;                            ///< 配置副本
    BcServerCallbacks m_callbacks;                      ///< 回调副本
    std::atomic<ZM_BROADCAST_STATE> m_state;            ///< 当前状态
    std::atomic<uint16_t> m_listenPort;                 ///< 实际监听端口

    struct evconnlistener* m_listener;                  ///< libevent 监听器
    std::unordered_map<std::string, BcClient*> m_clients;  ///< clientId → 客户端状态

    mutable std::mutex m_clientsMutex;                  ///< 保护 m_clients 的互斥锁
    std::mutex m_taskMutex;                             ///< 保护 m_pendingTasks 的互斥锁
    std::vector<BcScheduledTask> m_pendingTasks;        ///< 待投递的任务队列

    struct event* m_dispatchEvent;                      ///< 跨线程调度事件
    struct event* m_retryTimer;                         ///< 绑定重试定时器

    std::atomic<uint64_t> m_sentCount;                  ///< 累计发送成功数
    std::atomic<uint64_t> m_discardCount;               ///< 累计丢弃数
    uint64_t m_startTime;                               ///< 启动成功时间戳（毫秒），非原子（仅事件循环线程读写）

    int m_retryCount;                                   ///< 绑定重试计数（仅事件循环线程）
};

#endif // ZM_NET_BROADCAST_SERVER_H
```

- [ ] **Step 2: 检查头文件完整性**

确认：
- include 守卫正确
- 所有类/结构体/函数声明完整
- 前向声明正确（ZmEvBaseRunLoop、event_base、evconnlistener、bufferevent、event）
- 与设计文档签名一致

---

### Task 4: 服务端实现 — 构造/析构 + 状态管理 + 基础框架

**Files:**
- Create: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`

- [ ] **Step 1: 写入框架代码（构造/析构、状态查询、运行时配置）**

```cpp
/**
 * @file zm_net_broadcast_server.cpp
 * @brief TCP 广播服务端实现
 */

#include "zm_net_broadcast_server.h"

#include "../util/zm_util_sys.h"
#include "../util/zm_util_str.h"
#include "../json/zm_json.h"
#include "../net/zm_net_runloop.h"
#include "zm_logger.h"

#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>

// ============================================================================
// 构造 / 析构
// ============================================================================

ZmBroadcastServer::ZmBroadcastServer(const BcServerConfig& config, const BcServerCallbacks& cbs)
    : m_config(config)
    , m_callbacks(cbs)
    , m_state(ZM_BC_STATE_IDLE)
    , m_listenPort(0)
    , m_listener(nullptr)
    , m_dispatchEvent(nullptr)
    , m_retryTimer(nullptr)
    , m_sentCount(0)
    , m_discardCount(0)
    , m_startTime(0)
    , m_retryCount(0)
{
}

ZmBroadcastServer::~ZmBroadcastServer()
{
    Stop();
}

// ============================================================================
// 状态查询（线程安全 — 使用 atomic 或加锁读副本）
// ============================================================================

ZM_BROADCAST_STATE ZmBroadcastServer::GetState() const
{
    return m_state.load(std::memory_order_acquire);
}

uint16_t ZmBroadcastServer::GetPort() const
{
    return m_listenPort.load(std::memory_order_acquire);
}

int ZmBroadcastServer::GetConnectionCount() const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return (int)m_clients.size();
}

int ZmBroadcastServer::GetMaxConnections() const
{
    return m_config.maxConnections;
}

size_t ZmBroadcastServer::GetGlobalQueueSize() const
{
    std::lock_guard<std::mutex> lock(m_taskMutex);
    return m_pendingTasks.size();
}

uint64_t ZmBroadcastServer::GetRunningTime() const
{
    if (m_startTime == 0)
        return 0;
    return (BcNowMillis() - m_startTime) / 1000;
}

uint64_t ZmBroadcastServer::GetSentCount() const
{
    return m_sentCount.load(std::memory_order_acquire);
}

uint64_t ZmBroadcastServer::GetDiscardCount() const
{
    return m_discardCount.load(std::memory_order_acquire);
}

BcClientInfo ZmBroadcastServer::GetClientInfo(const std::string& clientId) const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(clientId);
    if (it != m_clients.end())
    {
        BcClientInfo info = it->second->info;
        info.queuePending = it->second->msgQueue.size();
        return info;
    }
    return BcClientInfo();
}

std::vector<BcClientInfo> ZmBroadcastServer::GetAllClients() const
{
    std::vector<BcClientInfo> result;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    result.reserve(m_clients.size());
    for (const auto& pair : m_clients)
    {
        BcClientInfo info = pair.second->info;
        info.queuePending = pair.second->msgQueue.size();
        result.push_back(std::move(info));
    }
    return result;
}

// ============================================================================
// 运行时配置修改
// ============================================================================

void ZmBroadcastServer::SetMaxConnections(int max)
{
    m_config.maxConnections = max;
}

void ZmBroadcastServer::SetHeartbeatTime(int seconds)
{
    if (seconds > 0)
        m_config.heartbeatTime = seconds;
}

void ZmBroadcastServer::SetClientQueueMaxSize(size_t max)
{
    m_config.clientQueueMaxSize = max;
}
```

- [ ] **Step 2: 验证编译**

此时缺少后续方法实现（Start/Stop/Send 等），暂不单独编译。在后续任务中逐步补全后统一编译。

---

### Task 5: 服务端实现 — 监听绑定（Start + DoStart + 重试）

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（追加代码）

- [ ] **Step 1: 在文件末尾追加 Start / DoStart / 重试逻辑**

```cpp
// ============================================================================
// 生命周期 — Start
// ============================================================================

bool ZmBroadcastServer::Start()
{
    ZM_BROADCAST_STATE expected = ZM_BC_STATE_IDLE;
    ZM_BROADCAST_STATE stopped = ZM_BC_STATE_STOPPED;
    if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STARTING) &&
        !m_state.compare_exchange_strong(stopped, ZM_BC_STATE_STARTING))
    {
        // 状态不允许启动
        return false;
    }

    if (!m_config.evLoop)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("evLoop is null");
        return false;
    }

    if (!m_config.evLoop->IsLooped())
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("evLoop is not running");
        return false;
    }

    // 创建跨线程调度事件（在事件循环线程中创建）
    BcScheduledTask task;
    task.type = BC_TASK_START;
    ScheduleTask(task);

    return true;
}

void ZmBroadcastServer::DoStart()
{
    struct event_base* evbase = m_config.evLoop->GetEventBase();
    if (!evbase)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("event_base is null");
        return;
    }

    // 创建跨线程调度事件
    m_dispatchEvent = event_new(evbase, -1, EV_READ | EV_PERSIST,
                                ZmBroadcastServer::OnDispatchEventCB, this);
    if (!m_dispatchEvent)
    {
        m_state.store(ZM_BC_STATE_ERROR);
        if (m_callbacks.onListenFailed)
            m_callbacks.onListenFailed("Failed to create dispatch event");
        return;
    }
    event_add(m_dispatchEvent, nullptr);

    // 尝试绑定监听
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(m_config.listenPort);

    if (m_config.listenIp == "0.0.0.0")
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        evutil_inet_pton(AF_INET, m_config.listenIp.c_str(), &sin.sin_addr);

    m_listener = evconnlistener_new_bind(evbase,
        ZmBroadcastServer::OnAcceptConnCB, this,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        (struct sockaddr*)&sin, sizeof(sin));

    if (m_listener)
    {
        // 获取实际端口
        struct sockaddr_storage ss;
        ev_socklen_t slen = sizeof(ss);
        evutil_socket_t fd = evconnlistener_get_fd(m_listener);
        if (getsockname(fd, (struct sockaddr*)&ss, &slen) == 0)
        {
            if (ss.ss_family == AF_INET)
            {
                uint16_t port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
                m_listenPort.store(port);
                if (m_config.listenPort == 0)
                    m_config.listenPort = port;
            }
        }

        m_startTime = BcNowMillis();
        m_retryCount = 0;
        m_state.store(ZM_BC_STATE_LISTENING);

        DEFAULT_LOG_INFO("[BcServer] Listening on {}:{}", m_config.listenIp, m_listenPort.load());

        if (m_callbacks.onListenSuccess)
            m_callbacks.onListenSuccess(m_listenPort.load());
    }
    else
    {
        // 绑定失败，启动重试
        m_retryCount++;
        DEFAULT_LOG_ERROR("[BcServer] Bind failed on {}:{}, retry #{}",
                          m_config.listenIp, m_config.listenPort, m_retryCount);

        // 创建重试定时器（1 秒后重试）
        timeval tv = {1, 0};
        m_retryTimer = evtimer_new(evbase,
            [](evutil_socket_t fd, short what, void* ctx) {
                ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;
                server->DoStart();
            }, this);
        evtimer_add(m_retryTimer, &tv);
    }
}
```

- [ ] **Step 2: 确认依赖**

确保 `getsockname`、`evconnlistener_new_bind` 等 API 的头文件已在顶部 include。
需要补充 include：`<event2/listener.h>`、`<winsock2.h>`（通过 libevent 间接引入）。

---

### Task 6: 服务端实现 — 客户端连接处理（Accept + 握手 + 读回调）

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（追加代码）

- [ ] **Step 1: 追加 OnAcceptConnCB 静态回调**

```cpp
// ============================================================================
// 客户端连接 — Accept 回调
// ============================================================================

void ZmBroadcastServer::OnAcceptConnCB(struct evconnlistener* listener,
                                        evutil_socket_t fd, struct sockaddr* addr,
                                        int socklen, void* ctx)
{
    ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;

    // 检查连接数限制
    if (server->m_config.maxConnections > 0)
    {
        std::lock_guard<std::mutex> lock(server->m_clientsMutex);
        if ((int)server->m_clients.size() >= server->m_config.maxConnections)
        {
            DEFAULT_LOG_WARN("[BcServer] Connection limit reached ({})", server->m_config.maxConnections);
            evutil_closesocket(fd);
            return;
        }
    }

    struct event_base* evbase = server->m_config.evLoop->GetEventBase();

    // 创建 bufferevent
    struct bufferevent* bev = bufferevent_socket_new(evbase, fd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (!bev)
    {
        DEFAULT_LOG_ERROR("[BcServer] Failed to create bufferevent for new connection");
        evutil_closesocket(fd);
        return;
    }

    // 分配客户端状态
    BcClient* client = new BcClient();
    client->bev = bev;
    client->clientId = BcGenerateUUID();
    client->info.clientId = client->clientId;
    client->info.connectTime = BcNowMillis();
    client->lastActiveTime = BcNowMillis();
    client->info.lastActiveTime = client->lastActiveTime;
    client->handshakeDone = false;

    // 解析对端地址信息
    if (addr->sa_family == AF_INET)
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        char ipBuf[64];
        evutil_inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
        client->info.ip = ipBuf;
        client->info.port = ntohs(sin->sin_port);
    }

    // 设置 bufferevent 回调
    bufferevent_setcb(bev,
        ZmBroadcastServer::OnClientReadCB,
        ZmBroadcastServer::OnClientWriteCB,
        ZmBroadcastServer::OnClientEventCB,
        client);

    // 设置读水位线（4 字节 + 至少 2 字节 JSON body 确保帧可解析）
    bufferevent_setwatermark(bev, EV_READ, 6, 0);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    // 发送 settings 帧
    ZMJSON settings;
    settings["settings"]["heartbeat_time"] = server->m_config.heartbeatTime;
    std::string settingsJson = zm_json_dump(settings);
    BcFrameEncode(bev, settingsJson);
    client->lastDataSentTime = BcNowMillis();

    // 启动握手超时定时器
    timeval handshakeTv = {server->m_config.handshakeTimeout, 0};
    client->handshakeTimer = evtimer_new(evbase,
        ZmBroadcastServer::OnHandshakeTimeoutCB, client);
    evtimer_add(client->handshakeTimer, &handshakeTv);

    // 暂存客户端（握手尚未完成，但允许接收 confirm_settings）
    {
        std::lock_guard<std::mutex> lock(server->m_clientsMutex);
        server->m_clients[client->clientId] = client;
    }

    DEFAULT_LOG_INFO("[BcServer] New connection: fd={}, client_id={}, ip={}:{}",
                     (int)fd, client->clientId, client->info.ip, client->info.port);
}
```

- [ ] **Step 2: 追加 OnClientReadCB（处理帧解码 + 协议消息分发）**

```cpp
// ============================================================================
// 客户端数据读取
// ============================================================================

void ZmBroadcastServer::OnClientReadCB(struct bufferevent* bev, void* ctx)
{
    BcClient* client = (BcClient*)ctx;
    ZmBroadcastServer* server = nullptr;

    // 查找 owning server（通过 m_clients 反向查找或存储回指针）
    // 简便做法：通过 bufferevent 的 cbarg 或额外存储 server 指针
    // 此处需要在客户端结构体中存储 server 回指针
    // 暂用静态方法处理

    struct evbuffer* input = bufferevent_get_input(bev);

    // 循环解码所有完整帧
    while (true)
    {
        std::string json = BcFrameDecode(input);
        if (json.empty())
            break; // 数据不足，等待更多数据

        // 刷新活跃时间
        client->lastActiveTime = BcNowMillis();
        client->info.lastActiveTime = client->lastActiveTime;

        // 解析 JSON
        std::string error;
        ZMJSON msg = zm_json_parse(json, error);
        if (!error.empty() || !msg.is_object())
        {
            DEFAULT_LOG_WARN("[BcServer] Invalid JSON from client {}: {}", client->clientId, error);
            continue;
        }

        std::string action = zm_json_get_str(msg, "action", "");

        if (action == "confirm_settings")
        {
            // 握手完成
            if (client->handshakeDone)
                continue;

            client->handshakeDone = true;

            // 取消握手超时定时器
            if (client->handshakeTimer)
            {
                event_free(client->handshakeTimer);
                client->handshakeTimer = nullptr;
            }

            // 从 server 的 clients map 中找到 server 指针来回调
            // 由于 BcClient 需要引用 server，这里需要存储 server 指针
            // 此处在首次实现时先记录日志，具体回调逻辑见 Task 7
            DEFAULT_LOG_INFO("[BcServer] Client {} handshake completed", client->clientId);

            // 回调 onClientOnline（需要 server 指针）
            // 在 Task 7 中完善（给 BcClient 添加 m_server 回指针）
        }
        else if (action == "pong")
        {
            // pong 响应已通过 lastActiveTime 刷新体现
            DEFAULT_LOG_DEBUG("[BcServer] Client {} pong", client->clientId);
        }
        else if (action == "subscribe")
        {
            // 更新订阅 tag 列表（将在 Task 8 完善）
            if (msg.contains("tags") && msg["tags"].is_array())
            {
                client->info.tags.clear();
                for (const auto& t : msg["tags"])
                {
                    if (t.is_string())
                    {
                        std::string tag = t.get<std::string>();
                        // 去重
                        bool found = false;
                        for (const auto& existing : client->info.tags)
                        {
                            if (existing == tag) { found = true; break; }
                        }
                        if (!found)
                            client->info.tags.push_back(tag);
                    }
                }
                DEFAULT_LOG_INFO("[BcServer] Client {} subscribed tags: {}",
                                 client->clientId, client->info.tags.size());
            }
        }
        else if (action == "unsubscribe")
        {
            if (msg.contains("tags") && msg["tags"].is_array())
            {
                for (const auto& t : msg["tags"])
                {
                    if (t.is_string())
                    {
                        std::string tag = t.get<std::string>();
                        auto& tags = client->info.tags;
                        auto it = std::find(tags.begin(), tags.end(), tag);
                        if (it != tags.end())
                            tags.erase(it);
                    }
                }
                DEFAULT_LOG_INFO("[BcServer] Client {} unsubscribed tags", client->clientId);
            }
        }
        else
        {
            DEFAULT_LOG_DEBUG("[BcServer] Unknown action from client {}: {}", client->clientId, action);
        }
    }
}
```

- [ ] **Step 3: 追加 OnClientEventCB（处理断开）**

```cpp
// ============================================================================
// 客户端事件（断开 / 错误）
// ============================================================================

void ZmBroadcastServer::OnClientEventCB(struct bufferevent* bev, short events, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    if (events & BEV_EVENT_EOF)
    {
        DEFAULT_LOG_INFO("[BcServer] Client {} disconnected (EOF)", client->clientId);
    }
    else if (events & BEV_EVENT_ERROR)
    {
        DEFAULT_LOG_ERROR("[BcServer] Client {} error: {}",
                          client->clientId, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    }
    else if (events & BEV_EVENT_TIMEOUT)
    {
        DEFAULT_LOG_WARN("[BcServer] Client {} timeout", client->clientId);
    }

    // RemoveClient 需要 server 指针 — 将在 Task 7 完善
    // server->RemoveClient(client->clientId);
}
```

- [ ] **Step 4: 追加 OnClientWriteCB（队列消费）**

```cpp
// ============================================================================
// 客户端写完成（从消息队列取下一帧）
// ============================================================================

void ZmBroadcastServer::OnClientWriteCB(struct bufferevent* bev, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    if (client->msgQueue.empty())
        return;

    // 取出队首帧（不立即发送，留给队列管理逻辑）
    // 写完成回调仅用于触发队列消费 — 实际消费由 DeliverToClient 调用时驱动
}
```

---

### Task 7: 服务端实现 — 完善 BcClient 回指针 + RemoveClient + 握手回调

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.h`（给 BcClient 添加 m_owner 指针）
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（修改 Accept/Read/Event 回调，添加 RemoveClient）

- [ ] **Step 1: 在头文件的 BcClient 中添加 m_owner 回指针**

在 `zm_net_broadcast_server.h` 的 `BcClient` 结构体中添加成员：

```cpp
        ZmBroadcastServer* m_owner;           ///< 所属服务端指针
```

完整地更新 BcClient 定义为：

```cpp
    struct BcClient
    {
        std::string clientId;
        struct bufferevent* bev;
        BcClientInfo info;
        std::deque<std::string> msgQueue;
        struct event* heartbeatTimer;
        struct event* handshakeTimer;
        bool handshakeDone;
        uint64_t lastActiveTime;
        uint64_t lastDataSentTime;
        ZmBroadcastServer* m_owner;           ///< 所属服务端回指针

        BcClient()
            : bev(nullptr)
            , heartbeatTimer(nullptr)
            , handshakeTimer(nullptr)
            , handshakeDone(false)
            , lastActiveTime(0)
            , lastDataSentTime(0)
            , m_owner(nullptr)
        {
            info.port = 0;
            info.connectTime = 0;
            info.queuePending = 0;
            info.lastActiveTime = 0;
            info.sentCount = 0;
        }
    };
```

- [ ] **Step 2: 更新 OnAcceptConnCB 中设置 m_owner**

在 OnAcceptConnCB 中 `BcClient* client = new BcClient();` 之后添加：

```cpp
    client->m_owner = server;
```

- [ ] **Step 3: 修改 OnClientReadCB 中握手完成的回调逻辑**

将 confirm_settings 处理中的注释替换为实际回调：

```cpp
        if (action == "confirm_settings")
        {
            // 握手完成
            if (client->handshakeDone)
                continue;

            client->handshakeDone = true;

            // 取消握手超时定时器
            if (client->handshakeTimer)
            {
                event_free(client->handshakeTimer);
                client->handshakeTimer = nullptr;
            }

            ZmBroadcastServer* server = client->m_owner;

            // 启动心跳检测定时器
            struct event_base* evbase = server->m_config.evLoop->GetEventBase();
            int pingInterval = server->m_config.heartbeatTime / 2;
            if (pingInterval < 1) pingInterval = 1;

            timeval heartbeatTv = {pingInterval, 0};
            client->heartbeatTimer = event_new(evbase, -1, EV_TIMEOUT | EV_PERSIST,
                                                ZmBroadcastServer::OnHeartbeatCheckCB, client);
            event_add(client->heartbeatTimer, &heartbeatTv);

            DEFAULT_LOG_INFO("[BcServer] Client {} handshake completed", client->clientId);

            if (server->m_callbacks.onClientOnline)
                server->m_callbacks.onClientOnline(client->info);
        }
```

- [ ] **Step 4: 修改 OnClientEventCB 中调用 RemoveClient**

```cpp
void ZmBroadcastServer::OnClientEventCB(struct bufferevent* bev, short events, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    if (events & BEV_EVENT_EOF)
        DEFAULT_LOG_INFO("[BcServer] Client {} disconnected (EOF)", client->clientId);
    else if (events & BEV_EVENT_ERROR)
        DEFAULT_LOG_ERROR("[BcServer] Client {} error: {}",
                          client->clientId, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
    else if (events & BEV_EVENT_TIMEOUT)
        DEFAULT_LOG_WARN("[BcServer] Client {} timeout", client->clientId);

    if (client->m_owner)
        client->m_owner->RemoveClient(client->clientId);
}
```

- [ ] **Step 5: 添加 RemoveClient 实现**

```cpp
// ============================================================================
// 客户端清理
// ============================================================================

void ZmBroadcastServer::RemoveClient(const std::string& clientId)
{
    BcClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(clientId);
        if (it == m_clients.end())
            return;
        client = it->second;
        m_clients.erase(it);
    }

    // 回调离线（保存快照）
    if (m_callbacks.onClientOffline)
    {
        BcClientInfo info = client->info;
        info.queuePending = client->msgQueue.size();
        m_callbacks.onClientOffline(info);
    }

    // 释放资源
    if (client->handshakeTimer)
    {
        event_free(client->handshakeTimer);
        client->handshakeTimer = nullptr;
    }
    if (client->heartbeatTimer)
    {
        event_free(client->heartbeatTimer);
        client->heartbeatTimer = nullptr;
    }
    if (client->bev)
    {
        bufferevent_free(client->bev);
        client->bev = nullptr;
    }

    delete client;
    DEFAULT_LOG_INFO("[BcServer] Client {} removed", clientId);
}
```

- [ ] **Step 6: 添加握手超时回调**

```cpp
// ============================================================================
// 握手超时
// ============================================================================

void ZmBroadcastServer::OnHandshakeTimeoutCB(evutil_socket_t fd, short what, void* ctx)
{
    BcClient* client = (BcClient*)ctx;
    DEFAULT_LOG_WARN("[BcServer] Client {} handshake timeout", client->clientId);

    if (client->m_owner)
        client->m_owner->RemoveClient(client->clientId);
}
```

---

### Task 8: 服务端实现 — 心跳检测

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（追加心跳回调）

- [ ] **Step 1: 追加 OnHeartbeatCheckCB 实现**

```cpp
// ============================================================================
// 心跳检测
// ============================================================================

void ZmBroadcastServer::OnHeartbeatCheckCB(evutil_socket_t fd, short what, void* ctx)
{
    BcClient* client = (BcClient*)ctx;
    if (!client || !client->m_owner)
        return;

    ZmBroadcastServer* server = client->m_owner;
    uint64_t now = BcNowMillis();
    int heartbeatTimeMs = server->m_config.heartbeatTime * 1000;
    int pingIntervalMs = (server->m_config.heartbeatTime / 2) * 1000;
    if (pingIntervalMs < 1000) pingIntervalMs = 1000;

    // 1. 检查超时
    uint64_t elapsed = now - client->lastActiveTime;
    if (elapsed >= (uint64_t)heartbeatTimeMs)
    {
        DEFAULT_LOG_WARN("[BcServer] Client {} heartbeat timeout (elapsed={}ms, threshold={}ms)",
                         client->clientId, elapsed, heartbeatTimeMs);
        server->RemoveClient(client->clientId);
        return;
    }

    // 2. 若空闲超过 ping 间隔则发送 ping
    uint64_t sendElapsed = now - client->lastDataSentTime;
    if (sendElapsed >= (uint64_t)pingIntervalMs)
    {
        ZMJSON ping;
        ping["action"] = "ping";
        std::string pingJson = zm_json_dump(ping);
        if (BcFrameEncode(client->bev, pingJson))
        {
            client->lastDataSentTime = now;
            DEFAULT_LOG_DEBUG("[BcServer] Ping sent to client {}", client->clientId);
        }
    }
}
```

---

### Task 9: 服务端实现 — 消息发送（Send / Broadcast + 队列管理）

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（追加发送相关实现）

- [ ] **Step 1: 添加 Send / Broadcast 公共接口**

```cpp
// ============================================================================
// 立即发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::Send(const std::string& clientId, const std::string& topic,
                              const std::string& content, const std::string& tag)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = 0;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::Broadcast(const std::string& topic, const std::string& content,
                                   const std::string& tag)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = 0;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// 延时发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::SendDelayed(const std::string& clientId, const std::string& topic,
                                     const std::string& content, const std::string& tag,
                                     uint32_t delayMs)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = delayMs;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::BroadcastDelayed(const std::string& topic, const std::string& content,
                                          const std::string& tag, uint32_t delayMs)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = delayMs;
    task.timestampMs = 0;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// 定时发送（线程安全入口）
// ============================================================================

bool ZmBroadcastServer::SendAt(const std::string& clientId, const std::string& topic,
                                const std::string& content, const std::string& tag,
                                uint64_t timestampMs)
{
    if (clientId.empty() || topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.clientId = clientId;
    task.isBroadcast = false;
    task.delayMs = 0;
    task.timestampMs = timestampMs;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

bool ZmBroadcastServer::BroadcastAt(const std::string& topic, const std::string& content,
                                     const std::string& tag, uint64_t timestampMs)
{
    if (topic.empty())
        return false;

    BcScheduledTask task;
    task.type = BC_TASK_SEND;
    task.isBroadcast = true;
    task.delayMs = 0;
    task.timestampMs = timestampMs;
    task.message.id = BcGenerateUUID();
    task.message.timestamp = BcNowTimestamp();
    task.message.topic = topic;
    task.message.content = content;
    task.message.tag = tag;

    ScheduleTask(task);
    return true;
}

// ============================================================================
// DoSend（事件循环线程中执行真正的发送）
// ============================================================================

void ZmBroadcastServer::DoSend(const BcMessage& msg, const std::string& clientId,
                                bool isBroadcast, uint32_t delayMs, uint64_t timestampMs)
{
    // 计算实际延迟
    uint32_t actualDelay = delayMs;
    if (timestampMs > 0)
    {
        uint64_t now = BcNowMillis();
        if (timestampMs > now)
            actualDelay = (uint32_t)(timestampMs - now);
        else
            actualDelay = 0;
    }

    // 如果需延迟，创建一次性定时器
    if (actualDelay > 0)
    {
        struct event_base* evbase = m_config.evLoop->GetEventBase();

        // 将任务数据拷贝到堆上
        BcScheduledTask* heapTask = new BcScheduledTask();
        heapTask->type = BC_TASK_SEND;
        heapTask->clientId = clientId;
        heapTask->isBroadcast = isBroadcast;
        heapTask->message = msg;
        heapTask->m_server = this;

        timeval tv;
        tv.tv_sec = actualDelay / 1000;
        tv.tv_usec = (actualDelay % 1000) * 1000;

        struct event* timer = evtimer_new(evbase, OnDelayedSendCB, heapTask);
        evtimer_add(timer, &tv);
        return;
    }

    // 构造线上 JSON
    ZMJSON jsonMsg;
    jsonMsg["id"] = msg.id;
    jsonMsg["timestamp"] = msg.timestamp;
    jsonMsg["topic"] = msg.topic;
    jsonMsg["content"] = ZMJSON::parse(msg.content, nullptr, false);
    if (jsonMsg["content"].is_discarded())
        jsonMsg["content"] = msg.content; // 非 JSON 内容作为字符串

    std::string jsonStr = zm_json_dump(jsonMsg);

    if (isBroadcast)
    {
        // 遍历所有已握手客户端，匹配 tag
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients)
        {
            BcClient* client = pair.second;
            if (!client->handshakeDone)
                continue;

            // tag 过滤：空 tag 表示全部推送，否则仅推送给订阅了此 tag 的客户端
            if (!msg.tag.empty())
            {
                bool subscribed = false;
                for (const auto& t : client->info.tags)
                {
                    if (t == msg.tag) { subscribed = true; break; }
                }
                if (!subscribed)
                    continue;
            }

            DeliverToClient(client, jsonStr);
        }
    }
    else
    {
        // 单播
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(clientId);
        if (it != m_clients.end() && it->second->handshakeDone)
        {
            // tag 过滤
            BcClient* client = it->second;
            if (!msg.tag.empty())
            {
                bool subscribed = false;
                for (const auto& t : client->info.tags)
                {
                    if (t == msg.tag) { subscribed = true; break; }
                }
                if (!subscribed)
                    return;
            }
            DeliverToClient(client, jsonStr);
        }
    }
}

// ============================================================================
// DeliverToClient（帧编码 + 入队列 + bufferevent_write）
// ============================================================================

void ZmBroadcastServer::DeliverToClient(BcClient* client, const std::string& jsonStr)
{
    // 帧编码
    // 构造帧 buffer
    uint32_t bodyLen = (uint32_t)jsonStr.size();
    uint8_t lenBuf[4];
    lenBuf[0] = (bodyLen >> 24) & 0xFF;
    lenBuf[1] = (bodyLen >> 16) & 0xFF;
    lenBuf[2] = (bodyLen >> 8)  & 0xFF;
    lenBuf[3] =  bodyLen        & 0xFF;

    std::string frame;
    frame.reserve(4 + bodyLen);
    frame.append((const char*)lenBuf, 4);
    frame.append(jsonStr);

    // 检查队列上限
    if (client->msgQueue.size() >= m_config.clientQueueMaxSize)
    {
        // 丢弃最旧消息
        client->msgQueue.pop_front();
        m_discardCount.fetch_add(1, std::memory_order_relaxed);
    }

    // 入队列
    client->msgQueue.push_back(std::move(frame));

    // 如果队列之前为空，需要主动触发发送
    // bufferevent 写就绪时会自动发送
    if (client->msgQueue.size() == 1)
    {
        const std::string& firstFrame = client->msgQueue.front();
        bufferevent_write(client->bev, firstFrame.data(), firstFrame.size());
    }
    else
    {
        // 队列非空，等待 OnClientWriteCB 驱动后续发送
    }

    client->info.sentCount++;
    client->info.queuePending = client->msgQueue.size();
    m_sentCount.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// 延时发送回调
// ============================================================================

void ZmBroadcastServer::OnDelayedSendCB(evutil_socket_t fd, short what, void* ctx)
{
    BcScheduledTask* heapTask = (BcScheduledTask*)ctx;

    // 需要找到 owning server。由于延时任务是独立定时器回调，
    // 此处通过全局方式或存储 server 指针。简化处理：存储 server 指针在 task 中。
    // 此处先释放 heapTask，实际实现在 Task 10 中完善（加入 server 回指针）。
    delete heapTask;
}
```

---

### Task 10: 服务端实现 — 线程安全调度 + Kick + Stop

**Files:**
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.h`（给 BcScheduledTask 添加 m_server 指针）
- Modify: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`（追加 ScheduleTask、ExecuteTask、OnDispatchEventCB、Kick、Stop）

- [ ] **Step 1: 更新头文件 BcScheduledTask**

在 `zm_net_broadcast_server.h` 的 `BcScheduledTask` 结构体中添加：

```cpp
    struct BcScheduledTask
    {
        BcTaskType type;
        BcMessage  message;
        std::string clientId;
        uint32_t   delayMs;
        uint64_t   timestampMs;
        bool       isBroadcast;
        ZmBroadcastServer* m_server;    ///< 所属服务端回指针

        BcScheduledTask()
            : type(BC_TASK_SEND)
            , delayMs(0)
            , timestampMs(0)
            , isBroadcast(false)
            , m_server(nullptr)
        {}
    };
```

- [ ] **Step 2: 在 cpp 中实现 ScheduleTask + OnDispatchEventCB**

```cpp
// ============================================================================
// 跨线程任务调度
// ============================================================================

void ZmBroadcastServer::ScheduleTask(const BcScheduledTask& task)
{
    // 如果当前在事件循环线程，直接执行
    struct event_base* evbase = m_config.evLoop->GetEventBase();
    if (evbase && event_base_get_running_event(evbase))
    {
        // 在事件循环线程中
        ExecuteTask(task);
        return;
    }

    // 非事件循环线程，投递到 pending 队列然后激活 dispatch 事件
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        BcScheduledTask copy = task;
        copy.m_server = this;
        m_pendingTasks.push_back(std::move(copy));
    }

    if (m_dispatchEvent)
        event_active(m_dispatchEvent, EV_READ, 0);
}

void ZmBroadcastServer::OnDispatchEventCB(evutil_socket_t fd, short what, void* ctx)
{
    ZmBroadcastServer* server = (ZmBroadcastServer*)ctx;

    std::vector<BcScheduledTask> tasks;
    {
        std::lock_guard<std::mutex> lock(server->m_taskMutex);
        tasks.swap(server->m_pendingTasks);
    }

    for (auto& task : tasks)
    {
        server->ExecuteTask(task);
    }
}

void ZmBroadcastServer::ExecuteTask(const BcScheduledTask& task)
{
    switch (task.type)
    {
    case BC_TASK_START:
        DoStart();
        break;
    case BC_TASK_STOP:
        DoStop();
        break;
    case BC_TASK_SEND:
        DoSend(task.message, task.clientId, task.isBroadcast,
               task.delayMs, task.timestampMs);
        break;
    case BC_TASK_KICK:
        RemoveClient(task.clientId);
        break;
    }
}

// ============================================================================
// KickClient
// ============================================================================

bool ZmBroadcastServer::KickClient(const std::string& clientId)
{
    if (clientId.empty())
        return false;

    // 检查客户端是否存在
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.find(clientId) == m_clients.end())
            return false;
    }

    BcScheduledTask task;
    task.type = BC_TASK_KICK;
    task.clientId = clientId;
    ScheduleTask(task);
    return true;
}

// ============================================================================
// 生命周期 — Stop
// ============================================================================

void ZmBroadcastServer::Stop()
{
    ZM_BROADCAST_STATE expected = ZM_BC_STATE_LISTENING;
    if (!m_state.compare_exchange_strong(expected, ZM_BC_STATE_STOPPING))
    {
        expected = ZM_BC_STATE_IDLE;
        if (m_state.load() == ZM_BC_STATE_STOPPED ||
            m_state.load() == ZM_BC_STATE_IDLE)
            return; // 已停止或未启动
        // STARTING 等其他状态也尝试停止
    }

    BcScheduledTask task;
    task.type = BC_TASK_STOP;
    ScheduleTask(task);
}

void ZmBroadcastServer::DoStop()
{
    // 关闭监听器
    if (m_listener)
    {
        evconnlistener_free(m_listener);
        m_listener = nullptr;
    }

    // 断开所有客户端
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        // 收集所有 clientId（因为 RemoveClient 会修改 map）
        std::vector<std::string> allIds;
        allIds.reserve(m_clients.size());
        for (const auto& pair : m_clients)
            allIds.push_back(pair.first);

        // 释放锁后逐个移除（RemoveClient 内部会加锁）
        for (const auto& id : allIds)
            RemoveClient(id);
    }

    // 释放重试定时器
    if (m_retryTimer)
    {
        event_free(m_retryTimer);
        m_retryTimer = nullptr;
    }

    // 释放 dispatch 事件
    if (m_dispatchEvent)
    {
        event_free(m_dispatchEvent);
        m_dispatchEvent = nullptr;
    }

    m_startTime = 0;
    m_state.store(ZM_BC_STATE_STOPPED);

    if (m_callbacks.onListenStopped)
        m_callbacks.onListenStopped();

    DEFAULT_LOG_INFO("[BcServer] Server stopped");
}
```

- [ ] **Step 3: 完善 OnDelayedSendCB**

在 Task 9 中临时占位的 OnDelayedSendCB 替换为完整实现：

```cpp
void ZmBroadcastServer::OnDelayedSendCB(evutil_socket_t fd, short what, void* ctx)
{
    BcScheduledTask* heapTask = (BcScheduledTask*)ctx;
    if (!heapTask || !heapTask->m_server)
    {
        delete heapTask;
        return;
    }

    ZmBroadcastServer* server = heapTask->m_server;

    // 重新入队为立即发送
    BcScheduledTask immediateTask = *heapTask;
    immediateTask.delayMs = 0;
    immediateTask.timestampMs = 0;

    // 释放旧的定时器资源（由 libevent 管理，无需手动 free）
    delete heapTask;

    // 投递执行
    server->ScheduleTask(immediateTask);
}
```

- [ ] **Step 4: 更新 OnClientWriteCB 驱动词队列消费**

```cpp
void ZmBroadcastServer::OnClientWriteCB(struct bufferevent* bev, void* ctx)
{
    BcClient* client = (BcClient*)ctx;

    // 写完成后从队列取出下一帧：当前帧已由 bufferevent 内部发送完毕
    if (!client->msgQueue.empty())
    {
        client->msgQueue.pop_front();
    }

    // 发送下一帧
    if (!client->msgQueue.empty())
    {
        const std::string& nextFrame = client->msgQueue.front();
        bufferevent_write(bev, nextFrame.data(), nextFrame.size());
    }
}
```

---

### Task 11: 编译验证 + 修复

**Files:**
- Check: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_base.h`
- Check: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_base.cpp`
- Check: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.h`
- Check: `A:\ZiMo\ZiMoPublic\net\zm_net_broadcast_server.cpp`

- [ ] **Step 1: 检查所有 include 完整性**

确认各文件引入的头文件清单：

`zm_net_broadcast_base.h`：
- `<cstdint>`, `<string>`, `<vector>`
- 前向声明 `struct bufferevent`、`struct evbuffer`

`zm_net_broadcast_base.cpp`：
- `"zm_net_broadcast_base.h"`
- `<event2/buffer.h>`, `<event2/bufferevent.h>`
- `<random>`, `<sstream>`, `<chrono>`, `<iomanip>`, `<cstring>`

`zm_net_broadcast_server.h`：
- `"zm_net_broadcast_base.h"`
- `<atomic>`, `<cstdint>`, `<deque>`, `<functional>`, `<mutex>`, `<string>`, `<unordered_map>`, `<vector>`
- 前向声明 `struct event_base`、`struct evconnlistener`、`struct bufferevent`、`struct event`、`class ZmEvBaseRunLoop`

`zm_net_broadcast_server.cpp`：
- `"zm_net_broadcast_server.h"`
- `"../util/zm_util_sys.h"`（ZmSystem::CurrentTimeMills 备选，实际用 BcNowMillis）
- `"../json/zm_json.h"`
- `"zm_net_runloop.h"`（ZmEvBaseRunLoop 定义）
- `"zm_logger.h"`
- `<event2/listener.h>`, `<event2/bufferevent.h>`, `<event2/buffer.h>`, `<event2/event.h>`

- [ ] **Step 2: 检查已知问题并修复**

1. `evbase && event_base_get_running_event(evbase)` — `event_base_get_running_event` 不是 libevent 公开 API，需替换线程检测方案：

在头文件中添加 `std::thread::id m_loopThreadId` 成员，在 `DoStart()` 中记录当前线程 ID：
```cpp
m_loopThreadId = std::this_thread::get_id();
```

`ScheduleTask` 中判断：
```cpp
if (std::this_thread::get_id() == m_loopThreadId)
{
    ExecuteTask(task);
    return;
}
```

2. 补充缺失 include：`<thread>`、`<algorithm>`（std::find）

3. `DEFAULT_LOG_WARN` — 确认 `zm_logger.h` 提供此宏，若无则替换为 `DEFAULT_LOG_INFO`

- [ ] **Step 3: 编译**

```bash
cd A:/ZiMo/ZiMoService
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64 /t:Build
```

预期：编译通过，无错误。

- [ ] **Step 4: 修复编译错误**

根据实际编译输出逐项修复。常见可能问题：
- `evutil_inet_pton` 在 Windows 下需要包含 `<winsock2.h>`（libevent 内部已包含）
- `getsockname` 需要 `<winsock2.h>` 或 `<sys/socket.h>`（通过 libevent）
- `struct sockaddr_in` 需包含 `<event2/util.h>` 或 `<winsock2.h>`

- [ ] **Step 5: 提交**

```bash
git add ZiMoPublic/net/zm_net_broadcast_base.h
git add ZiMoPublic/net/zm_net_broadcast_base.cpp
git add ZiMoPublic/net/zm_net_broadcast_server.h
git add ZiMoPublic/net/zm_net_broadcast_server.cpp
git commit -m "feat: 新增TCP广播服务端模块

实现基于libevent+ZmEvBaseRunLoop的一对多消息推送服务端:
- 帧协议(4字节大端长度前缀+JSON body)
- 客户端握手(settings→confirm_settings→分配client_id)
- 双向活动检测心跳(ping/pong)
- Tag过滤订阅(subscribe/unsubscribe)
- 立即/延时/定时消息发送(线程安全)
- 每客户端独立消息队列(溢出丢弃最旧)
- 连接数限制、按client_id踢出"
```
