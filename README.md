# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，提供异步网络基础设施，基于 libevent 事件循环对外暴露 WebSocket 和 HTTP JSON-RPC 接口。

## 功能概览

- **高性能异步 I/O** — 单线程 libevent 事件循环，零额外线程开销
- **WebSocket 服务** — 端口 37310，支持实时双向通信
- **HTTP JSON-RPC 服务** — 端口 39440，路径 `/ZiMo/ZiMoService`，提供标准化 RPC 调用
- **TAP 代理链** — 可扩展的协议路由架构，支持多协议前端共享 Hub
- **异步 DNS 解析** — 基于 libevent `evdns_getaddrinfo`，无阻塞、事件驱动
- **Windows 服务生命周期** — 支持安装、卸载、调试模式，完整的会话/电源/关机事件处理
- **会话感知** — 监控用户登录/登出/锁屏/解锁/远程桌面连接
- **电源感知** — 监控睡眠/恢复/电池/AC 电源状态及电源设置变更

## 架构

```
service_main.cpp                 # 入口：install | uninstall | debug
  └─ ServiceCenter               # Windows 服务控制器
       ├─ ServicePortal          # JRPC 请求处理门户
       └─ NetDock                # 网络层编排者
            ├─ DockRunLoop       # libevent 事件循环线程
            ├─ MessageServerManager  # WebSocket 服务器 (端口 37310)
            ├─ HubProxyManager       # TAP Hub 路由层（多协议共享）
            └─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
```

### 请求处理流程

```
HTTP JSON-RPC 请求 (端口 39440)
  → ZmJsonRpcServer (Worker 线程)
    → bufferevent pair (跨线程交付)
      → HubProxyManager.OnPairAcceptConn()
        → ZmTapHubProxy (协议探测)
          → ZmTapDelegateJRPC
            → ServicePortal::JrpcRequestReadCB()
              → 业务处理
            ← TAP 回链响应
          ← BackChainPop
        ← bufferevent pair 回传
      ← Worker 线程获取结果
    ← HTTP 200 + JSON 响应
```

## 构建

### 环境要求

- **Visual Studio 2022**（平台工具集 v143）
- **Windows SDK**（Windows 服务开发）
- **同级目录** `..\ZiMoPublic\`（共享依赖库）

### 命令行构建

```bash
# Release 构建（x64）
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64

# Debug 构建（x64）
msbuild ZiMoService.sln /p:Configuration=Debug /p:Platform=x64

# Release 构建（Win32）
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=Win32
```

### 构建输出

| 配置 | 输出目录 |
|------|----------|
| Release | `$(SolutionDir)Release\` |
| Debug | `$(SolutionDir)Debug\` |
| 中间文件 | `temp\` |

### 关键编译配置

| 配置项 | 值 |
|--------|-----|
| C++ 标准 | C++17 (Win32) / C++20 (x64) |
| CRT 链接 | 静态 (`MultiThreaded`) |
| 源码编码 | `/utf-8` |
| 预处理器宏 (Release) | `ZIMO_SERVICE` |
| 强制包含头文件 | `stdafx.h` |
| Release 权限 | 需要管理员权限运行 |

## 服务管理

```bash
# 安装 Windows 服务
ZiMoService.exe install

# 卸载 Windows 服务
ZiMoService.exe uninstall

# 调试模式（前台运行，不注册为 Windows 服务）
ZiMoService.exe debug

# 正常启动（由 SCM 调用）
ZiMoService.exe
```

安装后服务名称为 `ZM_Svc`，可在 `services.msc` 中查看和管理。

## 网络端口

| 服务 | 端口 | 协议 | 说明 |
|------|------|------|------|
| WebSocket | 37310 | TCP | WebSocket 实时通信 |
| HTTP JSON-RPC | 39440 | HTTP | JSON-RPC 2.0，路径 `/ZiMo/ZiMoService` |

## JSON-RPC 协议

服务以 JSON-RPC 2.0 为主要通信协议。请求格式：

```json
{
    "jsonrpc": "2.0",
    "method": "方法名",
    "params": { ... },
    "id": 1
}
```

成功响应：

```json
{
    "jsonrpc": "2.0",
    "result": { ... },
    "id": 1
}
```

错误响应：

```json
{
    "jsonrpc": "2.0",
    "error": {
        "code": 601,
        "message": "错误描述"
    },
    "id": 1
}
```

### 自定义错误码

| 错误码 | 含义 |
|--------|------|
| 601 | SendToHubProxy 失败 |
| 602 | 响应为空 |
| 603 | 响应格式错误 |

## 依赖

项目依赖同级目录 `..\ZiMoPublic\` 中的共享库：

| 模块 | 说明 |
|------|------|
| `net/` | TCP socket、HTTP 客户端/服务端、DNS、TAP 隧道代理 |
| `service/` | `ZmServiceBase` Windows 服务基类 |
| `ssl/` | SSL 上下文管理、指纹校验 |
| `websocket/` | WebSocket 服务端实现 |
| `util/` | 线程 (`ZmThread`)、字符串工具、文件 I/O、libevent 辅助 |
| `json/` | nlohmann/json 封装 (`zm_json`) |
| `spdlog/` | 定制版 spdlog 日志 (`zm_logger`) |
| `libevent/` | 预编译 libevent 头文件及静态库 |
| `openssl/` | 预编译 OpenSSL 头文件及静态库 |

## 项目结构

```
ZiMoService/
├── service_main.cpp              # 程序入口
├── service_center.h/cpp          # Windows 服务控制器（生命周期管理）
├── service_portal.h/cpp          # JRPC 请求处理门户
├── service_define.h              # 服务名称、端口、魔数等宏定义
├── service_global.h              # 全局变量声明
├── net_dock.h/cpp                # 网络层编排者（组件生命周期 + 跨线程调度）
├── dock_runloop.h/cpp            # libevent 事件循环线程
├── message_server_manager.h/cpp  # WebSocket 服务器管理器
├── hub_proxy_manager.h/cpp       # TAP Hub 路由层管理器
├── http_jsonrpc_manager.h/cpp    # HTTP JSON-RPC 前端管理器
├── resource.h                    # Windows 资源定义
├── ZiMoService.sln               # Visual Studio 解决方案
├── ZiMoService.vcxproj           # 项目文件
└── README.md
```

### 核心类职责

| 类 | 职责 |
|----|------|
| `ServiceCenter` | Windows 服务生命周期：启动/停止/会话变更/电源事件/关机 |
| `ServicePortal` | JRPC 请求业务处理入口 |
| `NetDock` | 编排所有网络组件，管理启动/关闭顺序，提供跨线程任务调度 |
| `DockRunLoop` | libevent 事件循环线程，管理 event_base / evdns_base / 心跳定时器 |
| `MessageServerManager` | WebSocket 服务器 (`ZmMessageServer`) 生命周期 |
| `HubProxyManager` | TAP Hub 路由层，管理 TAP 上下文池 / HubProxy / JRPC Delegate |
| `HttpJsonRpcManager` | HTTP JSON-RPC 前端，通过 bufferevent pair 向 Hub 交付请求 |

## 关键设计模式

### 单线程事件循环

所有网络 I/O 操作在 `DockRunLoop` 线程中执行。从其他线程调度任务使用：

```cpp
netDock->ScheduleTaskInLoop(
    [](void* param) {
        // 在 libevent 线程中执行
    },
    [](void* param) {
        // 任务完成后的回调
    },
    param
);
```

### 异步 DNS 解析

使用 libevent 内置 `evdns_getaddrinfo`，在事件循环线程中异步完成，零额外线程：

```
EvDnsResolve(hostname, port)
  → OnDnsResolvedCB
    → delegate->OnTapDnsResolved()
```

### 启动/关闭顺序

启动：`Init → OpenHub → OpenHttpJsonRpcServer`（Hub 必须先于前端启动）

关闭：`前端先停 → Hub 再停 → DockRunLoop 最后停`（逆序保证回调安全释放）

### TAP 代理链

请求通过代理栈路由，每个代理可在上下文链上压入/弹出处理器。响应通过 `ZmTapContext::BackChainPop()` 回传。

## 开发注意事项

- 所有注释和描述使用中文，LF 换行，UTF-8 编码
- 函数/变量/宏/结构体按 `@brief @param @return` 格式添加注释
- 声明与定义分离：头文件只放声明，实现放在对应 `.cpp`
- 成员变量使用 `m_` 前缀（结构体除外），全局变量使用 `g_` 前缀
- 所有异步操作必须发生在 `DockRunLoop` 线程中
- 不要在主线程或 Worker 线程中直接调用 libevent API
- 修改代码后请同步更新 `CLAUDE.md` 中的架构说明
