# Broadcast 集成设计文档

## 概述

将 ZmBroadcastServer 包装为 BroadcastManager，遵循 HttpServerManager 模式，在 NetDock 中管理生命周期，在 ServicePortal 中暴露状态查询，前端展示 Broadcast 状态卡片。

## 一、BroadcastManager

### 文件

- `A:\ZiMo\ZiMoService\broadcast_manager.h`
- `A:\ZiMo\ZiMoService\broadcast_manager.cpp`

### 接口

```cpp
class BroadcastManager
{
public:
    BroadcastManager();
    ~BroadcastManager();

    bool Open(ZmEvBaseRunLoop* evLoop, uint16_t port);
    void Close();
    bool IsOpen() const;

    uint16_t GetPort() const;
    int GetConnectionCount() const;
    uint64_t GetSentCount() const;

    bool Broadcast(const std::string& topic, const std::string& content, const std::string& tag);
    bool Send(const std::string& clientId, const std::string& topic,
              const std::string& content, const std::string& tag);

private:
    ZmBroadcastServer* m_server;
    BcServerCallbacks m_callbacks;
    uint16_t m_port;
};
```

## 二、NetDock 集成

### 新增成员

```cpp
BroadcastManager* m_broadcastMgr;
```

### 新增方法

```cpp
void OpenBroadcastServer(uint16_t port);
void CloseBroadcastServer();
BroadcastManager* GetBroadcastManager();
bool IsBroadcastOpen() const;
```

### 生命周期

- 启动：`OpenHttpServer` → `OpenBroadcastServer`
- 关闭：`UnInit` 中按倒序 `CloseBroadcastServer` → `CloseHttpServer` → ...

## 三、ServicePortal

### getStatus 响应新增

```json
{
  "broadcast": {
    "status": "running" | "stopped",
    "port": 39640,
    "connections": 5,
    "sent": 1234
  }
}
```

## 四、前端

### www/js/app.js

- 卡片索引 4 从 `WebSocket` 改为 `Broadcast`
- 读取 `d.broadcast` 替代 `d.websocket`

## 五、常量

### service_define.h

```cpp
#define ZM_BROADCAST_SERVER_PORT 39640
```
