# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，基于 libevent 事件循环提供 HTTP、WebSocket、JSON-RPC 网络基础设施。

## 功能

- **通用 HTTP 服务** — 端口 80，Express 风格路由中间件 + 线程池，承载管理面板
- **HTTP JSON-RPC** — 端口 39440，路径 `/ZiMo/ZiMoService`，标准化 RPC 调用
- **WebSocket** — 端口 37310，实时双向通信
- **TAP 代理链** — 多协议前端共享 Hub 路由层
- **异步 DNS** — 基于 libevent `evdns_getaddrinfo`，事件驱动
- **系统监控** — CPU/内存/GPU 实时负载采集
- **Windows 服务生命周期** — 安装/卸载/调试，会话和电源事件感知

## 架构

```
service_main.cpp                     # 入口：install | uninstall | debug
  └─ ServiceCenter                   # Windows 服务控制器
       ├─ ServicePortal              # JRPC 入口 + HTTP API 业务层
       └─ NetDock                    # 网络层编排者
            ├─ DockRunLoop           # libevent 事件循环线程
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80)
            │     └─ ZmHttpRouter    # 路由中间件链
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
            ├─ HubProxyManager       # TAP Hub 路由层（多协议共享）
            └─ MessageServerManager  # WebSocket 服务器 (端口 37310)
```

## 网络端口

| 服务 | 端口 | 说明 |
|------|------|------|
| 通用 HTTP | 80 | 管理面板 + REST API + 静态文件 |
| HTTP JSON-RPC | 39440 | JSON-RPC 2.0，路径 `/ZiMo/ZiMoService` |
| WebSocket | 37310 | 实时双向通信 |

## HTTP API

所有接口返回 JSON。

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/service_time` | 服务器当前时间 |
| GET | `/api/service_status` | 综合状态（HTTP/JRPC/Hub/WS/系统负载） |
| GET | `/api/routes` | API 路由文档列表 |
| ANY | `/api/api_test` | 通用接口测试（POST JSON 回显） |
| GET | `/api/about/backend` | 后端 README.md（架构和技术栈） |
| GET | `/api/about/frontend` | 前端 README.md（前端信息） |

## 管理面板

浏览器访问 `http://localhost` — 深色主题 SPA：

- **首页** — 实时时钟 + 服务状态卡片 + CPU/内存/GPU 负载（每秒刷新）
- **文档** — API 接口文档，可折叠展开，支持一键复制和跳转测试
- **接口测试** — URL + 方法选择 + 请求体编辑 + 返回体预览
- **关于** — 从 README.md 动态读取渲染

前端：Vue 3 (CDN) + marked.js + CSS Grid，零构建

## 关键设计

- **线程模型** — libevent 单线程事件循环 + HTTP 请求线程池复用（ZmThreadPool）
- **路由中间件** — Express/Gin 风格，`(task, next)` 管道 + 前缀树匹配（`:id` 参数、`*` 通配符）
- **跨线程操作** — `ResponseAsync` / `SetDropTimerAsync` 自动回投到事件循环线程
- **启动/关闭顺序** — 前端先停 → Hub 再停 → 事件循环最后停

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
