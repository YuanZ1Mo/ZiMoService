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
            ├─ HubProxyManager       # TAP Hub 路由层（★ 内部持有 EvBaseRunLoop）
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80，仅静态文件)
            │     └─ ZmHttpRouter    # 路由中间件链
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440)
            └─ MessageServerManager  # WebSocket 服务器 (端口 37310)
```

## 线程模型

系统包含 **三条独立的事件循环线程** 和 **两个线程池**：

| 线程/池 | 所属组件 | 说明 |
|---------|---------|------|
| **Hub 事件循环** | `HubProxyManager` → `ZmEvBaseRunLoop` | `event_base_loop(EVLOOP_NO_EXIT_ON_EMPTY)`，处理所有 TAP 代理链操作 |
| **HTTP 80 事件循环** | `HttpServerManager` → `ZmHttpServer` | `event_base_dispatch()`，接收 HTTP 请求并分发响应 |
| **HTTP 39440 事件循环** | `HttpJsonRpcManager` → `ZmJsonRpcServer` | `event_base_dispatch()`，接收 JRPC HTTP 请求并分发响应 |
| **HTTP 线程池** | `ZmHttpServer::m_pool` | 每个 HTTP 服务器独立的线程池（`hardware_concurrency` 线程），执行请求处理 |
| **JRPC delegate 线程池** | `ZmThreadPool::InvokeLater` | 全局线程池，执行 `ServicePortal::JrpcRequestReadCB` 业务逻辑 |

跨线程通信机制：
- **Worker → Hub 事件循环**：`ZmNetRequestChannel`（SPSC 队列 + `event_active` 唤醒）
- **Hub 事件循环 → JRPC HTTP 事件循环**：`SendDeferredReply()` → `event_active(m_reply_event)`（`event_active` 线程安全，可跨 event_base）
- **任意线程 → Hub 事件循环**：`ZmTapContext::ScheduleInLoop(evbase, fn)`（通过临时 event + `event_active` 投递）
- **任意线程 → TAP 操作**：`ZmTapContext::Response` / `SetDropTimer` / `Drop`（内部均通过 `ScheduleInLoop` 回投）

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

---

## JRPC 请求处理流程（端口 39440）

一个完整的异步 JRPC 请求经过 **3 条线程、7 个阶段、19 个步骤**，涉及两条独立的事件循环（JRPC HTTP 事件循环 ↔ Hub 事件循环）。

### 阶段 1：HTTP 接收 → Worker 提交

```
线程: [JRPC HTTP 事件循环]
  ① OnHttp_RequestCB()                              ← evhttp 接收到 TCP 连接
     └─ new ZmHttpdDoer(server, request)             ← 创建请求上下文 + m_reply_event
     └─ m_pool->Submit(doer)                         ← 提交到 HTTP 线程池
        ↓
线程: [HTTP 线程池 Worker]
  ② ZmHttpdDoer::Process()
     └─ ZmHttpServer::Perform(task)                  ← 添加 CORS 头、处理 OPTIONS
        └─ ZmJsonRpcServer::OnHttpdRequest()         ← URI 匹配 /ZiMo/JRPC，进入 JRPC 路径
           ├─ 解析 JSON 请求体，提取 method / params / id
           ├─ task->DeferReply()                     ← ★ 阻止自动响应（m_deferred = true）
           └─ m_on_jsonrpc_async(task, method, params, reply)
              = HttpJsonRpcManager::OnJsonRpcAsync()
                 ├─ 构建请求 JSON（method + ip + port + useragent + params）
                 └─ m_hub_channel->SubmitAsync(reqjs, callback)
```

**线程状态**：Worker 线程在 `SubmitAsync` 返回后立即释放回池，**不阻塞等待**。

### 阶段 2：跨线程传输（SPSC 队列）

```
线程: [HTTP 线程池 Worker] → 立即返回
  ③ ZmNetRequestChannel::SubmitAsync()
     ├─ 创建 ZmNetRequestItem（含 direct_callback）
     ├─ 入队 SPSC 队列（m_mutex 保护）
     └─ event_active(m_notifyEvent)                  ← 唤醒 Hub 事件循环
```

### 阶段 3：Hub 注入 + 协议探测

```
线程: [Hub 事件循环 (EvBaseRunLoop)]
  ④ ZmNetRequestChannel::OnNotifyEvent()
     └─ Drain()                                      ← 原子交换队列，批量取出
        └─ m_handler(requestJson, responseCallback)
           = HttpJsonRpcManager::InjectJrpcRequest()
              ├─ 从 BuffereventPairPool 获取/创建 bufferevent_pair
              ├─ 写入 pair[0]："JRPC" 魔数(4B) + 大端长度(4B) + JSON 体
              └─ OnPairAcceptBev(HubProxy, pair[1], slot, ReleaseHalf)
                 └─ ZmTapContextEventHandler::OnPairAcceptBev()
                    ├─ context->Get()                ← 从 TAP 池获取 ZM_TAP_CTX
                    ├─ 设置 TAP 字段（IP=127.0.0.1，delegate=HubProxy）
                    └─ HubProxy->OnTapRequesterAccept(tap, -1, addr)
                       = ZmTapHubProxy::OnTapRequesterAccept()
                          └─ 设置 4 字节读水位线 + 协议探测回调
                             (OnProtocolDetectReadCB / OnProtocolDetectEventCB)

  ⑤ 数据到达 pair[1] → 触发探测回调
     ZmTapHubProxy::OnProtocolDetectReadCB()
     ├─ 读取 4 字节魔数 "JRPC"
     ├─ SwitchDelegate(tap, m_delegate_jrpc)         ← 切换到 JRPC delegate
     │   └─ 替换 bufferevent 回调为标准 OnRequesterReadCB / OnRequesterEventCB
     └─ tap->delegate->OnTapRequesterRead(tap, input, remaining)
        = ZmTapDelegateJRPC::OnTapRequesterRead()
```

### 阶段 4：JRPC 解析 → 业务层分发

```
线程: [Hub 事件循环]
  ⑥ ZmTapDelegateJRPC::OnTapRequesterRead()
     ├─ 解析 4 字节大端长度头 → 获取消息体长度
     ├─ evbuffer_copyout 增量读取 JSON 体（分片到达时多次累积）
     ├─ ZmTapContext::BackChainPush(tap, this)       ← JRPC delegate 压入回传链
     └─ ZmThreadPool::InvokeLater([tap, reqCopy, cb]() {
            cb(tap, reqCopy.c_str());                ← 投递到 JRPC delegate 线程池
        })
        ↓
线程: [JRPC delegate 线程池 Worker]
  ⑦ ServicePortal::JrpcRequestReadCB(tap, reqData)
     ├─ zm_json_parse() 解析请求 JSON
     ├─ 提取 method → 分发到对应处理器
     │   ├─ ping       → result["pong"] = true
     │   ├─ getTime    → 获取当前时间
     │   ├─ getStatus  → 采集各组件状态 + 系统负载
     │   ├─ echo       → 回显 params
     │   ├─ getRoutes  → 返回方法文档列表
     │   ├─ getAbout   → 读取 README.md
     │   └─ 未知方法   → error = {-32601, "Method not found"}
     ├─ 构建响应 JSON（result 或 error）
     └─ ZmTapContext::Response(tap, rspJson)
```

### 阶段 5：响应回传（跨线程调度）

```
线程: [JRPC delegate 线程池 Worker]
  ⑧ ZmTapContext::Response(tap, jsResponse)
     └─ ScheduleInLoop(tap, [tap, rspJson]() { ... })
        ├─ 创建临时 event 绑定到 Hub event_base
        └─ event_active() 唤醒 Hub 事件循环
           ↓
线程: [Hub 事件循环]
  ⑨ ZmTapContext::ResponseImpl(tap, jsResponse)
     ├─ BackChainPop(tap) → ZmTapDelegateJRPC       ← 从回传链弹出 JRPC delegate
     ├─ SetOnBackData(tap, json_str)                ← 设置回传数据
     └─ back_delegate->OnTapDelegateBackEvent(tap)
        = ZmTapDelegateJRPC::OnTapDelegateBackEvent()
           └─ WriteResponse(tap, json_str, data_len)
              ├─ 构造 4 字节大端长度头 + JSON 响应体
              ├─ evbuffer_add_iovec → pair[1] 输出缓冲区（单次提交双 iovec）
              └─ bufferevent_flush(BEV_FLUSH)        ← 强制刷出

  ⑩ 数据流经 bufferevent_pair → pair[0] 端触发读回调
     HttpJsonRpcManager::OnResponseRead(bev, rctx)
     ├─ 读取 4 字节长度头 → 获取响应长度
     ├─ 读取完整 JSON 响应体
     ├─ rctx->callback(responseJson)                ← SubmitAsync 的回调链
     │   └─ 解析响应 JSON → reply(result, error)
     │       = ZmJsonRpcServer::OnHttpdRequest 中的 reply lambda
     │          ├─ 构建 JSON-RPC 2.0 响应信封（jsonrpc + id + result/error）
     │          ├─ task->SetReply(200)
     │          ├─ task->PutReplyHeader("Content-type", "application/json; charset=utf-8")
     │          ├─ task->SetReplyData(body)
     │          └─ task->SendDeferredReply()         ← ★ 跨 event_base 唤醒
     │             └─ event_active(m_reply_event, ZM_HTTPD_CONTROL_REPLY, 0)
     └─ 归还 pair[0] 到对象池（或直接 free）
```

### 阶段 6：HTTP 响应发送

```
线程: [JRPC HTTP 事件循环]  ← 被 event_active 唤醒
  ⑪ ZmHttpServer::OnEvent_Control(fd, ZM_HTTPD_CONTROL_REPLY, ctx)
     └─ doer->SendReply()
        ├─ 将 m_reply_headers 写入 evhttp_request 输出头
        └─ evhttp_send_reply(m_request, 200, body)   ← 发送 HTTP 响应
        └─ 创建 1 秒一次性定时器（m_remove_event）

  ⑫ (1 秒后) ZmHttpServer::OnEvent_Timer()
     └─ doer->SendReplyEnd()
        └─ delete this                               ← 释放 ZmHttpdDoer 资源
```

### 阶段 7：连接清理

```
线程: [Hub 事件循环]
  ⑬ 30 秒超时或客户端关闭 → OnRequesterEventCB()
     └─ tap->Drop("reason")                          ← 回收 TAP 到池
        └─ FreeRequesterEnd(tap)                     ← 归还 pair[1] 到对象池
```

### JRPC 完整调用链速查

```
浏览器 / 客户端
  │  POST http://localhost:39440/ZiMo/JRPC
  ▼
[JRPC HTTP 事件循环] evhttp → OnHttp_RequestCB
  │  ZmHttpdDoer → m_pool->Submit
  ▼
[HTTP 线程池 Worker] Process → Perform → OnHttpdRequest (ZmJsonRpcServer)
  │  DeferReply + m_on_jsonrpc_async → OnJsonRpcAsync
  │  SubmitAsync → SPSC 队列 → event_active → 立即返回
  ▼
[Hub 事件循环] Drain → InjectJrpcRequest
  │  bufferevent_pair → "JRPC" 魔数 → OnPairAcceptBev
  │  OnTapRequesterAccept → 协议探测 → SwitchDelegate
  │  OnTapRequesterRead (ZmTapDelegateJRPC)
  │  长度前缀解析 → InvokeLater
  ▼
[JRPC delegate 线程池] JrpcRequestReadCB (ServicePortal)
  │  业务逻辑分发 → ZmTapContext::Response
  │  ScheduleInLoop → event_active
  ▼
[Hub 事件循环] ResponseImpl → BackChainPop → OnTapDelegateBackEvent
  │  WriteResponse → pair[1] → pair[0] → OnResponseRead
  │  callback → reply(result, error) → SendDeferredReply
  │  event_active(m_reply_event) → 跨 event_base 唤醒
  ▼
[JRPC HTTP 事件循环] OnEvent_Control → SendReply → evhttp_send_reply
  │  1s 定时器 → SendReplyEnd → delete this
  ▼
浏览器 / 客户端
```

---

## HTTP 请求处理流程（端口 80）

端口 80 的 HTTP 请求为**同步模式**（相对 JRPC 异步链路而言），请求处理全部在单次 Worker 线程调度中完成，不涉及 Hub 事件循环。

### 完整流程

```
线程: [HTTP 80 事件循环]
  ① OnHttp_RequestCB()                              ← evhttp 接收到 TCP 连接
     └─ new ZmHttpdDoer(server, request)             ← 创建请求上下文 + m_reply_event
     └─ m_pool->Submit(doer)                         ← 提交到 HTTP 线程池
        ↓
线程: [HTTP 线程池 Worker]
  ② ZmHttpdDoer::Process()
     └─ ZmHttpServer::Perform(task)
        ├─ 获取客户端 IP，忽略 SIGPIPE
        ├─ 添加 CORS 响应头（Access-Control-Allow-Origin: *）
        ├─ OPTIONS 请求 → SetReply(200) → 直接返回
        └─ OnHttpdRequest(task, data, dlen)          ← 虚函数调用
           = HttpServerManager::OnHttpRequest()
              └─ m_router.Serve(task, data, dlen)    ← 委托 ZmHttpRouter
                 │
                 ├── 中间件 1: ZmHttpMiddlewareLogging   ← 请求日志
                 ├── 中间件 2: ZmHttpMiddlewareRecovery  ← 异常恢复
                 │
                 ├── 精确路由匹配:
                 │   GET /         → ServeStaticFile(task, "/html/index.html")
                 │   GET /control  → ServeStaticFile(task, "/html/control.html")
                 │
                 └── 通配符路由匹配:
                     /html/*       → ServeStaticFile(task, uri)
                     /css/*        → ServeStaticFile(task, uri)
                     /js/*         → ServeStaticFile(task, uri)
                     未匹配        → 404
                    │
                    └─ HttpServerManager::ServeStaticFile(task, uri)
                       ├─ 路径拼接: m_wwwRoot + uri
                       ├─ GetFullPathNameA 规范化 + 前缀比对（防目录穿越）
                       ├─ std::ifstream 分块读取（64KB 栈缓冲区）
                       ├─ PutReplyHeader("Content-type", MIME)
                       └─ SetReplyData(chunk) × N

  ③ 回到 Process()（m_deferred = false → 自动触发）
     └─ event_active(m_reply_event, ZM_HTTPD_CONTROL_REPLY, 0)
        ↓
线程: [HTTP 80 事件循环]  ← 被 event_active 唤醒
  ④ ZmHttpServer::OnEvent_Control(fd, ZM_HTTPD_CONTROL_REPLY, ctx)
     └─ doer->SendReply()
        ├─ 写入响应头（m_reply_headers → evhttp_add_header）
        └─ evhttp_send_reply(m_request, status_code, reason, m_reply_buf)
        └─ 创建 1 秒定时器（m_remove_event）

  ⑤ (1 秒后) ZmHttpServer::OnEvent_Timer()
     └─ doer->SendReplyEnd()
        └─ delete this                               ← 释放 ZmHttpdDoer
```

### HTTP 80 调用链速查

```
浏览器
  │  GET http://localhost/ 或 GET http://localhost/control
  ▼
[HTTP 80 事件循环] evhttp → OnHttp_RequestCB
  │  ZmHttpdDoer → m_pool->Submit
  ▼
[HTTP 线程池 Worker] Process → Perform
  │  OnHttpdRequest → m_router.Serve
  │  中间件链 → 路由匹配 → ServeStaticFile
  │  event_active(REPLY)
  ▼
[HTTP 80 事件循环] OnEvent_Control → SendReply → evhttp_send_reply
  │  1s 定时器 → SendReplyEnd → delete this
  ▼
浏览器
```

### HTTP 80 vs JRPC 39440 对比

| 维度 | HTTP 80 | JRPC 39440 |
|------|---------|------------|
| 处理模式 | 同步（Worker 内完成） | 异步（跨两条事件循环） |
| 涉及线程数 | 2（HTTP 80 事件循环 + Worker） | 4（JRPC HTTP 事件循环 + Worker + Hub 事件循环 + JRPC delegate Worker） |
| 跨线程通道 | 无（仅 event_active 回复通知） | ZmNetRequestChannel（SPSC 队列）+ ScheduleInLoop |
| Worker 阻塞 | 不阻塞（静态文件 I/O） | 不阻塞（SubmitAsync 立即返回） |
| 响应路径 | Worker → event_active → HTTP 80 事件循环 → evhttp_send_reply | Worker → Hub 事件循环 → TAP 链 → 响应 → Hub 事件循环 → event_active → JRPC HTTP 事件循环 → evhttp_send_reply |
| DeferReply | 不使用（m_deferred = false） | 使用（m_deferred = true，异步完成后手动触发） |

---

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

- **线程模型** — 三条独立事件循环（Hub / HTTP 80 / HTTP 39440）+ 两个线程池（HTTP Worker / JRPC delegate Worker），跨线程通过 `event_active` + SPSC 队列通信
- **路由中间件** — Express/Gin 风格，`(task, next)` 管道 + 前缀树匹配（`:id` 参数、`*` 通配符）
- **跨线程操作** — `ZmTapContext::Response` / `ZmTapContext::SetDropTimer` / `ZmTapContext::Drop` 内部通过 `ScheduleInLoop` 自动回投到事件循环线程
- **启动/关闭顺序** — 前端先停 → 清理调度残留 → Hub 停（内部释放 delegate 和 EvBaseRunLoop）
- **业务与静态分离** — 所有业务 API 通过 JRPC 端口（39440）暴露，HTTP 端口（80）仅提供静态文件服务
- **BuffereventPair 池化** — `BuffereventPairPool` 预创建 128 对 `bufferevent_pair`，高并发下 O(1) 获取，消除 `socketpair` 系统调用和堆分配开销
- **对象生命周期安全** — `HttpJsonRpcManager::Close()` 仅软关闭（通道拒绝 + HTTP 停止），pair 池推迟到析构函数销毁。调用者确保 `delete` 在 `CloseHub()` 之后执行，防止在飞请求访问已释放的 pair

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
