# 广播客户端设计文档

## 概述

基于 libevent + ZmEvBaseRunLoop 实现 TCP 广播客户端，与服务端配套，复用 `zm_net_broadcast_base` 的帧协议。

### 文件结构

```
ZiMoPublic/net/
  zm_net_broadcast_client.h      # 客户端声明
  zm_net_broadcast_client.cpp    # 客户端实现
```

依赖已有的 `zm_net_broadcast_base.h/.cpp`（帧协议、UUID、时间戳）。

---

## 一、状态枚举

复用 `ZM_BROADCAST_STATE`（base 层已定义），客户端语义：
- `IDLE` → `STARTING` → `LISTENING`(已连接) → `STOPPING` → `STOPPED` + `ERROR`

---

## 二、客户端配置

```cpp
struct BcClientConfig
{
    std::string serverIp;         // 服务端 IP
    uint16_t    serverPort;       // 服务端端口
    int         handshakeTimeout; // 握手超时秒数，默认 10
    std::vector<std::string> initialTags;  // 初始订阅的 tag 列表
    ZmThreadPool* threadPool;     // 业务层消息回调线程池（必填）
};
```

---

## 三、客户端回调

```cpp
struct BcClientCallbacks
{
    /// 连接成功 + 握手完成回调，参数为服务端 IP 和端口
    std::function<void(const std::string& serverIp, uint16_t port)> onConnected;

    /// 连接失败回调，参数为错误描述
    std::function<void(const std::string& error)> onConnectFailed;

    /// 断开回调（主动断开 / 服务端断开 / 错误）
    std::function<void()> onDisconnected;

    /// 错误回调
    std::function<void(const std::string& error)> onError;

    /// 业务消息回调（线程池线程中执行），参数为 topic 和 content
    std::function<void(const std::string& topic, const std::string& content)> onMessage;
};
```

---

## 四、公共接口

```cpp
class ZmBroadcastClient
{
public:
    ZmBroadcastClient(const BcClientConfig& config, const BcClientCallbacks& cbs);
    ~ZmBroadcastClient();

    // --- 生命周期 ---
    bool Connect();       // 异步连接，结果通过回调通知
    void Disconnect();    // 断开连接

    // --- 状态查询 ---
    ZM_BROADCAST_STATE GetState() const;
    std::string GetServerIp() const;
    uint16_t GetServerPort() const;
    uint64_t GetRunningTime() const;
    uint64_t GetReceivedCount() const;

    // --- Tag 管理 ---
    bool Subscribe(const std::vector<std::string>& tags);
    bool Unsubscribe(const std::vector<std::string>& tags);

private:
    // 内部：线程安全发送 JSON
    void SendJson(const std::string& json);

    // 内部：连接后自动发送当前 tag 列表
    void SendCurrentTags();

    // 内部：消息路由
    void HandleMessage(const std::string& json);

    // 内部：事件循环方法
    void DoConnect();
    void DoDisconnect();

    // libevent 回调
    static void OnConnectCB(struct bufferevent* bev, short events, void* ctx);
    static void OnReadCB(struct bufferevent* bev, void* ctx);
    static void OnEventCB(struct bufferevent* bev, short events, void* ctx);

    // 成员变量
    BcClientConfig    m_config;
    BcClientCallbacks m_callbacks;
    std::atomic<ZM_BROADCAST_STATE> m_state;

    ZmEvBaseRunLoop*   m_evLoop;
    struct bufferevent* m_bev;
    uint64_t m_startTime;
    std::atomic<uint64_t> m_receivedCount;
    std::vector<std::string> m_currentTags;    // 当前订阅列表
    std::mutex m_tagsMutex;
    std::mutex m_sendMutex;
    struct event* m_retryTimer;
    struct event* m_handshakeTimer;
    bool m_handshakeDone;
    std::thread::id m_loopThreadId;
};
```

---

## 五、消息路由

```
收到帧 → 解码 JSON
  ├── {"settings":{...}}     → 内部：confirm_settings + 发送当前 tags + 回调 onConnected
  ├── {"action":"ping"}      → 内部：回复 pong
  ├── {"id":"...","topic":...} → 业务：投 ZmThreadPool → onMessage(topic, content)
  └── 其他                    → 日志警告
```

---

## 六、连接流程

```
Connect()
  → STARTING → DoConnect()
    → bufferevent_socket_new + bufferevent_socket_connect
      ├── 连接成功 → 发启 handshakeTimer(10s)
      │     └── 收到 settings → confirm_settings + subscribe tags → CONNECTED
      └── 连接失败 → 回调 onConnectFailed → 1s 后重试
```

---

## 七、心跳与断开

- 收到 ping → SendJson(`{"action":"pong"}`)，内部处理
- 服务端断开 → `BEV_EVENT_EOF` → onDisconnected → 重试连接
- Disconnect() → STOPPING → 断开 bev → STOPPED

## 八、线程安全

- 消息回调通过配置的 `ZmThreadPool` 异步执行
- 发送接口内部通过 `event_active` 投递到事件循环线程
- 状态查询使用 `std::atomic`
- Tag 列表用 `std::mutex` 保护
