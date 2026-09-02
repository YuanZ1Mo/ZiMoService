# 广播模块（Broadcast）设计文档

## 概述

在 ZiMoPublic/net/ 中新增广播模块，基于 libevent 实现 TCP 一对多消息推送服务端。

### 文件结构

```
ZiMoPublic/
  net/
    zm_net_broadcast_base.h          # 公共定义（状态枚举、消息结构、帧协议、回调类型）
    zm_net_broadcast_base.cpp        # 公共实现（帧协议编解码等）
    zm_net_broadcast_server.h        # 服务端声明
    zm_net_broadcast_server.cpp      # 服务端实现
```

base 层预留未来可能的 UDP 或其他广播变体扩展。

---

## 一、公共层（zm_net_broadcast_base）

### 1.1 状态枚举

```cpp
enum ZM_BROADCAST_STATE
{
    ZM_BC_STATE_IDLE      = 0,  // 未启动
    ZM_BC_STATE_STARTING  = 1,  // 启动中（正在绑定监听）
    ZM_BC_STATE_LISTENING = 2,  // 已监听
    ZM_BC_STATE_STOPPING  = 3,  // 停止中（关闭连接、清理 pending）
    ZM_BC_STATE_STOPPED   = 4,  // 已停止
    ZM_BC_STATE_ERROR     = 5,  // 错误
};
```

状态流转：`IDLE → STARTING → LISTENING → STOPPING → STOPPED`，任意阶段可进入 `ERROR`。

### 1.2 消息帧协议

4 字节大端长度前缀 + JSON body。

```cpp
// 从 evbuffer 读取并解析一条完整帧，返回 JSON 字符串
std::string BcFrameDecode(struct evbuffer* input);

// 将 JSON 字符串编码为帧并写入 bufferevent 输出缓冲区
bool BcFrameEncode(struct bufferevent* bev, const std::string& json);
```

`BcFrameDecode` 内部先 peek 4 字节解出长度，若 evbuffer 数据不足则返回空字符串等待更多数据。

### 1.3 消息结构

```cpp
struct BcMessage
{
    std::string id;         // 消息唯一ID（UUID v4）
    std::string topic;      // 主题
    std::string tag;        // 过滤标签（仅用于匹配，不进入线上 JSON）
    std::string content;    // 内容（JSON 任意类型）
    std::string timestamp;  // 发送时 ISO-8601 时间戳
};
```

线上 JSON 格式：`{"id":"...","timestamp":"...","topic":"...","content":...}`

注意：`tag` 不在线上 JSON 中，仅用于服务端过滤匹配。

### 1.4 客户端信息

```cpp
struct BcClientInfo
{
    std::string clientId;              // 服务端分配的唯一 ID
    std::string ip;                    // 对端 IP
    uint16_t port;                     // 对端端口
    uint64_t connectTime;              // 连接建立时间戳（毫秒）
    std::vector<std::string> tags;     // 当前订阅的 tag 列表
    size_t queuePending;               // 队列中待发送消息数
    uint64_t lastActiveTime;           // 最后活跃时间戳（毫秒）
    uint64_t sentCount;                // 已发送消息数
};
```

### 1.5 内部协议消息格式

以下消息通过帧协议承载（JSON body），用于服务端与客户端之间的控制通信：

**握手阶段（服务端→客户端）：**
```json
{
    "settings": {
        "heartbeat_time": 60
    }
}
```

**握手确认（客户端→服务端）：**
```json
{"action": "confirm_settings"}
```

**心跳（服务端→客户端）：**
```json
{"action": "ping"}
```

**心跳响应（客户端→服务端）：**
```json
{"action": "pong"}
```

**订阅（客户端→服务端）：**
```json
{"action": "subscribe", "tags": ["tag1", "tag2"]}
```

**取消订阅（客户端→服务端）：**
```json
{"action": "unsubscribe", "tags": ["tag3"]}
```

---

## 二、服务端（zm_net_broadcast_server）

### 2.1 配置结构

```cpp
struct BcServerConfig
{
    std::string listenIp;           // 监听地址，默认 "0.0.0.0"
    uint16_t listenPort;            // 监听端口，0 = 随机分配
    int maxConnections;             // 最大连接数，0 = 不限制
    int heartbeatTime;              // 心跳超时秒数，默认 60
    int handshakeTimeout;           // 握手超时秒数，默认 10
    size_t clientQueueMaxSize;      // 每客户端消息队列上限，默认 1024
    ZmEvBaseRunLoop* evLoop;       // 事件循环线程（必填，由调用方提供，内部通过 GetEventBase() 获取 event_base）
};
```

### 2.2 回调结构

```cpp
struct BcServerCallbacks
{
    // --- 监听事件 ---
    std::function<void(uint16_t port)> onListenSuccess;   // port 为实际监听端口
    std::function<void(const std::string& error)> onListenFailed;
    std::function<void()> onListenStopped;
    std::function<void(const std::string& error)> onError;

    // --- 客户端事件 ---
    std::function<void(const BcClientInfo&)> onClientOnline;
    std::function<void(const BcClientInfo&)> onClientOffline;  // 断开前最后一次快照
};
```

### 2.3 公共接口

```cpp
class ZmBroadcastServer
{
public:
    ZmBroadcastServer(const BcServerConfig& config, const BcServerCallbacks& cbs);
    ~ZmBroadcastServer();

    // --- 生命周期 ---
    bool Start();        // 异步：启动监听，结果通过回调通知
    void Stop();         // 停止服务，直接断开所有客户端连接

    // --- 状态查询 ---
    ZM_BROADCAST_STATE GetState() const;
    uint16_t GetPort() const;
    int GetConnectionCount() const;
    int GetMaxConnections() const;
    size_t GetGlobalQueueSize() const;
    uint64_t GetRunningTime() const;      // 返回已运行秒数
    uint64_t GetSentCount() const;
    uint64_t GetDiscardCount() const;

    // --- 客户端查询 ---
    BcClientInfo GetClientInfo(const std::string& clientId) const;
    std::vector<BcClientInfo> GetAllClients() const;

    // --- 立即发送 ---
    // 向指定客户端发送（仅当其订阅的 tag 匹配时送达）
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);
    // 向所有匹配 tag 的客户端发送
    bool Broadcast(const std::string& topic, const std::string& content,
                   const std::string& tag);

    // --- 延时发送（delayMs 毫秒后发送一次） ---
    bool SendDelayed(const std::string& clientId, const std::string& topic,
                     const std::string& content, const std::string& tag, uint32_t delayMs);
    bool BroadcastDelayed(const std::string& topic, const std::string& content,
                          const std::string& tag, uint32_t delayMs);

    // --- 定时发送（在指定 Unix 毫秒时间戳发送一次） ---
    bool SendAt(const std::string& clientId, const std::string& topic,
                const std::string& content, const std::string& tag, uint64_t timestampMs);
    bool BroadcastAt(const std::string& topic, const std::string& content,
                     const std::string& tag, uint64_t timestampMs);

    // --- 客户端管理 ---
    bool KickClient(const std::string& clientId);

    // --- 运行时配置修改 ---
    void SetMaxConnections(int max);
    void SetHeartbeatTime(int seconds);
    void SetClientQueueMaxSize(size_t max);
};
```

所有发送接口线程安全：若调用线程非事件循环线程，内部通过 `event_active` 将发送任务投递到事件循环线程执行。

### 2.4 内部结构

```
ZmBroadcastServer
  ├── BcServerConfig m_config                 # 配置副本
  ├── BcServerCallbacks m_callbacks           # 回调副本
  ├── std::atomic<ZM_BROADCAST_STATE> m_state # 当前状态
  ├── evconnlistener* m_listener              # libevent 监听器
  ├── std::unordered_map<std::string, BcClient> m_clients
  │     ├── std::string clientId
  │     ├── bufferevent* bev                  # 客户端连接
  │     ├── BcClientInfo info                 # 客户端元信息
  │     ├── std::deque<std::string> msgQueue  # 已编码帧的消息队列
  │     ├── event* heartbeatTimer             # 该客户端的心跳监测定时器
  │     ├── event* handshakeTimer             # 握手超时定时器
  │     ├── bool handshakeDone                # 握手是否完成
  │     └── uint64_t lastActiveTime           # 最后活跃时间（毫秒）
  ├── std::atomic<uint64_t> m_sentCount       # 累计发送成功数
  ├── std::atomic<uint64_t> m_discardCount    # 累计丢弃数（队列满时丢弃）
  └── uint64_t m_startTime                    # Start() 成功时的时间戳
```

---

## 三、客户端生命周期

```
TCP 连接建立
  → 服务端发送 settings JSON 帧
  → 启动 handshakeTimeout 定时器
      ├── 超时 → 断开连接
      └── 收到 confirm_settings → 分配 client_id（UUID v4）
            → 填充 BcClientInfo
            → 回调 onClientOnline
            → 启动心跳监测定时器
                  │
            [正常通信阶段]
              - 收到数据 → 刷新 lastActiveTime
              - 收到 subscribe/unsubscribe → 更新 tags
              - 收到 ping → 回复 pong
              - 发送消息 → 匹配 tag → 帧编码 → 入客户端队列 → bufferevent_write
                  │
            断开触发:
              - bufferevent EOF/ERROR 事件
              - 心跳超时（now - lastActiveTime >= heartbeatTime）
              - KickClient() 主动踢出
              - Stop() 服务端停止
                  │
                  → 回调 onClientOffline(clientInfo 快照)
                  → 释放 bufferevent、定时器
                  → 清空消息队列
                  → 从 m_clients 移除
```

---

## 四、心跳与活动检测

### 机制

每个客户端维护 `lastActiveTime`（收到任何数据都更新，包括 pong / subscribe / 任意 JSON 消息）。服务端通过 libevent 定时器周期性检查。

### 参数计算

| 参数 | 值 |
|------|-----|
| 心跳超时阈值 | `heartbeatTime` 秒（配置值，默认 60） |
| ping 间隔 | `heartbeatTime / 2` 秒（内部计算，默认 30） |
| 检查周期 | 每个客户端独立定时器，`heartbeatTime / 2` 秒触发一次 |

### 检查逻辑

每次定时器触发：
1. `now - lastActiveTime >= heartbeatTime` → 超时，断开连接
2. `now - lastDataSentTime >= heartbeatTime / 2` → 发送 ping
3. 否则 → 不操作

收到 pong 或任何数据时刷新 `lastActiveTime`。

### 优点

- 活跃通信时零心跳开销（频繁消息数据本身就算活跃）
- 服务端控制节奏，客户端只需被动响应
- pong 超时不单独设阈值，统一用 `lastActiveTime` + 任何数据

---

## 五、消息发送流程

### 5.1 立即发送

```
Send/Broadcast（任意线程调用）
  1. 生成 BcMessage：UUID id + 当前时间戳 + 参数
  2. 若当前非事件循环线程 → event_active 投递到事件循环线程
  3. 事件循环线程：
     Send: 查找 clientId → 匹配 tag → 帧编码 → 入客户端队列 → bufferevent_write
     Broadcast: 遍历 m_clients → 逐个匹配 tag → 帧编码 → 入各自队列 → bufferevent_write
```

### 5.2 延时发送

内部创建一次性 libevent timeout 定时器，到期后在事件循环线程执行发送。实现上复用立即发送逻辑，延迟执行。

### 5.3 定时发送

计算 `timestampMs - now` 的差值作为延时毫秒数，若差值 <= 0 则立即发送，否则同延时发送。

### 5.4 队列管理

- 每个客户端独立 `std::deque<std::string>` 存储已帧编码的消息
- 新消息入队时若 `queue.size() >= clientQueueMaxSize`，pop_front 丢弃最旧消息，`m_discardCount++`
- 通过 bufferevent 的写完成回调从队列取下一帧发送

### 5.5 慢客户端处理

bufferevent 输出缓冲区满时，libevent 自动缓冲。应用层客户端队列负责排队等待。若队列达上限则丢弃最旧消息。

---

## 六、线程安全

所有 libevent 对象的操作（bufferevent_write、event_add、event_del 等）通过事件循环线程执行。对外发送接口若在非事件循环线程调用，内部通过 `event_active` + 自定义控制事件投递到事件循环线程执行。

查询接口（GetState、GetClientInfo 等）使用 `std::atomic` 或 mutex 保护共享数据，可在任意线程安全调用。

---

## 七、依赖项

- `libevent` — evconnlistener、bufferevent、event_base、event
- `ZiMoPublic/util/` — ZmThread（仅供基类引用）、zm_util_libevent（evthread 初始化）
- `ZiMoPublic/json/` — zm_json（nlohmann/json 封装，消息序列化/反序列化）
- 不依赖 TLS/SSL（二期扩展）

---

## 八、二期预留

| 功能 | 说明 |
|------|------|
| 消息加密 | 通过 bufferevent_openssl 启用 TLS |
| 离线消息缓存 | 客户端断开后未发完的消息持久化，重连后补发 |
| 多频道/分组 | 更复杂的 tag 匹配逻辑（通配符、层级 tag） |
| UDP 广播 | 基于本设计 base 层扩展 |
| 广播客户端 | 连接远程广播服务端，接收推送 |

---

## 九、命名规范

遵循项目 CLAUDE.md 及 ZiMoPublic 现有风格：
- 文件名：`zm_net_broadcast_<module>.h/.cpp`
- 类名：`ZmBroadcast` 前缀（如 `ZmBroadcastServer`）
- 结构体：`Bc` 前缀（如 `BcMessage`、`BcClientInfo`、`BcServerConfig`）
- 枚举：`ZM_BC_STATE_` 前缀（`ZM_BROADCAST_STATE` 类型名）
- 函数：PascalCase（如 `GetState()`、`KickClient()`）
- 成员变量：`m_` 前缀
- 全局变量：`g_` 前缀
- 注释：中文 `@brief @param @return @example`
- 头文件守卫：`ZM_NET_BROADCAST_<MODULE>_H`
