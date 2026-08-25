# Drogon HTTP 服务器基类(DrogonHttpServer)需求文档

> 状态:待审阅 · 版本:v4 · 日期:2026-08-30
> 范围:定义 `DrogonHttpServer` 基类应具备的能力(功能需求 + 非功能需求 + 约束)
> 前置:ZiMoService 自研 HTTP 栈与旧业务模块已清理,基于 Drogon 1.9.13 绿色重建
> 关联:设计文档 `2026-08-30-drogon-httpserver-base-design.md`(基类设计)、`2026-08-30-drogon-network-layer-design.md`(总体设计)

***

## 1. 背景与目标

### 1.1 背景

ZiMoService 原基于自研 HTTP 栈(libevent)提供三类服务面,各占一端口、各持独立事件循环:

| 服务面         | 端口     | 职责                         |
| ----------- | ------ | -------------------------- |
| 通用 HTTP(前端) | 80/443 | 静态文件 + 页面路由 + SPA 回落       |
| JSON-RPC    | 39440  | JSON-RPC 2.0(兼容保留,现仅 ping) |
| RESTful     | 39441  | ★ 业务 API 入口                |

> 2026-08-30 评审修订(v2.1):三端口在证书存在时**全部升级 HTTPS**(39440/39441 与前端共享全局证书,热加载一致;无证书回落 HTTP)——原"39440/39441 为纯 HTTP 内部端口"的表述作废。

本次为**绿色重建**:自研 HTTP 栈与旧业务模块均已从工程移除(旧源码保留在磁盘,仅作协议/行为/库表结构参考),业务层基于 Drogon 1.9.13 + Drogon ORM 重新实现。对外端口/路径/协议行为与旧版一致(客户端零改动)。

### 1.2 目标

* 以 `DrogonHttpServer` 基类封装 Drogon,作为三个"服务器面"的公共底座

* 业务层通过基类**注册协程回调**,由 Drogon 路由器分离具体 API 动作

* 基类提供服务器所需的基础能力(生命周期/传输/TLS/可观测/异步设施)

* 基类**派生三个服务器面**:`HttpFrontendServer` / `HttpJsonRpcServer` / `HttpRestfulServer`,分别实例化、共享同一 `app()`

### 1.3 关键决策(已定)

| #  | 决策                                                                                                                                                                                                                     |
| -- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| D1 | **保留三端口**(80/443、39440、39441),客户端零改动                                                                                                                                                                                   |
| D2 | Drogon `app()` 全局单例,三个服务器面**共享同一 app()**,结构上多实例、运行时单 app                                                                                                                                                               |
| D3 | 业务回调形态采用**协程 handler**(`Task<HttpResponsePtr>`)                                                                                                                                                                        |
| D4 | 不引入 Drogon 内置 session,沿用自研 `zm_session` cookie + SQLite 会话                                                                                                                                                             |
| D5 | **使用 Drogon ORM**(sqlite3 DbClient)替代自研 SQLite 封装——自编译 SQLite 已移除、符号冲突消除;SQLite 经 `co_await execSqlCoro(...)` 异步访问(Drogon sqlite3 后端独立线程执行,async 非阻塞),**不再需要 RunOnPool**;文件 I/O 与 PBKDF2 仍需"自有线程池 + CallbackAwaiter"离核 |
| D6 | 广播服务(39640,自定义 TCP)不迁移                                                                                                                                                                                                 |
| D7 | 基类派生三个服务器面:`HttpFrontendServer`(80/443)/`HttpJsonRpcServer`(39440)/`HttpRestfulServer`(39441),三者**分别实例化**、各管自己的端口与路由组,共享同一 `app()`(运行时单实例)                                                                             |

***

## 2. 术语

| 术语         | 含义                                   |
| ---------- | ------------------------------------ |
| 事件循环线程(核)  | Drogon 处理 handler 的 I/O 线程,不可做阻塞操作   |
| 阻塞离核       | 把文件/CPU 密集操作从事件循环线程搬到工作线程池           |
| 协程 handler | `Task<HttpResponsePtr>(req)` 形态的业务回调 |
| Filter     | Drogon 按路由组生效的横切机制(鉴权/限流/改写)         |
| Advice     | Drogon 全局挂点(所有请求必经的拦截/观察点)           |
| 水位节流       | 背压控制:排队未发字节超阈值即暂停读盘,防止慢客户端内存线性涨      |

***

## 3. 功能需求

### 3.1 生命周期与运行

#### FR-01 生命周期 + 状态

* 生命周期为**进程级静态**状态机 `Uninit → Initialized → Opened → Closed`,由基类静态方法承载(与 drogon `app()` 全局单例、"run() 只能跑一次"的硬约束对齐):

  * `ZmHttpServer::Init(opts)`:**一次性**注入全局运行参数/证书/全局 advice(`/ping`、访问日志、JSONP);只能调用一次,重复调用报错;

  * `ZmHttpServer::Open()`:后台线程运行 `app().run()`(阻塞语义),绑定失败(端口占用等)在 300ms 内探测并返回 false(fail-fast);

  * `ZmHttpServer::Close()`:全局唯一关闭 = `app().quit()` + join;幂等;`Closed` 为终态,之后不能再 `Open`/`Init`(同一进程内"关了再开"不支持,重启须走进程级重启);

  * 状态查询 `IsInitialized()` / `IsOpened()`。

* 每个派生面对象只负责"端口 + 路由登记",不再有实例级 `Open/Close/IsOpen`。

* **验收**:Init/Open/Close 时序非法调用被状态机拒绝;启动/停止幂等;停止后无残留线程;状态查询准确。

#### FR-02 监听配置

* 提供 `AddListener(port, useSSL, ip, useOldTLS, sslConfCmds)`,可注册多个监听;**证书统一经** **`ZmHttpServer::Init`** **的 Options(`certFile/keyFile`)全局设置**(保证热加载),不在 AddListener 传 per-listener cert。

* 前端 443(HTTPS)+ 39440/39441(HTTP)由它承载。

* **验收**:三个端口同时可用;每个监听独立 SSL 生效;路由按路径全局生效不串扰。

#### FR-03 运行参数设置及查询

* 运行参数经 `ZmHttpServer::Init(opts)` 一次性注入(`ZmHttpServer::Options`):线程数、最大连接数、请求体上限、空闲超时、keep-alive 请求数、请求流、工作池大小、压缩、证书等。

* 注意:这些参数为 **app() 全局**,非 per-listener;且 drogon 运行期不可改 → **只能启动前设定,重启换配置**。

* 特殊值:线程数 0 = 自动 = CPU 核数;keep-alive 请求数 0 = 不限次数回收;空闲超时 0 = 关闭空闲回收;工作池**切勿设 0**。

* **验收**:设置生效;`getClientMaxBodySize` ≥ 10GB(文件中心上传)。

#### FR-04 优雅关闭

* 停止顺序:**业务线程先 join → 再** **`app().quit()`**;在飞请求 graceful 收尾。

* **验收**:Stop 无崩溃、无 UAF、无内存泄漏;在飞阻塞任务不访问已销毁对象(呼应项目 UAF 教训)。

### 3.2 路由与分发

#### FR-05 注册协程回调(核心)

* 提供 `RegisterCoro(path, method, CoroHandler)`,其中 `CoroHandler = Task<HttpResponsePtr>(const HttpRequestPtr&)`。

* 支持路径参数(`/xxx/{1}`)、query 参数读取。

* **验收**:业务层按模块注册的协程 handler 正确被路由匹配调用;阻塞业务可经 RunOnPool 离核后正常返回。

#### FR-06 Filter 路由组隔离

* 提供 `AddFilter(name, filter)`;`RegisterCoro(..., {filterNames})` 支持按路由挂 filter。

* 实现"每服务器各自前置"(鉴权/限流/请求改写)通过路由组 filter 隔离,而非全局 advice 混在一起。

* **验收**:同一 filter 只作用于挂载的路由组;不同服务器面的前置互不干扰。

#### FR-07 统一前置挂点(全局横切)

* 提供全局 advice 挂点(PreRouting/PostRouting/PostHandling/PreSending)。

* 用途:全局访问日志、全局 CORS、全局统计。

* **验收**:横切逻辑一处注册,对所有请求生效;可拦截(短路)与观察两种形态均可用。

#### FR-08 错误处理

* 统一响应助手:`JsonResponse(status, data)` / `ErrorResponse(status, msg)`。

* 自定义 404 页;handler 未捕获异常 → 500(由框架兜底)。

* **验收**:所有响应走统一格式;404/500 语义正确;异常不导致进程崩溃。

#### FR-09 统一响应助手

* 静态方法封装 JSON 响应/错误响应,替代旧 `ZmReqLoopRest::ResponseJson/ResponseError`。

* **验收**:业务层不再直接拼 JSON 响应头,统一助手保证格式一致。

### 3.3 TLS 与安全

#### FR-10 证书附加 / HTTP→HTTPS 升级 / 证书热加载

* 启动时经 `ZmHttpServer::Init` 的 Options(`certFile/keyFile`)附加证书(**全局**,保证热加载);有证书则该监听以 HTTPS(443),无证书则 HTTP(80)。

* 提供 `ReloadCertificates()` 热加载(不改路径,只换内容)。

* **验收**:有证书启动为 HTTPS;热加载后新连接立即用新证书;HTTP 模式无证书零开销。

#### FR-11 TLS session ticket 决策

* **接受 OpenSSL 默认**:ticket 密钥不持久化、不轮换;服务重启后旧 ticket 失效,客户端完整重握一次(影响 \~1 RTT,可接受)。

* 基类**必须暴露** `Options=-SessionTicket` 禁用开关(配置项),默认不启用;启用后客户端每次全量握手,不签发/使用 ticket。

* **验收**:重启后客户端自动重握成功,无连接错误;禁用开关配置后无 ticket 签发;文档注明取舍。

### 3.4 数据传输

#### FR-12 通用文件传输 + 水位节流

* 提供**两条发送路径,业务层按需调用**:

  * `SendFileCoro(req, path, attachmentName)`(方案甲):`newFileResponse`,Range 206/416、`Content-Length`、`Accept-Ranges`、`Content-Disposition` 内置,trantor 内部背压兜底内存;

  * `SendFileStreamCoro(req, path, attachmentName, opts)`(方案乙):`newAsyncStreamResponse` 分块,可配水位阈值/停滞放弃(默认 120s)/进度回调。

* **必须支持内存有界**:方案乙分块发送 + 块间节流让出缓冲排水,内存有界(严格字节水位为**可选增强**:经 `req->getConnectionPtr()` 取 `TcpConnection::setHighWaterMarkCallback`,是否启用由实测定);对端停滞超时可主动放弃(客户端 Range 续传)。

* 提供 `SendFileHybridCoro` 便捷入口:按文件大小自动路由(常规→甲,≥阈值→乙),供不关心细节的业务层使用。

* **验收**:常规下载正确;慢客户端内存不随下载时长线性涨;Range 续传正确;停滞放弃可续传;进度回调可用;连接关闭即中止并释放 fd。

#### FR-13 流式响应

* 提供 `SendStream(resp, cb, disableKickoff=true)` 封装 `newAsyncStreamResponse`。

* 支撑音频流、zip 打包流、SSE。

* **验收**:分块发送正确;长流不被踢出超时误杀;业务侧发送线程可安全 `send`/`close`。

#### FR-14 请求超时

* 连接级:空闲超时(`setIdleConnectionTimeout`)框架内置。

* **业务级 deadline 需自建**:`RegisterCoroWithDeadline(path, method, cb, deadlineMs)`。

  * 到期由事件循环定时器发 504;**TryReply 原子门保证只回一次**;

  * 业务晚到结果经**弱引用守卫 + connected() 检查**后安全丢弃,防 UAF。

* **验收**:超时返回 504 且只回一次;晚到业务不访问已销毁连接;可断点续传的下载不受 deadline 误杀。

#### FR-15 上传流式接收

* 全局 `setClientMaxBodySize(10GB)`(单请求上限,所有路由统一)+ `enableRequestStream(true)`;基类提供流式接收封装:`RegisterStreamCoro(path, method, handler, filters, maxBytes)`——handler 携带框架注入的 `RequestStreamPtr`,数据逐块交付不整体吃进内存;`SaveStreamToFile(stream, destPath, opts, &tooLarge)` 落盘助手(块到即写)。

* `maxBytes` 为**路由级上限**(0 = 不额外限制,全局 10GB 兜底):基类入口按 `X-File-Size` 声明自动早拒(超限 → null reader 丢弃 + 413,不进入业务),并写入 `req` attributes(`ZmStreamMaxBytes`)供落盘兜底取用。

* 保留业务层 `X-File-Size` 声明二次校验语义。

* **验收**:大文件上传不整体吃进内存;超全局上限由框架拒绝;超路由上限(声明早拒 / 落盘实时兜底)正确拒绝并清理半成品;断点中断可续。

### 3.5 实时通信

#### FR-16 WebSocket

* 提供 `RegisterWebSocket(path, WsCallbacks{onOpen/onClose/onAuth/onMessage})`,单独一路(与协程 handler 并列,不合并)。

* 握手鉴权(`onAuth` 收**完整握手请求** `HttpRequestPtr`,false 在升级回调内 `close` 拒绝)可复用业务层 `AuthAndTouch`;消息处理内可使用协程。

* **验收**:握手/消息/关断三事件正确;鉴权失败拒绝升级;连接生命周期收尾无 UAF。

### 3.6 可观测与公共设施

#### FR-17 访问日志

* 记录方法/路径/状态码/耗时。

* 实现:`ZmHttpServer::Init` 一次性注册一对 advice(PreRouting 记录起始时间入 req attributes;PostHandling 结算耗时),单行 `PUBLIC_LOG_*` 输出(与运行日志同文件、按标签区分),覆盖方法/路径/状态码/耗时/字节数;不自建 access.log、不使用 AccessLogger 插件。

* **验收**:每个请求一条日志,信息完整,对性能影响可忽略。

#### FR-18 压缩

* 静态文件 gzip + 动态响应压缩(gzip/brotli)开关。

* **验收**:开启后对应内容带 `Content-Encoding`;与 Range 下载不冲突(文件下载不压缩)。

#### FR-19 RunOnPool 阻塞离核桥接

* 提供 `RunOnPool<T>(fn) -> Task<T>`:提交阻塞任务到**自有工作线程池**(复用 `ZmThreadPool`),经 `CallbackAwaiter` 桥回协程。

* 工作线程池为**单一静态共享池**(三面共用,构造/析构随进程;大小可配 `SetWorkPoolSize(n)`,默认 8,首次 `RunOnPool` 前生效),业务层只调 `RunOnPool`。

* **验收**:阻塞业务(文件 I/O、PBKDF2)不阻塞事件循环;并发请求共享固定线程池(非每请求一线程);晚到结果安全。SQLite 走 Drogon ORM `execSqlCoro` 异步访问,不占用工作线程池。

#### FR-20 静态文件 + SPA 回落 + 404

* 提供 `SetDocumentRoot(www)`(防穿越/Range/MIME)+ `SetImplicitPage(portal.html)`(SPA history fallback)+ `SetNotFoundPage(404.html)`。

* **验收**:静态资源正确;`/portal/*` 刷新回落 portal.html;404 页正确;`doc/` 目录不可达。

#### FR-21 CORS 凭据

* 跨端口 cookie 场景,提供全局 CORS(PreSending advice 或全局 filter):`Access-Control-Allow-Origin`(回显)+ `Credentials` + OPTIONS 预检。

* **验收**:前端 39441 跨端口携带 cookie 请求正常;预检正确。

#### FR-22 80→443 重定向(HTTPS 模式)

* HTTPS 模式下 80 端口对前端路径 301 → 443。

* **验收**:`http://host/...` → `https://host/...` 重定向正确。

#### FR-23 健康检查(默认能力)

* 默认注册 `GET /ping` → `{"pong":true}`。

* **验收**:空跑可用,不依赖业务模块。

#### FR-24 JSONP

* **自动识别型**(与主流中间件语义一致):请求为 GET 且带合法 `callback` 参数、响应为 JSON 时,基类经全局 PreSending advice 自动包装为 `callback(json);`(Content-Type `application/javascript`);不满足任一规则 → 原样返回(不污染普通 REST/静态/WS)。

* callback 名须**白名单校验**(`[A-Za-z0-9_.]`,长度 ≤128),防 XSS 反射;非法名不包装(原样返回)。

* 保留显式助手 `JsonpResponse(req, data)`(个别路由手动决定;同样复用白名单)。

* 全局开关 `SetAutoJsonp(false)` 可关闭(三面共享,默认开)。

* **验收**:GET+callback 的 JSON 响应自动成为可执行 JS 包装;非法 callback 名不包装;无 callback / 非 GET 行为与常规 JSON 一致;关闭开关后全部原样。

#### FR-25 派生服务器面

* 基类必须可派生三个具体服务器面:`HttpFrontendServer`(80/443)、`HttpJsonRpcServer`(39440)、`HttpRestfulServer`(39441)。

* 三者**分别实例化**:各自 `AddListener` 自己的端口、各自注册自己的路由组(filter/advice 隔离)、各自状态查询(`IsOpen`/`IsHttps`/`GetPorts`)。

* 三者**共享同一** **`app()`**(运行时单实例、单事件循环池、单路由表,靠路径前缀区分)。

* **验收**:三个派生类可独立构造/注册/查询;路由互不串扰;符合 D2/D7。

***

## 4. 非功能需求

| 类别  | 要求                                                                            |
| --- | ----------------------------------------------------------------------------- |
| 性能  | 事件循环线程**禁止阻塞操作**;并发由事件循环承载 + 固定线程池消化阻塞;慢客户端内存有界                               |
| 安全  | 静态资源防目录穿越;会话沿用自研 `zm_session`;限流持久化在业务层(基于 Drogon ORM),基类仅提供内存 RateLimiter 可选 |
| 可用性 | 优雅关闭无 UAF/泄漏;连接关闭即释放资源;业务 deadline 只回一次                                       |
| 兼容性 | 三端口/路径/协议/响应格式与旧版一致;客户端零改动                                                    |
| 可维护 | 基类只提供**机制**,不掺业务具体;业务模块通过基类接口注册,不直接依赖 `app()` 单例                              |

***

## 5. 约束与边界(Drogon 1.9.13 硬限制)

1. `app()` 全局单例;路由/运行参数全局;无 per-listener 路由/advice。
2. **无** **`getThreadPool()`**;`drogon::Task` 不能直接 `co_await std::future` → 阻塞离核必须"自有线程池 + CallbackAwaiter"。
3. **无 per-request body 上限覆盖**(`setClientMaxBodySize` 全局)→ 每接口差异化靠业务层二次校验。
4. **无内置业务级 deadline** → `RegisterCoroWithDeadline` 自建。
5. **无 ticket key 管理 API** → 接受 OpenSSL 默认(FR-11)。
6. **WebSocket 单独一路**(WebSocketController),协程 handler 不涵盖。
7. 不做:广播 39640 迁移、Drogon 内置 session。**Drogon ORM(sqlite3 DbClient)为已启用能力**,业务层基于它重新实现 DB 层。

***

## 6. 决议记录(均已定,无待决项)

| 项          | 决议                                                                                                              |
| ---------- | --------------------------------------------------------------------------------------------------------------- |
| TLS ticket | 接受 OpenSSL 默认 + 基类暴露 `Options=-SessionTicket` 禁用开关,默认不启用 → FR-11                                                |
| 访问日志       | `ZmHttpServer::Init` 一次性注册 PreRouting+PostHandling 一对 advice,`PUBLIC_LOG_*` 单行输出,不自建 → FR-17                    |
| 流式接收       | `RegisterStreamCoro(..., maxBytes=0)` 路由级上限(声明早拒内置 + attributes 透传),`SaveStreamToFile` 落盘兜底;全局 10GB → FR-15     |
| 健康检查       | `/ping` 进基类默认能力 → FR-23                                                                                         |
| 文档目录       | 已由用户调整                                                                                                          |
| JSONP      | 自动识别型(PreSending advice):GET + 白名单 `callback` + JSON 响应 → 自动包装;`JsonpResponse` 保留显式通道;开关 `SetAutoJsonp` → FR-24 |
| 文件传输       | 双路径(方案甲/乙)+ `SendFileHybridCoro`,业务层按需调用 → FR-12                                                                |
| DB 访问      | 使用 Drogon ORM sqlite3 DbClient,SQLite 不再需要 RunOnPool → D5                                                       |
| 唯一待实测项     | trantor Windows `sendFile` 内存行为:仅影响 FR-12 方案甲适用上限与默认阈值(实测后调整阈值即可,不阻塞设计)                                         |

***

## 7. 验收清单(总)

1. 三端口启动/停止/状态正确,路由互不串扰
2. 业务层协程回调注册并正确被路由调用;阻塞业务经 RunOnPool 离核正常返回
3. 证书热加载生效;HTTPS 升级/HTTP 降级正确;ticket 重启后客户端自动重握
4. 通用文件传输:Range 206/416、水位节流、连接关闭即中止
5. 流式响应(音频/zip/SSE)正常;长流不被误杀
6. 业务 deadline:超时 504 且只回一次;晚到结果安全丢弃无 UAF
7. WebSocket 三事件 + 鉴权拒绝正确
8. 静态文件/SPA 回落/404/防穿越正确;`doc/` 不可达
9. CORS 凭据、80→443 重定向、访问日志、压缩符合预期
10. 优雅关闭:无崩溃/泄漏;在飞请求 graceful 收尾
11. JSONP:带 `callback` 返回 JS 包装、非法名拒绝、无 callback 走常规 JSON
12. 派生:三服务器面独立实例化/注册/状态查询,共享 app() 不串扰

