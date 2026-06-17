# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，基于 libevent 事件循环提供 HTTP、消息广播、JSON-RPC 网络基础设施。

## 功能

- **通用 HTTP 服务** — 端口 80，静态文件 + 管理面板着陆页
- **文件中心** — 文件上传/下载/管理，零拷贝传输，密码保护目录，断点续传
- **HTTP JSON-RPC** — 端口 39440，路径 `/ZiMo/JRPC`，所有业务 API 统一入口
- **消息广播** — 端口 39640，TCP 一对多推送，基于 ZmBroadcastServer/ZmBroadcastClient
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
            ├─ HubProxyManager       # TAP Hub 路由层（★ 内部持有 EvBaseRunLoop）
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80)
            │     ├─ ZmHttpRouter        # 路由中间件链
            │     └─ HttpServerModuleFileHub  # 文件中心模块（内部持有，自注册路由）
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
            └─ BroadcastManager      # 消息广播服务端 (端口 39640)
```

## 线程模型

系统包含 **四条独立的事件循环线程** 和 **三个线程池**：

| 线程/池 | 所属组件 | 说明 |
|---------|---------|------|
| **Hub 事件循环** | `HubProxyManager` → `ZmEvBaseRunLoop` | 处理 TAP 代理链 + 内部 JRPC 通道 |
| **HTTP 80 事件循环** | `HttpServerManager` → `ZmEvBaseRunLoop` | 仅处理 HTTP 80 请求接收与响应发送 |
| **HTTP 39440 事件循环** | `HttpJsonRpcManager` → `ZmEvBaseRunLoop` | 仅处理 JRPC HTTP 请求接收与响应发送 |
| **广播事件循环** | `BroadcastManager` → `ZmEvBaseRunLoop` | 处理广播客户端连接和消息推送 |
| **HTTP 线程池** | `ZmHttpServer::m_pool` | 每个 HTTP 服务器独立的线程池，执行请求处理 |
| **JRPC delegate 线程池** | `ZmThreadPool::InvokeLater` | 执行 `ServicePortal::JrpcRequestReadCB` 业务逻辑 |

## 网络端口

| 服务 | 端口 | 说明 |
|------|------|------|
| 通用 HTTP | 80 | 着陆页 + 控制中心 + 文件中心 + 静态文件 |
| HTTP JSON-RPC | 39440 | JSON-RPC 2.0 业务 API，路径 `/ZiMo/JRPC` |
| 消息广播 | 39640 | TCP 一对多消息推送 |

## 前端页面

浏览器访问 `http://localhost`（白名单模式，仅注册路由可访问）：

| 路径 | 说明 |
|------|------|
| `/` | 着陆页 — 进入控制中心 |
| `/control` | 控制中心 SPA — 首页/文档/接口测试/关于 |
| `/filehub` | 文件中心 — 文件管理/上传/下载 |
| `/file_hub/download/*` | 文件下载（零拷贝，Range 断点续传） |
| `/file_hub/upload/*` | 文件上传（mmap 零拷贝） |

```
www/
├── html/          ← /html/* HTTP 可访问
├── css/           ← /css/*  HTTP 可访问
├── js/            ← /js/*   HTTP 可访问
├── doc/           ← 不可通过 HTTP 访问（JRPC getAbout 内部读取）
└── db/filehub/    ← 不可通过 HTTP 直接访问（仅通过 /file_hub/* 路由）
```

前端：Vue 3 (CDN) + 纯 CSS，零构建。控制中心和文件中心通过 JRPC（端口 39440）获取数据，上传/下载走 HTTP 80 端口。

## HTTP 80 路由架构

```
HttpServerManager::SetupRouter()           # 基础路由（静态文件）
  ├── /html/* → ServeStaticFile           # 零拷贝发送
  ├── /css/*  → ServeStaticFile
  └── /js/*   → ServeStaticFile

ServicePortal::RegisterHttpRoutes()        # 页面入口路由
  ├── GET /          → index.html
  ├── GET /control   → control.html
  └── GET /filehub   → file_hub.html

HttpServerModuleFileHub::RegisterHttpRoutes()  # 文件中心业务路由
  ├── ANY /file_hub/download/* → SendFile    # 通用下载（Range 支持）
  └── POST /file_hub/upload/*  → ReceiveFile # 通用上传（mmap 零拷贝）

HttpServerManager 暴露通用能力：
  SendFile(task, physicalPath)              # 零拷贝下载
  ReceiveFile(task, physicalPath, data)     # mmap 上传
```

## JRPC 方法

所有业务 API 统一通过 JSON-RPC 2.0（端口 39440，路径 `/ZiMo/JRPC`）访问。

### 系统

| 方法 | 说明 |
|------|------|
| `ping` | 心跳检测，返回 pong |
| `getTime` | 服务器当前时间 |
| `getStatus` | 综合状态（HTTP/JRPC/Hub/Broadcast/系统负载） |
| `echo` | 通用接口测试，回显传入数据 |

### 广播

| 方法 | 说明 |
|------|------|
| `broadcast` | 向匹配 tag 的客户端推送消息 |

### 文档

| 方法 | 说明 |
|------|------|
| `getRoutes` | JRPC 方法文档列表 |
| `getAbout` | 后端和前端技术信息 |

### 文件中心

| 方法 | 说明 |
|------|------|
| `listFiles` | 列出目录下的文件和文件夹（一层，按名称排序，区分加密） |
| `searchFiles` | 递归模糊搜索文件/文件夹名 |
| `createDir` | 新建目录（可选设用户名/密码，HMAC-SHA256 哈希存储） |
| `deleteItem` | 删除文件或目录（有密码的目录需验证用户名和密码） |
| `verifyDirPassword` | 验证目录密码 |
| `changeDirPassword` | 修改目录密码（需旧密码验证） |
| `batchDelete` | 批量删除文件 |

## HTTP 80 请求处理

端口 80 为同步模式，请求在单次 Worker 线程中完成：

```
[HTTP 80 事件循环] evhttp → OnHttp_RequestCB
  └─ m_pool->Submit(doer)
     ↓
[HTTP 线程池 Worker] Process → Perform
  └─ OnHttpdRequest → m_router.Serve
     ├── 中间件: ZmHttpMiddlewareLogging / ZmHttpMiddlewareRecovery
     ├── 路由匹配 → handler
     ├── 文件服务: ServeStaticFile / SendFile（零拷贝 evbuffer_file_segment）
     └── 文件上传: ReceiveFile（CreateFileMapping + MapViewOfFile）
  └─ event_active(REPLY)
     ↓
[HTTP 80 事件循环] OnEvent_Control → SendReply → evhttp_send_reply
  └─ 1s 定时器 → SendReplyEnd → delete this
```

## 关键设计

- **零拷贝传输** — 下载使用 `evbuffer_file_segment`（mmap），上传使用 `CreateFileMapping + MapViewOfFile`
- **断点续传** — 下载支持 HTTP Range 请求（206 Partial Content）
- **密码安全** — HMAC-SHA256 哈希存储，前端 `lang="en"` + `ime-mode:disabled` 防止中文输入
- **中文路径** — 全链路 Wide API（`CreateFileW`/`FindFirstFileW`），`ZmString::UTF8_To_Unicode`/`Unicode_To_UTF8` 转换
- **线程模型** — 四条独立事件循环 + 三个线程池，跨线程通过 `event_active` + SPSC 队列通信
- **路由中间件** — Express 风格，`(task, next)` 管道 + 前缀树匹配
- **架构分离** — 通用层（HttpServerManager）与业务层（HttpServerModuleFileHub）分离，文件中心自注册路由

## 构建

```bash
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64
```

输出到 `$(SolutionDir)$(Configuration)\`，中间文件到 `temp\`。需要 VS 2022 + Windows SDK + 同级目录 `..\ZiMoPublic\`。

libevent 使用静态库 + `/MT` 链接方式（`EVENT__LIBRARY_TYPE=STATIC` + `EVENT__MSVC_STATIC_RUNTIME=ON`）。

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
| `net/broadcast` | 消息广播服务端与客户端 |
| `util/` | 线程、线程池、系统监控、字符串工具、文件工具 |
| `json/` | nlohmann/json 封装 |
| `spdlog/` | 日志 |
| `libevent/` | 自编译 libevent（静态库 /MT） |
| `openssl/` | 预编译 OpenSSL |

## 提交规范

```
feat: 新功能 / fix: 修复 / docs: 文档 / style: 代码风格
refactor: 重构 / perf: 性能优化 / test: 测试
chore: 杂项 / build: 构建 / ci: CI / revert: 回滚
```
