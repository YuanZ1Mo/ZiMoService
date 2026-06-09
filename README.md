# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，基于 libevent 事件循环提供 HTTP、WebSocket、JSON-RPC 网络基础设施。

## 功能

- **通用 HTTP 服务** — 端口 80，静态文件 + 管理面板着陆页
- **HTTP JSON-RPC** — 端口 39440，路径 `/ZiMo/JRPC`，所有业务 API 统一入口
- **WebSocket** — 端口 37310，实时双向通信
- **TAP 代理链** — 多协议前端共享 Hub 路由层
- **异步 DNS** — 基于 libevent `evdns_getaddrinfo`，事件驱动
- **系统监控** — CPU/内存/GPU 实时负载采集
- **Windows 服务生命周期** — 安装/卸载/调试，会话和电源事件感知

## 架构

```
service_main.cpp                     # 入口：install | uninstall | debug
  └─ ServiceCenter                   # Windows 服务控制器
       ├─ ServicePortal              # JRPC 业务层（所有 API 统一通过 JRPC 暴露）
       └─ NetDock                    # 网络层编排者
            ├─ DockRunLoop           # libevent 事件循环线程
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80，仅静态文件)
            │     └─ ZmHttpRouter    # 路由中间件链
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
            ├─ HubProxyManager       # TAP Hub 路由层（多协议共享）
            └─ MessageServerManager  # WebSocket 服务器 (端口 37310)
```

## 网络端口

| 服务 | 端口 | 说明 |
|------|------|------|
| 通用 HTTP | 80 | 着陆页 + 控制中心 + 静态文件 |
| HTTP JSON-RPC | 39440 | JSON-RPC 2.0 业务 API，路径 `/ZiMo/JRPC` |
| WebSocket | 37310 | 实时双向通信 |

## 前端页面

浏览器访问 `http://localhost`（按目录放行：html/css/js，其余目录不可访问）：

| 路径 | 说明 |
|------|------|
| `/` | 着陆页 — 进入控制中心入口 |
| `/control` | 控制中心 SPA — 首页/文档/接口测试/关于 |

```
www/
├── html/          ← /html/* HTTP 可访问
├── css/           ← /css/*  HTTP 可访问
├── js/            ← /js/*   HTTP 可访问
└── doc/           ← 不可通过 HTTP 访问（JRPC getAbout 内部读取）
```

前端：Vue 3 (CDN) + marked.js + CSS Grid，零构建。控制中心通过 JRPC（端口 39440）获取数据。

## JRPC 方法

所有业务 API 统一通过 JSON-RPC 2.0（端口 39440，路径 `/ZiMo/JRPC`）访问。

| 方法 | 分类 | 说明 |
|------|------|------|
| `ping` | 系统 | 心跳检测，返回 pong |
| `getTime` | 系统 | 服务器当前时间 |
| `getStatus` | 系统 | 综合状态（HTTP/JRPC/Hub/WS/系统负载/时间） |
| `echo` | 测试 | 通用接口测试，回显传入数据 |
| `getRoutes` | 文档 | JRPC 方法文档列表 |
| `getAbout` | 文档 | 后端和前端技术信息（README.md） |

请求示例：
```json
POST http://localhost:39440/ZiMo/JRPC
Content-Type: application/json

{"method":"getStatus","params":{}}
```

## 关键设计

- **线程模型** — libevent 单线程事件循环 + HTTP 请求线程池复用（ZmThreadPool）
- **路由中间件** — Express/Gin 风格，`(task, next)` 管道 + 前缀树匹配（`:id` 参数、`*` 通配符）
- **跨线程操作** — `ResponseAsync` / `SetDropTimerAsync` 自动回投到事件循环线程
- **启动/关闭顺序** — 前端先停 → Hub 再停 → 事件循环最后停
- **业务与静态分离** — 所有业务 API 通过 JRPC 端口（39440）暴露，HTTP 端口（80）仅提供静态文件服务

## 构建

```bash
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64
```

输出到 `$(SolutionDir)$(Configuration)\`，中间文件到 `temp\`。需要 VS 2022 + Windows SDK + 同级目录 `..\ZiMoPublic\`。

## 服务管理

```bash
ZiMoService.exe install     # 安装 Windows 服务
ZiMoService.exe uninstall   # 卸载
ZiMoService.exe debug       # 前台调试运行
```

## 依赖（ZiMoPublic）

| 模块 | 说明 |
|------|------|
| `net/` | TCP、HTTP、DNS、TAP 代理、路由中间件 |
| `service/` | ZmServiceBase |
| `ssl/` | SSL 上下文管理 |
| `websocket/` | WebSocket 服务端 |
| `util/` | 线程、线程池、系统监控、字符串工具 |
| `json/` | nlohmann/json 封装 |
| `spdlog/` | 日志 |
| `libevent/` | 预编译 libevent |
| `openssl/` | 预编译 OpenSSL |
