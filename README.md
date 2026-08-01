# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，基于 libevent 事件循环提供 HTTP、RESTful API、JSON-RPC、SSE 推送、消息广播网络基础设施。

## 功能

- **通用 HTTP 服务** — 端口 80，静态文件 + 管理面板着陆页
- **RESTful API 服务** — 端口 39441，路径 `/zimo/api`，所有业务 API 统一入口（★ 已从 JRPC 迁移）
- **HTTP JSON-RPC** — 端口 39440，路径 `/zimo/jrpc`，兼容旧版 JRPC 调用
- **文件中心** — 文件上传/下载/管理，零拷贝传输，密码保护目录，断点续传
- **消息广播** — 端口 39640，TCP 一对多推送，基于 ZmBroadcastServer/ZmBroadcastClient
- **SSE 推送** — `GET /zimo/api/events`，服务端实时事件流推送
- **远程音频** — WASAPI loopback 采集系统声音 + Opus 编码,RESTful 流式推送,手机网页实时收听
- **TAP 代理链** — 多协议前端共享 Hub 路由层（JRPC + RESTful 双协议委托）
- **异步 DNS** — 基于 libevent `evdns_getaddrinfo`，事件驱动
- **系统监控** — CPU/内存/GPU 实时负载采集
- **Windows 服务生命周期** — 安装/卸载/调试，会话和电源事件感知

## 架构

```
service_main.cpp                     # 入口：install | uninstall | debug
  └─ ServiceCenter                   # Windows 服务控制器
       ├─ ServicePortal              # 业务层（JRPC + RESTful 双回调入口）
       │    ├─ FileHubModule            # 文件中心（业务逻辑抽离，双协议共享，自建自管）
       │    └─ ServerAudioStreamModule  # 远程音频（WASAPI 采集 + Opus 编码 + 订阅分发）
       └─ NetDock                    # 网络层编排者
            ├─ HubProxyManager       # TAP Hub 路由层（内部持有 Hub/JRPC/RESTful 三条事件循环）
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80)
            │     └─ ZmHttpRouter        # 路由中间件链（Express 风格）
            ├─ HttpRestfulManager   # ★ HTTP RESTful 前端 (端口 39441)
            │     └─ ZmRESTfulServer + bufferevent_pair 池 → Hub → ZmTapDelegateRESTful
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
            │     └─ ZmJsonRpcServer + bufferevent_pair 池 → Hub → ZmTapDelegateJRPC
            └─ BroadcastManager      # 消息广播服务端 (端口 39640)
```

**RESTful 请求路由（★ 新架构）**：
```
HTTP RESTful 请求 → ZmRESTfulServer (Worker 线程)
  → HttpRestfulManager::OnRESTfulCBAsync → 打包 "REST" 帧
  → bufferevent_pair[1] → Hub 协议探测
  → ZmTapDelegateRESTful 解帧 → 线程池分发
  → ServicePortal::RestfulRequestCB (业务处理)
  → tap->httpd_task->TriggerReply() → HTTP 响应（★ 直通模式，不绕 pair）
```

**JRPC 请求路由（原架构，保留兼容）**：
```
HTTP JRPC 请求 → ZmJsonRpcServer (Worker 线程)
  → InjectJrpcRequest → bufferevent_pair[1] → Hub 协议探测
  → ZmTapDelegateJRPC 解帧 → 线程池分发
  → ServicePortal::JrpcRequestReadCB (业务处理)
  → ZmTapContext::Response() → pair[1] 回写
  → pair[0] 回调 → reply() → HTTP 响应
```

## 线程模型

系统包含 **六条独立的事件循环线程** 和 **四个线程池**：

| 线程/池 | 所属组件 | 说明 |
|---------|---------|------|
| **Hub 事件循环** | `HubProxyManager` → `m_evLoopHub` | 处理 TAP 代理链共享路由 |
| **JRPC 事件循环** | `HubProxyManager` → `m_evLoopJRPC` | JRPC delegate 专用事件循环 |
| **RESTful 事件循环** | `HubProxyManager` → `m_evLoopRESTful` | RESTful delegate 专用事件循环 |
| **HTTP 80 事件循环** | `HttpServerManager` → `ZmEvBaseRunLoop` | 仅处理 HTTP 80 请求接收与响应发送 |
| **HTTP 39440 事件循环** | `HttpJsonRpcManager` → `ZmEvBaseRunLoop` | 仅处理 JRPC HTTP 请求接收与响应发送 |
| **HTTP 39441 事件循环** | `HttpRestfulManager` → `m_evLoopHttpServer` | 仅处理 RESTful HTTP 请求接收与响应发送 |
| **HTTP 线程池** | `ZmHttpServer::m_pool` | 每个 HTTP 服务器独立的线程池，执行请求处理 |
| **JRPC delegate 线程池** | `ZmTapDelegateJRPC::m_threadPool` | 执行业务回调 |
| **RESTful delegate 线程池** | `ZmTapDelegateRESTful::m_threadPool` | 执行业务回调 |
| **Pair 池事件循环** | `HttpJsonRpcManager/HttpRestfulManager::m_evLoopPairPool` | bufferevent_pair 池专用事件循环 |

## 网络端口

| 服务 | 端口 | 说明 |
|------|------|------|
| 通用 HTTP | 80 | 着陆页 + 控制中心 + 文件中心 + 静态文件 |
| HTTP RESTful | 39441 | ★ RESTful API 业务入口，路径 `/zimo/api` |
| HTTP JSON-RPC | 39440 | JSON-RPC 2.0 业务 API，路径 `/zimo/jrpc`（兼容保留） |
| 消息广播 | 39640 | TCP 一对多消息推送 |

## 前端页面

浏览器访问 `http://localhost`（白名单模式，仅注册路由可访问）：

| 路径 | 说明 |
|------|------|
| `/` | 着陆页 — 进入控制中心 |
| `/control` | 控制中心 SPA — 首页/文档/接口测试/关于 |
| `/filehub` | 文件中心 — 文件管理/上传/下载 |
| `/audio` | 远程音频 — 实时收听服务器声音(WebCodecs 播放) |
| `/404` | 自定义 404 页面 |
| `*` | 兜底路由 — 文件不存在则展示 404 页面 |

```
www/
├── html/          ← /html/* HTTP 可访问
├── css/           ← /css/*  HTTP 可访问
├── js/            ← /js/*   HTTP 可访问
├── doc/           ← 不可通过 HTTP 访问（getAbout 内部读取）
└── db/filehub/    ← 不可通过 HTTP 直接访问（仅通过 API 路由）
```

前端：Vue 3 (CDN) + 纯 CSS，零构建。控制中心和文件中心通过 RESTful API（端口 39441）获取数据，上传/下载走 HTTP 80 端口。

## HTTP 80 路由架构

```
HttpServerManager::SetupRouter()           # 基础路由（静态文件）
  ├── /html/* → ServeStaticFile           # 零拷贝发送
  ├── /css/*  → ServeStaticFile
  └── /js/*   → ServeStaticFile

ServicePortal::RegisterHttpRoutes()        # 页面入口路由
  ├── GET /          → index.html
  ├── GET /control   → control.html
  ├── GET /filehub   → filehub.html
  ├── GET /404       → 404.html           # ★ 自定义 404 页面
  └── ANY *          → ServeStaticFile    # ★ 兜底路由（文件不存在展示 404）


HttpServerManager 暴露通用能力：
  SendFile(task, physicalPath)              # 零拷贝下载
  ReceiveFile(task, physicalPath, data)     # mmap 上传
```

## RESTful API 方法

所有业务 API 统一通过 RESTful（端口 39441，根路径 `/zimo/api`）访问。

### 系统

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/ping` | 心跳检测，返回 `{"pong": true}` |
| GET | `/time` | 服务器当前时间与时间戳 |
| GET | `/status` | 综合状态（HTTP/JRPC/RESTful/Hub/Broadcast/系统负载） |
| POST | `/echo` | 通用接口测试，回显 Query 参数 |

### 广播

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/broadcast` | 向匹配 tag 的客户端推送消息 |

### 推送

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/events` | SSE 服务端事件推送（text/event-stream） |

### 音频

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/audio/stream` | 远程音频流(服务器系统声音,二进制帧: len(4B)+seq(4B)+Opus 20ms 帧) |

### 文档

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/routes` | RESTful API 方法文档列表 |
| GET | `/about` | 后端和前端技术信息 |

### 文件中心

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/files` | 列出目录下的文件和文件夹（一层，按名称排序，区分加密） |
| GET | `/files/search` | 递归模糊搜索文件/文件夹名 |
| POST | `/files/dirs` | 新建目录（可选设用户名/密码，HMAC-SHA256 哈希存储） |
| DELETE | `/files` | 删除文件或目录（有密码的目录需验证用户名和密码） |
| GET/POST | `/files/verify-password` | 验证目录密码 |
| PUT | `/files/password` | 修改目录密码（需旧密码验证） |
| POST | `/files/batch-delete` | 批量删除文件 |
| POST | `/files/upload` | 上传文件（body 为二进制内容） |
| GET | `/files/download` | 下载文件（支持 Range 断点续传，Content-Disposition: attachment） |

### 调用示例

```bash
# 心跳
curl http://localhost:39441/zimo/api/ping

# 列出文件
curl "http://localhost:39441/zimo/api/files?path="

# 创建密码保护的目录
curl -X POST "http://localhost:39441/zimo/api/files/dirs?path=&dirName=Private&username=admin&password=secret"

# 上传文件
curl -X POST --data-binary @file.zip "http://localhost:39441/zimo/api/files/upload?path=file.zip"

# 下载文件
curl "http://localhost:39441/zimo/api/files/download?path=file.zip" -o file.zip

# SSE 事件流
curl "http://localhost:39441/zimo/api/events"
```

## JRPC 方法（兼容保留）

以下 JRPC 方法仍在端口 39440（路径 `/zimo/jrpc`）上可用，推荐新业务迁移到 RESTful API。

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
```

## RESTful 请求处理（★ 新）

端口 39441 为异步模式，请求注入 Hub 代理链后业务层直通响应：

```
[RESTful 事件循环] evhttp → OnRESTfulCBAsync
  └─ 打包帧: "REST" + body_len + raw_body
  └─ bufferevent_pair[1] 注入 Hub → 协议探测
     ↓
[ZmTapDelegateRESTful] OnTapRequesterRead
  └─ 解帧 → m_threadPool 分发
     ↓
[RESTful delegate 线程池] ServicePortal::RestfulRequestCB
  ├── 路由匹配: verb + path
  ├── 业务处理（文件中心/广播/系统等）
  └── tap->httpd_task->TriggerReply() → ★ 直通 HTTP 响应
```

与 JRPC 的关键区别：RESTful 响应不通过 pair 回传，业务层直接操作 `tap->httpd_task` 写响应，避免了 pair 往返，路径更短。

## 关键设计

- **双协议共存** — RESTful (端口 39441) 与 JRPC (端口 39440) 各自独立前端 + delegate，共享文件中心业务模块
- **响应直通** — RESTful 业务层直写 HTTP 响应，无 pair 回传；JRPC 通过 pair 回写兼容旧流程
- **零拷贝传输** — 下载使用 `evbuffer_file_segment`（mmap），上传使用 `CreateFileMapping + MapViewOfFile`
- **断点续传** — 下载支持 HTTP Range 请求（206 Partial Content）
- **SSE 推送** — 支持 `text/event-stream` 流式响应，业务层可自定义事件源
- **流式输出** — HTTP 服务器支持 `StartStreamReply`/`SendReplyChunk`/`EndStreamReply` 分块响应
- **请求头传递** — 公共库支持将 HTTP 请求头投入业务侧，业务侧可回复自定义响应头
- **密码安全** — HMAC-SHA256 哈希存储，前端 `lang="en"` + `ime-mode:disabled` 防止中文输入
- **中文路径** — 全链路 Wide API（`CreateFileW`/`FindFirstFileW`），`ZmString::UTF8_To_Unicode`/`Unicode_To_UTF8` 转换
- **线程模型** — 六条独立事件循环 + 四个线程池，跨线程通过 `event_active` + SPSC 队列通信
- **路由中间件** — Express 风格，`(task, next)` 管道 + 前缀树匹配
- **架构分离** — 通用层（HttpServerManager）与业务层（FileHubModule）分离，文件中心通过双协议暴露
- **Pair 对象池** — bufferevent_pair 复用减少高并发下的内存分配和系统调用
- **Doer 池化** — ZmHttpdDoer 对象池化，即时释放减少内存占用

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
| `net/` | TCP、HTTP、RESTful 服务器、DNS、TAP 代理（JRPC + RESTful 双协议委托）、路由中间件 |
| `service/` | ZmServiceBase |
| `ssl/` | SSL 上下文管理 |
| `libopus/` | Opus 编码器（静态库 /MT，远程音频编码） |
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
