# Broadcast 集成实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 创建 BroadcastManager 包装 ZmBroadcastServer，集成到 NetDock/ServicePortal/前端。

**Architecture:** 遵循 HttpServerManager 模式 — BroadcastManager 包装 ZmBroadcastServer，NetDock 管理生命周期，ServicePortal 通过 getStatus 暴露状态，前端展示 Broadcast 卡片。

**Tech Stack:** C++17, ZmBroadcastServer, ZmEvBaseRunLoop

---

### Task 1: service_define.h 添加广播端口常量

**Files:**
- Modify: `A:\ZiMo\ZiMoService\service_define.h`

- [ ] **Step 1: 添加端口定义**

在 `#define ZM_JSONRPC_SERVER_PORT 39440` 后面添加：

```cpp
#define ZM_BROADCAST_SERVER_PORT 39640
```

---

### Task 2: 创建 broadcast_manager.h

**Files:**
- Create: `A:\ZiMo\ZiMoService\broadcast_manager.h`

```cpp
#ifndef BROADCAST_MANAGER_H
#define BROADCAST_MANAGER_H

#include "zm_net_broadcast_server.h"

/**
 * @brief 广播服务端管理器
 *
 * 包装 ZmBroadcastServer，遵循 HttpServerManager 模式。
 * 提供生命周期管理和对外的状态查询 / 消息推送接口。
 */
class BroadcastManager
{
public:
    BroadcastManager();
    ~BroadcastManager();

    /**
     * @brief 启动广播服务端
     * @param evLoop  事件循环线程
     * @param port    监听端口（0 = 随机）
     * @return true 成功，false 失败
     */
    bool Open(ZmEvBaseRunLoop* evLoop, uint16_t port);

    /** @brief 停止广播服务端 */
    void Close();

    /** @brief 是否正在运行 */
    bool IsOpen() const;

    /** @brief 获取实际监听端口 */
    uint16_t GetPort() const;

    /** @brief 获取当前连接数 */
    int GetConnectionCount() const;

    /** @brief 获取累计发送数 */
    uint64_t GetSentCount() const;

    /**
     * @brief 向所有匹配 tag 的客户端广播消息
     * @param topic    主题
     * @param content  内容（JSON 字符串）
     * @param tag      过滤标签
     * @return true 成功投递
     */
    bool Broadcast(const std::string& topic, const std::string& content, const std::string& tag);

    /**
     * @brief 向指定客户端发送消息
     * @param clientId  目标客户端 ID
     * @param topic     主题
     * @param content   内容
     * @param tag       过滤标签
     * @return true 成功投递
     */
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);

private:
    ZmBroadcastServer* m_server;       ///< 底层广播服务端
    BcServerConfig     m_config;       ///< 服务端配置
    BcServerCallbacks  m_callbacks;    ///< 事件回调
    uint16_t           m_port;         ///< 实际监听端口
};

#endif // BROADCAST_MANAGER_H
```

---

### Task 3: 创建 broadcast_manager.cpp

**Files:**
- Create: `A:\ZiMo\ZiMoService\broadcast_manager.cpp`

```cpp
#include "broadcast_manager.h"
#include "zm_logger.h"

BroadcastManager::BroadcastManager()
    : m_server(nullptr)
    , m_port(0)
{
}

BroadcastManager::~BroadcastManager()
{
    Close();
}

bool BroadcastManager::Open(ZmEvBaseRunLoop* evLoop, uint16_t port)
{
    if (m_server)
        return true;

    if (!evLoop)
    {
        DEFAULT_LOG_ERROR("[BroadcastMgr] evLoop is null");
        return false;
    }

    m_config.listenIp = "0.0.0.0";
    m_config.listenPort = port;
    m_config.evLoop = evLoop;
    m_config.heartbeatTime = 60;
    m_config.handshakeTimeout = 10;
    m_config.clientQueueMaxSize = 1024;

    m_callbacks.onListenSuccess = [this](uint16_t actualPort) {
        m_port = actualPort;
        DEFAULT_LOG_INFO("[BroadcastMgr] Listening on port {}", actualPort);
    };

    m_callbacks.onListenFailed = [](const std::string& error) {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Listen failed: {}", error);
    };

    m_callbacks.onListenStopped = []() {
        DEFAULT_LOG_INFO("[BroadcastMgr] Server stopped");
    };

    m_callbacks.onError = [](const std::string& error) {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Error: {}", error);
    };

    m_callbacks.onClientOnline = [](const BcClientInfo& info) {
        DEFAULT_LOG_INFO("[BroadcastMgr] Client online: {} ({}:{})",
                         info.clientId, info.ip, info.port);
    };

    m_callbacks.onClientOffline = [](const BcClientInfo& info) {
        DEFAULT_LOG_INFO("[BroadcastMgr] Client offline: {} ({}:{})",
                         info.clientId, info.ip, info.port);
    };

    m_server = new ZmBroadcastServer(m_config, m_callbacks);

    if (!m_server->Start())
    {
        DEFAULT_LOG_ERROR("[BroadcastMgr] Start failed");
        delete m_server;
        m_server = nullptr;
        return false;
    }

    return true;
}

void BroadcastManager::Close()
{
    if (m_server)
    {
        m_server->Stop();
        delete m_server;
        m_server = nullptr;
    }
    m_port = 0;
}

bool BroadcastManager::IsOpen() const
{
    if (!m_server)
        return false;
    ZM_BROADCAST_STATE state = m_server->GetState();
    return state == ZM_BC_STATE_LISTENING;
}

uint16_t BroadcastManager::GetPort() const
{
    return m_port;
}

int BroadcastManager::GetConnectionCount() const
{
    if (m_server)
        return m_server->GetConnectionCount();
    return 0;
}

uint64_t BroadcastManager::GetSentCount() const
{
    if (m_server)
        return m_server->GetSentCount();
    return 0;
}

bool BroadcastManager::Broadcast(const std::string& topic, const std::string& content,
                                  const std::string& tag)
{
    if (!m_server)
        return false;
    return m_server->Broadcast(topic, content, tag);
}

bool BroadcastManager::Send(const std::string& clientId, const std::string& topic,
                             const std::string& content, const std::string& tag)
{
    if (!m_server)
        return false;
    return m_server->Send(clientId, topic, content, tag);
}
```

---

### Task 4: 修改 net_dock.h

**Files:**
- Modify: `A:\ZiMo\ZiMoService\net_dock.h`

- [ ] **Step 1: 在 include 区域添加**

```cpp
#include "broadcast_manager.h"
```

- [ ] **Step 2: 在 public 区域添加方法声明**

```cpp
    void OpenBroadcastServer(uint16_t port);
    void CloseBroadcastServer();
    BroadcastManager* GetBroadcastManager();
    bool IsBroadcastOpen() const;
```

- [ ] **Step 3: 在 private 成员变量添加**

```cpp
    BroadcastManager*      m_broadcastMgr;        ///< 广播服务端管理器
```

---

### Task 5: 修改 net_dock.cpp

**Files:**
- Modify: `A:\ZiMo\ZiMoService\net_dock.cpp`

- [ ] **Step 1: 构造函数初始化添加**

```cpp
    , m_broadcastMgr(nullptr)
```

- [ ] **Step 2: UnInit() 中添加关闭**

在 `CloseHttpServer()` 之前添加：

```cpp
    CloseBroadcastServer();
```

- [ ] **Step 3: 添加方法实现**

```cpp
void NetDock::OpenBroadcastServer(uint16_t port)
{
    if (!m_broadcastMgr)
    {
        m_broadcastMgr = new BroadcastManager();
        m_broadcastMgr->Open(m_hubProxyMgr->EvBase(), port);
    }
}

void NetDock::CloseBroadcastServer()
{
    if (m_broadcastMgr)
    {
        m_broadcastMgr->Close();
        delete m_broadcastMgr;
        m_broadcastMgr = nullptr;
    }
}

BroadcastManager* NetDock::GetBroadcastManager()
{
    return m_broadcastMgr;
}

bool NetDock::IsBroadcastOpen() const
{
    return m_broadcastMgr && m_broadcastMgr->IsOpen();
}
```

---

### Task 6: 修改 service_portal.cpp

**Files:**
- Modify: `A:\ZiMo\ZiMoService\service_portal.cpp`

- [ ] **Step 1: getStatus 中添加 broadcast 字段**

在 `hub` 状态之后添加：

```cpp
        result["broadcast"]["status"]      = (m_netDock && m_netDock->IsBroadcastOpen()) ? "running" : "stopped";
        result["broadcast"]["port"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetPort() : 0;
        result["broadcast"]["connections"] = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetConnectionCount() : 0;
        result["broadcast"]["sent"]        = (m_netDock && m_netDock->GetBroadcastManager()) ? m_netDock->GetBroadcastManager()->GetSentCount() : 0;
```

---

### Task 7: 修改 service_center.cpp 启动广播

**Files:**
- Modify: `A:\ZiMo\ZiMoService\service_center.cpp`

在 `m_netDock->OpenHttpServer(wwwRoot.c_str());` 之后添加：

```cpp
    m_netDock->OpenBroadcastServer(ZM_BROADCAST_SERVER_PORT);
```

---

### Task 8: 修改前端 www/js/app.js

**Files:**
- Modify: `A:\ZiMo\ZiMoService\www\js\app.js`

- [ ] **Step 1: 卡片定义修改**

```js
        { key:'ws',      icon:'📡', label:'Broadcast',      val:'—', ok:false, sub:'' },
```

- [ ] **Step 2: fetchStatus 中修改**

```js
        if (d.broadcast) {
          const [v, ok] = s(d.broadcast.status); set(4, v, ok, '端口 ' + d.broadcast.port);
        }
```

---

### Task 9: 编译验证

```bash
cd A:/ZiMo/ZiMoService
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64
```
