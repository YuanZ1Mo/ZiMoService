# DrogonHttpServer 基类设计文档(含三个服务器面详细设计)

> 状态:待审阅 · 版本:v2.6 · 日期:2026-08-31
> 范围:`ZmHttpServer` 基类(`ZmHttpServer`,v2.3 前名 DrogonHttpServer)+ 三个派生服务器面(前端/JRPC/RESTful)的详细设计
> 需求:基于 `2026-08-30-drogon-httpserver-requirements.md`(25 条 FR + D1\~D7)
> 依赖:Drogon 1.9.13(头文件 + 静态库在 `ZiMoPublic\drogon`)、`ZmThreadPool`(`zm_util_thread.h`)、Drogon ORM
> 修订:v2.6 生命周期重构(用户决策,对齐 drogon 单次 run 硬约束):实例级 Open/Close/BootCoordinator 引用计数 → **进程级静态状态机** `Uninit→Initialized→Opened→Closed`;全局参数收敛进 `ZmHttpServer::Options` 经 `Init(opts)` 一次性注入;全局 advice(/ping/访问日志/JSONP)从"首个 Open 经 once_flag"改为 `Init` 内注册;派生面收敛为"端口+路由登记"(删除实例 Open/Close/IsOpen);三个 Manager 与 NetDock 生命周期委托同步删除。运行期唯一可热更新能力:证书 `reloadSSLFiles()`;不支持运行期单端口启停/热重启(重启须进程级)。
> 修订:v2.1 评审联动:①SPA 回落改用 advice(setImplicitPage 语义为"目录解析",非 SPA 回落,头文件核实);②补四个 advice 挂点到基类接口;③新增 per-port 门禁纪律(全局路由表下恢复旧"端口隔离"行为);④AccessLogger 经 `loadConfigJson` 注入最小配置;⑤方案乙改为定时器链驱动;⑥deadline 定时器放连接所属 loop;⑦AddFilter/RegisterCoro 顺序赋约束;⑧JsonpResponse 改收 req;⑨命名统一 DrogonHttpServer;⑩宿主分层(NetDock/Manager/Portal)并入 §2.5(本期交付 = 基类 + 三面 + 测试接口,业务路由延后);v2 并入原 network-layer-design 的三面细节并修正其过时说法;v1.1 根据代码评审修正(共享 app() 协调、水位机制、证书热加载、Range 解析、Filter 适配、WS onAuth、去重纪律、基类形态);v2.4(还原恢复)流式接收定版:`RegisterStreamCoro` 增 `maxBytes` 路由级上限(X-File-Size 早拒内置 + attributes 透传),`SaveStreamToFile` 落盘助手;全局上传上限 4GB→10GB(FR-03/FR-15);FR-17 访问日志 = PreRouting/PostHandling advice(`PUBLIC_LOG_*` 单行),弃 AccessLogger 插件;v2.5 JSONP 改自动识别型:全局 PreSending advice(GET + 白名单 callback + JSON 响应 → 自动包装),白名单抽 `IsValidJsonpCallback` 复用,开关 `SetAutoJsonp`(默认开),`JsonpResponse` 保留显式通道

***

## 1. 总览

| 项    | 决策                                                                                                    |
| ---- | ----------------------------------------------------------------------------------------------------- |
| 形态   | **抽象基类** `ZmHttpServer`(共享实现 + 一个纯虚 `RegisterRoutes()`,可覆写 virtual),派生三个服务器面,共享同一 `drogon::app()` |
| 生命周期 | **进程级静态状态机**(`Uninit→Initialized→Opened→Closed`),`Init(opts)/Open()/Close()` 均为静态方法;drogon `app()` 单次 run,关闭即终态 |
| 派生面  | `HttpFrontendServer`(80/443)· `HttpJsonRpcServer`(39440)· `HttpRestfulServer`(39441),只做"端口 + 路由登记" |
| 业务回调 | 协程 handler(`Task<HttpResponsePtr>`),阻塞业务经 `RunOnPool` 离核                                              |
| DB   | Drogon ORM(sqlite3 DbClient),SQLite 不再占工作线程池                                                          |
| 文件传输 | 双路径(方案甲/乙)+ Hybrid 自动路由                                                                               |
| 隔离   | 每服务器面前置经**路由组 Filter 隔离**;全局横切用 advice(静态去重)                                                          |
| 会话   | 不引入 Drogon 内置 session,沿用自研 `zm_session` cookie + SQLite 会话(业务层)                                       |
| 广播   | 39640 自定义 TCP,不迁移(D6)                                                                                 |

**运行时事实**:`app()` 单例、路由表全局、运行参数全局、`run()/quit()` 全局——三个派生类"结构上多实例、运行时单 app",靠**路径前缀**区分不串扰(D2)。对外端口/路径/协议/响应格式与旧版一致(客户端零改动,D1)。

**生命周期硬约束(v2.6)**:drogon `app()` 全局单例且 `run()` 只能跑一次 → 生命周期为进程级一次(`Init → Open → Close → 进程退出`);`Close` 后不能再 `Open`/`Init`,不支持运行期单端口启停/热重启(需要时走进程级重启)。运行期唯一可热更新能力:证书 `ReloadCertificates()`。

**本期不实现**:Drogon 内置 Session、Redis、视图模板(CSP)、广播迁移。

***

## 2. 类层次、生命周期协调与文件划分

### 2.1 类层次

```
ZmHttpServer(抽象基类:共享实现——静态生命周期/监听/响应助手/RunOnPool/全局去重;纯虚 RegisterRoutes())
  ├── HttpFrontendServer   # 80/443:静态 + SPA + 页面路由(覆写路由注册)
  ├── HttpJsonRpcServer    # 39440:/zimo/jrpc
  └── HttpRestfulServer    # 39441:/zimo/api ★业务入口
```

基类为**抽象基类**(含一个纯虚 `RegisterRoutes()`):生命周期为**基类静态方法**(`Init/Open/Close`),`AddListener/Setup/SetDocumentRoot/SetRootPath` 为实例方法;派生类覆写 `RegisterRoutes()` 等 virtual 完成各自路由注册。**派生对象只做"端口 + 路由登记",不再承载生命周期。**

### 2.2 共享 app() 生命周期协调(FR-01/02/04)

Drogon 要求 **`addListener`** **必须先于** **`app().run()`**(run 时统一绑定监听)。v2.6 起生命周期为**进程级静态状态机**,不再用 BootCoordinator 引用计数:

```
状态机:Uninit → Initialized(Init) → Opened(Open) → Closed(Close, 终态)

相位契约:
  Phase1 Configure:ZmHttpServer::Init(opts)(全局参数/证书/全局 advice,一次)
                   → 构造三面 → 各自 AddListener/Setup/RegisterCoro(全部在 Open 前完成)
  Phase2 Start:    ZmHttpServer::Open() —— 后台线程跑 app().run(),绑定失败 300ms 内 fail-fast
  Phase3 Stop:     ZmHttpServer::Close() —— app().quit() + join;幂等;Closed 终态
```

```cpp
// 基类静态生命周期(进程级一人份)
struct ZmHttpServer::Options { /* 全局运行参数/证书,见 §3.1 */ };
static bool ZmHttpServer::Init(const Options& opts);   // 一次性;重复调用报错
static bool ZmHttpServer::Open();                       // 须已 Init + 已登记监听;绑定失败返回 false
static void ZmHttpServer::Close();                      // quit+join;幂等;Closed 终态
static bool ZmHttpServer::IsInitialized();
static bool ZmHttpServer::IsOpened();
```

* **约束**:`AddListener` 必须在 `Open()` 前调用(Open 后调用被拒绝并报错);`Init` 只能一次;`Open` 前必须 `Init` 且至少登记一个监听;`Close` 后为终态,不能再 `Open`/`Init`(drogon `run()` 单次硬约束,重启须进程级)。

* `GetPorts()` 返回本面已注册监听端口;`IsHttps()` 返回本面是否含 443 监听。

* 关闭顺序(FR-04):业务线程先 join → 最后 `ZmHttpServer::Close()` 触发 `quit()`+join。

* **运行期不可改配置**:drogon 配置型 setter 运行期不可改(多数带 running 守卫/不生效),故全部经 `Init(opts)` 启动前注入;运行期唯一可热更新 = 证书 `ReloadCertificates()`。

### 2.3 全局注册去重纪律(FR-07/21/23)

advice、`/ping`、访问日志、自动 JSONP、CORS 均注册在全局 `app()` 上,**各面不得各自重复注册**:

* `/ping`、访问日志 advice、自动 JSONP advice:**由** **`ZmHttpServer::Init`** **一次性注册一次**(v2.6 起不再依赖"首个 Open 经 once_flag",语义更准:全局项在 Init 就绪);

* **advice**:由归属面经 `RegisterXxxAdvice` 各注册一次(门禁/SPA/重定向归前端面,CORS 归 Restful 面),基类按挂点名防重复(§4.3);

* 三个面各自的 `RegisterRoutes()` 只注册**自己路径前缀**的路由,天然不冲突。

### 2.4 宿主分层(NetDock / Manager / Portal,v2.1 并入)

按用户确认的分层,生命周期由 **NetDock** 承载,三层职责:

```
ServiceCenter
 ├── NetDock(网络层宿主)
 │    ├── HttpServerManager     → 持有 HttpFrontendServer(80/443)
 │    ├── HttpJsonRpcManager    → 持有 HttpJsonRpcServer(39440)
 │    └── HttpRestfulManager    → 持有 HttpRestfulServer(39441)
 └── ServicePortal(业务层):构造时经 NetDock 取三个 server 引用 → 注册全部路由(Phase1)
```

* `ServiceCenter::OnStart`:构造 NetDock → `NetDock::Init()`(内部先 `ZmHttpServer::Init(opts)` 全局一次,再构造/配置三面)→ 构造 ServicePortal(注册路由,Phase1)→ `NetDock::Open()`(即 `ZmHttpServer::Open()`,Phase2);`OnStop`:`ServicePortal.Shutdown()`(业务线程收尾)→ `NetDock::Close()`(即 `ZmHttpServer::Close()`,Phase3,quit+join 全局一次)。

* **Manager** 职责:仅配置本面服务器(`AddListener`/`SetDocumentRoot`/`SetRootPath`/`Setup`),并暴露 `GetServer()` 给 NetDock;**不再有 Open/Close/IsOpen**(生命周期归静态基类)。

* **ServicePortal** 职责:路由注册与 handler 业务逻辑(全部),不感知 Drogon API(经基类接口)。

* 三个面均无业务可挂时(本期测试),仅注册测试路由;`RegisterRoutes()` 纯虚仍由各派生面实现(实现为"本面内置路由 + 测试路由")。

### 2.5 文件划分

> v2.3 迁移(用户决策):基类与三个服务器面为平台能力,按现有命名规范下沉 **`ZiMoPublic\net`**(`zm_net_*` / `Zm*` / `ZM_NET_*_H`);服务工程仅保留宿主层。

| 文件                                   | 位置             | 内容                                                                       |
| ------------------------------------ | -------------- | ------------------------------------------------------------------------ |
| `zm_net_http_server.h/.cpp`          | ZiMoPublic/net | 基类 `ZmHttpServer`(静态生命周期 Init/Open/Close + Options + 响应助手 + RunOnPool + advice 挂点 + 公共类型) |
| `zm_net_http_frontend_server.h/.cpp` | ZiMoPublic/net | 前端面 `ZmHttpFrontendServer`:document root + SPA advice + 404 + 页面路由 + 重定向 |
| `zm_net_http_jsonrpc_server.h/.cpp`  | ZiMoPublic/net | JRPC 面 `ZmHttpJsonRpcServer`:`/zimo/jrpc` handler + 信封分发                 |
| `zm_net_http_restful_server.h/.cpp`  | ZiMoPublic/net | RESTful 面 `ZmHttpRestfulServer`:业务路由注册 + WebSocket + CORS 挂点             |
| `net/rest_util.h`                    | ZiMoService    | 业务辅助 `RequireModule`(业务层,声明先保留)                                          |
| `net_dock.h/.cpp`(重建既有空文件)           | ZiMoService    | 宿主:持有三 Manager,Init(全局 Options + 三面配置),Open/Close 转发静态,暴露 GetXxxServer() |
| `http_server_manager.h/.cpp` 等(重建)   | ZiMoService    | 各持有本面服务器派生类实例,仅配置(AddListener/Setup/GetServer),无生命周期委托(名字不变)            |

***

## 3. 生命周期与运行(FR-01\~04)

### 3.1 基类(抽象基类:一个纯虚 `RegisterRoutes()`,其余 virtual 可覆写)

```cpp
// net/drogon_http_common.h
using CoroHandler = std::function<
    drogon::Task<drogon::HttpResponsePtr>(const drogon::HttpRequestPtr&)>;

struct SendFileStreamOptions {
    size_t   chunkSize      = 1 * 1024 * 1024;   // 分块粒度(FR-12 方案乙)
    size_t   interBlockMs   = 50;                // 块间定时器间隔(定时器链节流,见 §6.1)
    size_t   watermarkBytes = 8 * 1024 * 1024;   // 严格水位目标(可选增强,见 §6.1)
    int64_t  stallAbortMs   = 120 * 1000;        // 对端停滞放弃
    std::function<void(uint64_t sent, uint64_t total)> onProgress;  // 进度回调(可选)
};

// net/drogon_http_server.h —— 基类(v2.6:生命周期静态化)
class ZmHttpServer
{
public:
    virtual ~ZmHttpServer() = default;

    // ── 全局运行参数(进程级全局,drogon 运行期不可改;经 Init 一次性注入) ──
    struct Options {
        size_t threadNum = 0;              // 事件循环线程数(0 = 自动 = CPU 核数)
        size_t maxConnections = 8192;      // 最大连接数护栏(0 大概率不限,慎用)
        size_t clientMaxBodySize = 10ULL*1024*1024*1024;  // 单请求体上限(0 无特殊语义,别设 0)
        size_t idleTimeoutSec = 90;        // keep-alive 空闲回收秒(0 = 关闭空闲回收)
        size_t keepaliveRequests = 0;      // 单连接请求数上限(0 = 不限次数回收)
        bool enableRequestStream = true;   // 上传流式落盘开关
        size_t workPoolSize = 8;           // 业务工作池线程数(切勿设 0)
        bool gzip=false, brotli=false;     // 动态压缩
        bool gzipStatic=false, brotliStatic=false;  // 静态压缩(本捆绑仅 gzip 生效)
        bool ticketDisabled = false;       // TLS SessionTicket 禁用
        std::string certFile, keyFile;     // 全局证书(空 = 纯 HTTP)
    };

    // ── 静态生命周期(进程级一次;状态机 Uninit→Initialized→Opened→Closed) ──
    static bool Init(const Options& opts);   // 一次性;重复调用报错
    static bool Open();                      // 后台 app().run(),绑定失败 300ms fail-fast
    static void Close();                     // quit+join;幂等;Closed 终态
    static bool IsInitialized();
    static bool IsOpened();

    bool IsHttps() const;                    // 本面监听含 443
    std::vector<uint16_t> GetPorts() const;  // 本面监听端口

    // ── 监听配置(FR-02,per-listener SSL 指 useSSL/useOldTLS,证书全局经 Init;必须先于 Open) ──
    virtual void AddListener(uint16_t port, bool useSSL = false,
                             const std::string& ip = "0.0.0.0",
                             bool useOldTLS = false,
                             const std::vector<std::pair<std::string, std::string>>& sslConfCmds = {});

    // ── TLS(FR-10/11;证书统一经 Init 的 Options 全局 setSSLFiles 保证热加载) ──
    virtual bool ReloadCertificates();               // reloadSSLFiles()(运行期唯一可热更新)
    virtual void SetTicketDisabled(bool disable);    // 全局 sslConfCmds 追加 Options=-SessionTicket

    // ── 阻塞工作池(FR-19;单一静态共享池,大小可配,默认 8,首次 RunOnPool 前生效) ──
    static void SetWorkPoolSize(size_t n);  static size_t GetWorkPoolSize();

    // ── 协程路由(FR-05,带可选 filter 约束) ──
    virtual void RegisterCoro(const std::string& path, drogon::HttpMethod m,
                              CoroHandler h,
                              const std::vector<std::string>& filters = {});
    virtual void RegisterCoroWithDeadline(const std::string& path, drogon::HttpMethod m,
                                          CoroHandler h, size_t deadlineMs,
                                          const std::vector<std::string>& filters = {});

    // ── Filter(FR-06;std::function 适配为 HttpFilter,见 §4.2) ──
    virtual void AddFilter(const std::string& name,
                           const std::function<bool(const drogon::HttpRequestPtr&,
                                                    drogon::HttpResponsePtr&)>& f);

    // ── 静态文件 + SPA + 404(FR-20) ──
    virtual void SetDocumentRoot(const std::string& www);
    virtual void SetImplicitPage(const std::string& file);
    virtual void SetNotFoundPage(const std::string& file);

    // ── 本面业务根路径(v2.3,可自定义) ──
    virtual void SetRootPath(const std::string& path);   // 空 = 关闭本面门禁
    virtual const std::string& GetRootPath() const;
    virtual void AddOtherRootPath(const std::string& path);  // 前端面登记外来前缀

    // ── 文件传输(FR-12,双路径;Range 由基类内部解析) ──
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                 const std::string& attachmentName = "");
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileStreamCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       const SendFileStreamOptions& opts = {});
    virtual drogon::Task<drogon::HttpResponsePtr>
    SendFileHybridCoro(const drogon::HttpRequestPtr& req, const std::string& path,
                       const std::string& attachmentName,
                       size_t threshold = 2ULL*1024*1024*1024,
                       const SendFileStreamOptions& streamOpts = {});

    // ── 流式(FR-13;工厂:返回流式响应,调用方自行设头) ──
    using StreamCb = std::function<void(drogon::ResponseStreamPtr)>;
    static drogon::HttpResponsePtr MakeStreamResponse(StreamCb cb, bool disableKickoff = true);

    // ── WebSocket(FR-16;onAuth 收完整握手请求) ──
    struct WsCallbacks {
        std::function<void(const drogon::WebSocketConnectionPtr&, const drogon::HttpRequestPtr&)> onOpen;
        std::function<void(const drogon::WebSocketConnectionPtr&)> onClose;
        std::function<bool(const drogon::HttpRequestPtr& /*握手请求*/)> onAuth;  // false → 升级回调内 close 拒绝
        std::function<void(const drogon::WebSocketConnectionPtr&, std::string&&, drogon::WebSocketMessageType)> onMessage;
    };
    virtual void RegisterWebSocket(const std::string& path, const WsCallbacks& cb);

    // ── 全局横切 advice(FR-07;同名注册重复由基类报错,详见 §4.3) ──
    // 签名与 Drogon 1.9.13 一致:PreRouting/PostRouting 可拦截(3 参);
    // PostHandling/PreSending 为观察(2 参 void(req, resp)),按头文件契约取双向接口。
    virtual void RegisterPreRouting(std::function<void(const drogon::HttpRequestPtr&, drogon::AdviceCallback&&, drogon::AdviceChainCallback&&)> a);
    virtual void RegisterPostRouting(std::function<void(const drogon::HttpRequestPtr&, drogon::AdviceCallback&&, drogon::AdviceChainCallback&&)> a);
    virtual void RegisterPostHandling(std::function<void(const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr&)> a);
    virtual void RegisterPreSending(std::function<void(const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr&)> a);

    // ── 统一响应助手(FR-08/09/24,静态) ──
    static drogon::HttpResponsePtr JsonResponse(int status, const Json::Value& data);
    static drogon::HttpResponsePtr ErrorResponse(int status, const std::string& msg);
    static drogon::HttpResponsePtr JsonpResponse(const drogon::HttpRequestPtr& req, const Json::Value& data);

    // ── 阻塞离核(FR-19,静态模板) ──
    template <typename T> static drogon::Task<T> RunOnPool(std::function<T()> fn);

protected:
    virtual void RegisterRoutes() = 0;   // 派生面实现:注册自己路径前缀的路由
    std::vector<uint16_t> m_listeners;   // 本面监听端口
    bool m_setupDone = false;

    // ── per-port 辅助(共享路由表下恢复"端口隔离",见 §4.5) ──
    static uint16_t LocalPort(const drogon::HttpRequestPtr& req);  // ntohs(getLocalAddr().portNetEndian())
    bool IsLocalPortIn(const drogon::HttpRequestPtr& req) const;   // 请求本地端口 ∈ m_listeners
};
```

**运行参数说明(v2.6)**:全部全局运行参数收敛进 `Options`,经 `Init(opts)` 一次性注入(drogon 无运行时 getter,且运行期不可改配置,故不再提供实例 `SetXxx/GetXxx`)。工作池大小保留静态 `SetWorkPoolSize`(首次 RunOnPool 前生效)。

### 3.2 运行线程模型

* 相位契约见 §2.2:`ZmHttpServer::Init(opts)` → `AddListener/RegisterRoutes` → `Open` → `Close`;`AddListener` 强制先于 `Open`。

* `app().run()` 由静态 `ZmHttpServer::Open()` 起 `std::jthread`;`Close()` 调 `app().quit()` 后 join(全局一次)。

* **优雅关闭顺序(FR-04)**:业务线程(音频发送/一致性校验/RunOnPool 池)先 join → 再 `ZmHttpServer::Close()`。

* **并发模型说明**:Drogon 无独立"业务请求池",并发 = `Options.threadNum`(0=自动) IO 线程 + 协程多路复用;连接级兜底 = `Options.maxConnections`;阻塞兜底 = `RunOnPool`(固定池)。

* **线程纪律红线(评审红线)**:

  1. **SQLite**:经 Drogon ORM `execSqlCoro` 异步访问(独立线程执行),严禁在 handler 内同步执行;
  2. **PBKDF2(600k 迭代)**:登录/注册密码散列必须 `RunOnPool` 离核;
  3. **文件 I/O / zip 打包 / mmap**:一律 `RunOnPool` 或独立业务线程(文件中心后台线程保留);
  4. **deadline 兜底**:业务级 deadline 由 `RegisterCoroWithDeadline` 承担;流式/下载端点不设死线(同现语义)。

***

## 4. 路由与分发(FR-05\~09,24)

### 4.1 RegisterCoro

`app().registerHandler(path, CoroHandler, {method} + filter 约束)`——`HttpBinder` 支持 `Task<HttpResponsePtr>` 返回型。路径参数 `/xxx/{1}` 经 `req->getParameter("1")` 读取。

**v2.2 实测修订(Windows 环境)**:

* `CoroHandler` 形参改用**按值** `HttpRequestPtr`——本捆绑 drogon 的 `FunctionTraits` 协程特化仅匹配 `Task<Resp>(*)(HttpRequestPtr, ...)`(member/functor 链落点;const 引用会静默落入无 `first_param_type` 的基础匹配);

* 含 `{1}` 占位符的路径经 `app().registerHandler` 在本环境**访问违例**——`RegisterCoro` 内部统一改走 `registerHandlerViaRegex` + 手工转换(`PathPatternToRegex`:`{N}`→`([^/]+)`),捕获组经 `req->getRoutingParameters()` 读取;

* 禁止给已注册 handler 再套一层 `co_await h(req)` 包装协程(会崩),基类直接注册业务 `std::function`。

### 4.2 Filter 适配(FR-06)

`AddFilter(name, f)` 实现机制:

1. 定义一个 `DrObject<HttpFilter>` 派生类 `FuncFilter`,构造时注入 `std::function<bool(req, resp)>`;`doFilter(req, resp, callback, chain)` 内调 `f(req, resp)`:返回 true → `chain()`;false → 已写 resp(如 401/403),不再 `chain()`。

2. **按名注册(注意:不存在** **`app().registerFilter(name)`** **这类按任意名注册的 API)**。Drogon Filter 经 `DrObject` 反射按**类名**注册(`app().registerFilter<T>(instance)` 内部 `DrClassMap::setSingleInstance`)。自定义名需 `DrClassMap::registerClass(name, nullptr, sharedFactory)` 以 `name` 注册 + 单例注入,使约束字符串 `name` 可被框架按名解析;**推荐**业务层直接定义各自 `HttpFilter` 派生类(反射类名即约束名),`AddFilter` 仅作便捷包装。

3. `RegisterCoro(path, m, h, {filters})` 把 filter 名作为 `internal::HttpConstraint`(Middleware)传入 → 按名挂到该路由(per-route 生效)。

4. **顺序约束(v2.1 补充)**:filter 实际在**请求到达时**经 `DrClassMap::getSingleInstance(name)` 解析(HB0 实测),故 `AddFilter` 只需先于首个请求;基类在 `RegisterCoro` 时校验名称未注册 → 打 ERROR(注册保留,便于 `AddFilter` 后补)。**推荐** Phase1 内顺序:先所有 `AddFilter` 再所有 `RegisterCoro`。

三面前置隔离示例:RESTful 挂 `AuthFilter`/`RateLimitFilter`,前端/JRPC 不挂。

### 4.3 全局横切(FR-07)

* 基类提供 `RegisterPreRouting/PostRouting/PostHandling/PreSending` 四个挂点(**与 Drogon 1.9.13 签名一致,见 §3.1**),调用即透传 `app().registerXxxAdvice`。

* **去重纪律**:`app()` 全局单例,advice 也在全局——基类内部按 `name`(派生面名 + 挂点类型)登记,同一挂点重复注册仅打 ERROR 不生效;**基类内置项(/ping、访问日志 advice、自动 JSONP advice)由** **`ZmHttpServer::Init`** **一次性注册一次**(v2.6 起不再依赖 once_flag);各面自注册 advice(门禁/SPA/CORS)在各自 `RegisterRoutes()` 内完成,天然每面一次。

* 用途:CORS 凭据(FR-21,由业务层显式挂到 Restful 面)、per-port 门禁(§4.5)、全局统计、日志计时。

### 4.4 per-port 门禁(共享路由表下的"端口隔离",v2.1 新增)

`app()` 路由表全局,旧版"各端口只服务各面路由"的行为无法自然成立(80 端口也会命中 `/zimo/api/*`、39441 也会返回 `/login` 静态页)。**每个派生面在** **`RegisterRoutes()`** **内注册自己的 PreRouting 门禁 advice**,策略:

* 前端面:请求本地端口 ∈ {本面监听端口}(经 `IsLocalPortIn`)且路径以 `/zimo/` 开头 → 404;

* JRPC 面:本地端口 ∈ {39440} 且路径非 `/zimo/jrpc`(且非 `/ping`) → 404;

* RESTful 面:本地端口 ∈ {39441} 且路径非 `/zimo/api/*`(且非 `/ping`) → 404。

门禁 advice 注册顺序保持在 SPA/重定向 advice **之前**,保证裁决唯一;`/ping`(基类默认,FR-23)三方均可达(旧版里 `/zimo/api/ping` 与全局 `/ping` 并存,路径不同无冲突)。

### 4.4 响应助手(FR-08/09/24)

| 助手                           | 行为                                                                                                                                      |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `JsonResponse(status, data)` | `newHttpJsonResponse(data)` + `setStatusCode`;成功为**裸 JSON**(与 auth.js 一致)                                                               |
| `ErrorResponse(status, msg)` | 统一错误包 `{error:{code,message}}`(与 auth.js 一致)                                                                                            |
| `JsonpResponse(req, data)`   | 显式助手:读 `req->getParameter("callback")`:有合法值 → `cb(json);`(Content-Type `application/javascript`);白名单 `[A-Za-z0-9_.]`(复用 `IsValidJsonpCallback`),非法值返回 400;无参数 → 常规 JSON                                                                   |

> 接口修正:v2.1 起 `JsonpResponse` 接收 `req`(FR-24"检测 query callback 参数"由基类完成,业务层只传数据)。
>
> **v2.5 自动识别型 JSONP(与主流 Koa/Spring 中间件语义一致)**:基类首个 `Open()` 经 once\_flag 注册一条全局 **PreSending advice**——规则收敛:请求为 GET 且带合法 `callback`(白名单 `[A-Za-z0-9_.]`,≤128)且响应 Content-Type 为 `CT_APPLICATION_JSON` 时,自动改写 `resp` 为 `cb(json);` + `CT_TEXT_JAVASCRIPT`;任一规则不满足 → 原样返回(不污染普通 REST/前端静态/WS 升级)。三面共享;开关 `SetAutoJsonp(false)` 可关闭(默认开)。<br/>**边界**:JSON-RPC 面(39440)为纯 POST 信封,天然不触发;访问日志(PostHandling)先于 PreSending 执行,记录的字节数为包装前大小。

* handler 未捕获异常由 Drogon `HttpBinder` 自动转 500(FR-08);404 页由 `SetNotFoundPage` 承载。

* 可选内存限流:基类提供 `CreateRateLimiter(type, capacity, timeUnit)`(封装 `drogon::RateLimiter`),业务层在 filter 内 `isAllowed()`;持久化限流留在业务层。

* **JRPC 信封注意**:39440 面是 JSON-RPC 信封(`{"jsonrpc":"2.0","error":{code,message},"id":...}`),**不复用** REST 的 `ErrorResponse`,由 `HttpJsonRpcServer` 单列自己的响应封装(§11.3)。

***

## 5. TLS(FR-10/11)

* **证书统一走全局** **`app().setSSLFiles(cert, key)`**;`AddListener(port, useSSL=true)` **不再传 per-listener cert**——因为 `reloadSSLFiles()` 只重载全局证书,per-listener 证书热加载不生效(头文件契约核实)。无证书则为 HTTP(80)。

* `ReloadCertificates()` → `app().reloadSSLFiles()`(热加载,换内容不换路径)。

* `SetTicketDisabled(true)` → 全局 `setSSLConfigCommands` 追加 `Options=-SessionTicket`(FR-11 开关,默认不启用)。

* **HTTPS 模式 80→443 重定向(FR-22,v2.1 定版)**:`HttpFrontendServer` 额外 `AddListener(80, false)`,并注册 PreRouting advice:本地端口 == 80 → 301 `https://{host}{path}`(**不采用** **`SecureSSLRedirector`** **插件**——它全局作用于所有非 SSL 监听,会把 39440/39441 的请求一并重定向;也不注册 `/{1}` 通配 handler——会串扰其他监听端口;一律经 `LocalPort()` 判定后走 advice)。

* **39440/39441 同步升级 HTTPS(v2.3 用户决策)**:有全局证书(`ZmHttpServer::Init` 的 Options 注入一份)时,三个面全部走 TLS——`HttpJsonRpcServer`/`HttpRestfulServer` 的 `SetupListeners(port, ip, useSSL=true)` 经 `AddListener(useSSL=true)` + 全局证书(热加载同 FR-10);无证书时回落 HTTP(保持旧行为)。业务期注意:客户端访问 39440/39441 改 `https://`、WebSocket 改 `wss://`;`/share` 302 目标协议随 `IsHttps()`。

***

## 6. 数据传输(FR-12/13/15)

### 6.1 双路径 + Hybrid(FR-12)

**Range 解析(方案甲与乙共用)**:`newFileResponse` 的 offset/length 重载**不解析** **`Range:`** **请求头**(头文件契约核实)。`SendFileCoro/SendFileStreamCoro` 内部统一解析:

* 合法单段 `bytes=a-b` → 206 + `Content-Range`;无 Range → 200 全文件;

* 多段 / 非法 → 416 `Range Not Satisfiable`。

**方案甲**:常规文件 → `newFileResponse(path, offset, length, setContentRange, dispName, ...)`。

**方案乙**:分块流式。**水位背压修正(头文件契约)**:

```
trantor AsyncStream::send() 返回 false = 连接已关闭,非"缓冲满"(AsyncStream.h 核实)。
→ 无法用 send() 返回值做字节级水位背压。
```

因此方案乙采用:

* **默认:定时器链节流(内存有界,v2.1 定版)**——`newAsyncStreamResponse` 的回调是普通函数,不能同步 while+runAfter,故实现为**定时器链状态机**(全部运行在事件循环线程,无锁):

```
回调(newAsyncStreamResponse 内,事件循环线程):
  stream 存入状态对象 St { stream(排他持有), 文件句柄/偏移, 剩余字节, lastSentBytes, lastSentTime }
  St::Next():读 chunkSize → stream->send(块) → 记录进度
              → stream->getLoop()->runAfter(interBlockMs, St::Next) 让发送与缓冲排水
              → 若 send() 返回 false(连接已关)或已发完 → stream->close() 结束
  停滞判定:每次 Next 检查"距上次 send 成功超出 stallAbortMs 且缓冲未空" → close()
  (真字节水位为可选增强:setHighWaterMarkCallback,复杂,由 O1 实测后定)
```

预读窗口 = 1 块(**内存有界**,不以字节计数,验收词相应放宽);块间延时与 `chunkSize` 可配(`SendFileStreamOptions` 增加 `interBlockMs`,默认 50ms)。

* **方案乙编码注记(头文件契约)**:`drogon::ResponseStream` 内部自做 chunked 帧(`send` 包装 `hex(len)\r\ndata\r\n`,`close()` 发 `0\r\n\r\n`),因此方案乙**不设 Content-Length**(Transfer-Encoding: chunked);`Content-Range`/206 头仍照常设置,客户端 Range 续传语义不变。

* **停滞放弃**:持续无进展 > `stallAbortMs`(默认 120s)→ `stream->close()`(客户端 Range 续传)。`onProgress` 可选回调。

```cpp
// 方案甲
resp = drogon::HttpResponse::newFileResponse(path, offset, length, true, dispName, CT_NONE, "", req);
```

> 注:v2.1 核实 `AsyncStream` 无 `getLoop()` 接口,定时器经 `drogon::app().getLoop()->runAfter()` 注册(IO 线程池首个 loop,回调不在连接线程——stream send 为线程安全,状态机仅在定时器回调与流回调内变动,无并发)。

**Hybrid**:文件 < `threshold`(默认 2GB)→ 方案甲;≥ → 方案乙。

### 6.2 流式(FR-13)

`MakeStreamResponse(cb, disableKickoff=true)` 为**静态工厂**,返回 `newAsyncStreamResponse` 的响应,调用方在返回的 `HttpResponsePtr` 上设置头部后再 `cb(resp)`。支撑音频流(订阅者线程 `stream->send`,trantor 线程安全)、zip 打包流、SSE。

* 音频流帧格式(len+seq+Opus 20ms)与 zip 分块逻辑均在业务层,仅把旧 `task->SendReplyChunk` 换成 `stream->send`。

* 现 `m_sseGone`/`m_tasksGone` 收尾防 UAF 纪律继续保留(stream 在业务线程 close 前需判活)。

### 6.3 上传(FR-15)

全局 `setClientMaxBodySize(10GB)`(单请求上限)+ `enableRequestStream(true)`;**流式接收封装(v2.4 定版,与代码一致)**:`RegisterStreamCoro(path, method, h, filters, maxBytes=0)`——业务协程 handler 携带框架注入的 `RequestStreamPtr`(内部绑定 Drogon stream-handler 三参回调 → `async_run` 桥接协程);`maxBytes` 为**路由级上限**(默认 0 = 不额外限制,全局 10GB 兜底):基类入口按 `X-File-Size` 自动早拒(超限 → null reader 丢弃 + 413,不进入业务),并写入 `req` attributes(`ZmStreamMaxBytes`)供业务落盘兜底取用;`SaveStreamToFile(stream, destPath, opts, &tooLarge)`:落盘状态机,块到即写 + `opts.maxBytes` 兜底(超限置 `tooLarge` 并清理半成品)。

***

## 7. 请求超时(FR-14)

连接级空闲超时框架内置;业务级 deadline 基类封装 `RegisterCoroWithDeadline`,内部**复刻旧版 TryReply 门**:

```cpp
void DrogonHttpServer::impl::runWithDeadline(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)> cb,
    CoroHandler h, size_t deadlineMs)
{
    auto gate    = std::make_shared<std::atomic<bool>>(false);  // TryReply 门
    auto connWk  = req->getConnectionPtr();                     // 弱引用守卫
    auto loop    = connWk.lock() ? connWk.lock()->getLoop() : app().getLoop();  // 连接所属 loop(v2.1:不用主 loop)
    loop->runAfter(deadlineMs / 1000.0,
        [gate, cb, connWk] {
            if (!gate->exchange(true)) {
                auto c = connWk.lock();
                if (!c || !c->connected()) return;   // 连接已关,不发
                cb(Response504());
            }
        });
    // 业务协程经 RunOnPool 等离核后,返回前:
    //   if (gate->exchange(true)) 丢弃;   // 已被超时占位
    //   auto c = connWk.lock(); if (!c || !c->connected()) 丢弃;  // 连接已关
    //   cb(resp);
}
```

**纪律**:原子门保证只回一次;弱引用 + `connected()` 保证晚到不碰已销毁连接;流式/下载端点不设死线。

***

## 8. 异步基础设施(FR-19)

### 8.1 RunOnPool

```cpp
template <typename T>
drogon::Task<T> DrogonHttpServer::RunOnPool(std::function<T()> fn)
{
    struct Awaiter : drogon::internal::CallbackAwaiter<T>
    {
        std::function<T()> fn_;
        void await_suspend(std::coroutine_handle<> h)
        {
            s_workPool.Submit([this, h] {          // 复用 ZmThreadPool(单一静态共享池)
                try { this->setValue(fn_()); }
                catch (...) { this->setException(std::current_exception()); }
                h.resume();
            });
        }
    };
    co_return co_await Awaiter(std::move(fn));
}
```

* 工作池为**单一静态共享池**(`RunOnPool` 为 static,三面共用,构造/析构随进程)。实现注:v2.1 修正——池大小不得经实例 `GetWorkPoolSize()`(static 方法无实例),改为**静态变量** `static std::atomic<size_t> s_workPoolSize{8}`,`SetWorkPoolSize(n)` 写入;池为函数局部 `static ZmThreadPool`,首次 `RunOnPool` 调用时按 `s_workPoolSize` 构造;**首次** **`RunOnPool`** **调用前**改变配置生效。

* **适用边界**:文件 I/O、PBKDF2 等纯阻塞;SQLite 走 Drogon ORM `execSqlCoro`,不占工作线程池。

* deadline 定时器放事件循环(§7 用连接所属 loop),不放工作池。

***

## 9. WebSocket(FR-16)

`RegisterWebSocket` 内部包 `registerWebSocketController`(单独一路)。**握手鉴权修正**:Drogon `handleNewConnection(req, conn)` 在升级时就能拿到**完整握手请求**(含 query sign/时间戳 + cookie)。因此:

* `WsCallbacks::onAuth(const drogon::HttpRequestPtr&) -> bool`:在 `handleNewConnection` 内先调 `onAuth(req)`,false → `conn->shutdown()` 拒绝升级;

* **无"接受前鉴权"钩子**(握手已在框架内完成),拒绝只能发生在升级回调里;

* `onMessage` 内可 `co_await`;`onClose` 收尾。

***

## 10. 可观测(FR-17/23)

* 访问日志:`ZmHttpServer::Init` 一次性注册一对 advice(PreRouting 记起始时间入 req attributes;PostHandling 结算耗时),单行 `PUBLIC_LOG_*` 输出(与运行日志同文件、按标签区分),覆盖 方法/路径/状态码/耗时/字节数/两端端口;不自建 access.log、不使用 AccessLogger 插件(v2.4 定版,与代码一致;v2.6 注册点从"首个 Open"移入 Init)。

* 健康检查:`/ping` 在 `ZmHttpServer::Init` 内默认注册 → `{"pong":true}`(FR-23)。

***

## 11. 派生服务器面详细设计(FR-25/D7)

### 11.1 三面总览

| 面                    | 端口     | 职责              | 关键注册                                                                                                                                         |
| -------------------- | ------ | --------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `HttpFrontendServer` | 80/443 | 静态 + SPA + 页面路由 | `SetDocumentRoot` + SPA 回落 advice + `SetNotFoundPage` + 页面别名 handler + `/share/{token}` 302 + HTTPS 模式 80→443 重定向(FR-22) + per-port 门禁(§4.4) |
| `HttpJsonRpcServer`  | 39440  | JSON-RPC        | `RegisterCoro("/zimo/jrpc", Post, ...)` + method 分发器(ping/-32700/-32601),**JSON-RPC 信封单列** + per-port 门禁                                     |
| `HttpRestfulServer`  | 39441  | ★ 业务 API        | 各业务模块 `RegisterCoro("/zimo/api/...", ...)` + `RegisterWebSocket` + **显式挂 CORS** + per-port 门禁                                                |

三者共享 `app()`;路由靠路径前缀区分;全局项(/ping/访问日志 advice/JSONP)由 `ZmHttpServer::Init` 一次性注册(去重纪律 §2.3)。

### 11.2 前端服务器 `HttpFrontendServer`(80/443)

**静态文件**:`SetDocumentRoot(wwwRoot)` 内置防目录穿越、MIME、Range;Cache-Control 语义保留:HTML 不缓存、JS/CSS 靠 `?v=` 破缓存(经 `SetStaticFileHeaders` 或页面别名 handler 按扩展名设置,见旧 [SendFile](file:///a:/ZiMo/ZiMoService/http_server_manager.cpp#L238-L257))。

**页面路由表(前端端口)**:

| 路径                                                      | 行为                                        | 注册方式                                                                  |
| ------------------------------------------------------- | ----------------------------------------- | --------------------------------------------------------------------- |
| `/`                                                     | `html/index.html`                         | `RegisterCoro("/", Get, ...)` → `newFileResponse`                     |
| `/login` `/register` `/reset` `/force-reset`            | 对应 `html/*.html`                          | 同上,逐条 handler                                                         |
| `/404`                                                  | `html/404.html`                           | handler                                                               |
| `/portal` 与 `/portal/*`                                 | SPA history fallback → `html/portal.html` | **SPA 回落 advice**(主方案,v2.1 定版;`setImplicitPage` 语义为"目录解析",不能做 SPA 回落) |
| `/share/{token}`                                        | 302 → RESTful 端口分享页                       | `RegisterCoro("/share/{1}", Get, ...)` → `newRedirectionResponse`     |
| `/html/*` `/css/*` `/js/*` `/resource/*` `/favicon.ico` | 物理静态文件                                    | `SetDocumentRoot` 直接命中                                                |
| `/doc/*`                                                | **不可达**(404)                              | 前端面 advice 拦截 `/doc/*`(www 下物理存在,必须显式封禁)                              |
| 其余未匹配                                                   | `html/404.html`(状态码 404)                  | `SetNotFoundPage("html/404.html")`                                    |

**前端面 advice 链(v2.1 定版,注册顺序):**

```cpp
// 1. 80→443 重定向(HTTPS 模式):本地端口 == 80 → 301
// 2. per-port 门禁(§4.4):本地端口 ∈ {80,443} 且路径以 /zimo/ 开头 → 404
// 3. SPA 回落:本地端口 ∈ {80,443} 且路径以 /portal(/../) 开头 → cb(portal.html)
// 4. /doc/* 封禁:路径以 /doc/ 或 == /doc 开头 → cb(404)
// 放行规则统一:cc() 交 document root / 404
```

> 当前 `www` 下无物理 `/portal` 目录,`/portal/*` 全部回落 portal.html(与旧 PortalModule 语义等价);advice **按本地端口 + /zimo/ 前缀双重要求**,39441/39440 不受影响。

**防穿越**:`SetDocumentRoot` 自带路径规范化与穿越防护;若业务层自行解析物理路径,沿用旧 `GetFullPathNameW` + 根包含校验逻辑。

### 11.3 JSON-RPC 服务器 `ZmHttpJsonRpcServer`(39440)

> v2.3:根路径默认 `ZM_HTTP_JRPC_SERVER_ROOT_URI`(`/zimo/jrpc`),可经 manager Init `rootPath` / `SetRootPath` 自定义;门禁与 handler 注册同源。本节与下节的 `/zimo/*` 写法均为默认值示意。
>
> **v2.4:协议校验下沉平台面**——参考旧版 `ZmJsonRpcServer::OnHttpdRequest` / `JrpcRequestReadCB` 语义:单 handler 内建 JSON-RPC 2.0 校验(-32700 Parse error / -32600 Invalid Request:非对象、jsonrpc!="2.0"、缺 id、method 缺失或非字符串 / -32602 Invalid params / -32601 Method not found / -32603 handler 异常兜底),信封单列、HTTP 恒 200;业务层经 `RegisterMethod(name, fn)` 注册处理器(`bool fn(params, result, error)`,成功写 result、失败写 `error{code,message}`);`ping` 平台内建。

Drogon 无内置 JRPC,注册**单一协程 handler**,内部按 `method` 字段分发(复刻旧 `JrpcRequestReadCB` 骨架):

```cpp
srv.RegisterCoro("/zimo/jrpc", Post,
    [](const HttpRequestPtr& req) -> Task<HttpResponsePtr> {
        Json::Value body;   // req->getJsonObject() 解析失败 → -32700
        if (解析失败) { 回信封 { jsonrpc:"2.0", id:null, error:{code:-32700, message:"Parse error"} }; }
        std::string method = body.get("method", "").asString();
        Json::Value rsp; rsp["jsonrpc"] = "2.0"; rsp["id"] = body["id"];
        if (method == "ping")      { rsp["result"] = Json::objectValue; rsp["result"]["pong"] = true; }
        else                       { rsp["error"]["code"] = -32601; rsp["error"]["message"] = "Method not found: " + method; }
        co_return HttpServerBase::JsonResponse(200, rsp);   // 注意:JRPC 信封单列,不复用 REST ErrorResponse
    });
```

* 请求/响应信封 `jsonrpc`、`id` 字段按 JSON-RPC 2.0 保留;

* 方法表扩展:仅在分发器加分支(现仅 `ping`)。

### 11.4 RESTful 服务器 `ZmHttpRestfulServer`(39441)

> v2.3:根路径默认 `ZM_HTTP_RESTFUL_SERVER_ROOT_URI`(`/zimo/api`),同上可自定义。

**注册策略**:显式注册,路径保留 `/zimo/api` 前缀(省去旧"剥根前缀"步骤)。每个业务模块新增 `RegisterDrogonRoutes(HttpServerBase&)`,内部逐条 `RegisterCoro`;旧 `ServicePortal::RestfulRequestCB` 分发链拆散为各模块路由注册,调用顺序等价性保留:auth → filehub → audio → portal → ping。

**路由表(前缀** **`/zimo/api`)**:

* 认证 `/auth/`(免登录):POST `/auth/login` `/auth/register` `/auth/reset` `/auth/complete-change` `/auth/logout`;GET `/auth/me` `/auth/heartbeat`。

* 门户 `/portal/`:GET `/portal/info`;GET `/portal/userManager`(分页);GET `/portal/userManager/{id}`;POST `/portal/userManager/{id}/{action}`。

* 文件中心 `/portal/filehub/`:GET `/list` `/search` `/download` `/shares` `/tasks` `/task_status` `/zip_task_download`;POST `/upload` `/mkdir` `/rename` `/move` `/copy` `/delete` `/zip` `/share` `/unshare` `/task_create` `/task_cancel` `/task_delete` `/zip_download` `/share_commit` `/share_cancel`;POST `/portal/filehubAdmin/sync`(手动一致性)。`zip_download` 走 query(`task_id`+`ids`),迁移后保持 query 语义(见旧 [HandleZipStart](file:///a:/ZiMo/ZiMoService/modules/module_file_hub.h#L233-L234))。

* 音频 `/portal/serverAudioStream/`:GET `/stream`(流式,§6.2);GET `/status`(采集快照)。

* 系统:GET `/ping` → `{"pong":true}`。

**鉴权 helper(替代各模块** **`AuthAndTouch`** **前置块)**:

```cpp
// net/rest_util.h —— 统一鉴权 + 模块权限(语义对齐旧 UserModule::RequireModule)
// 返回 false 时已写好 401/403 响应
bool RequireModule(const HttpRequestPtr& req, HttpResponsePtr& resp,
                   const char* moduleCode /* 可空 */, UserModule::UserInfo* outUi);
```

* 读 `req->getCookie("zm_session")` → DB 会话校验(现有逻辑原样);会话失效 → 401,模块未授权 → 403;

* **在每个 handler 内显式调用**(与现风格一致);也可抽成 `AuthFilter` 挂到路由组(§4.2),二选一,保持一致性即可。

**CORS + 凭据**:由业务层显式挂到 `HttpRestfulServer`:`RegisterPreSending` advice 追加 `Access-Control-Allow-Origin`(回显 Origin)+ `Access-Control-Allow-Credentials: true` + OPTIONS 预检(复用 Drogon `HttpOptionsMiddleware` 或自写);配置项与现 CORS 白名单一致。

### 11.5 会话与鉴权映射

* **不引入** Drogon 内置 session;沿用自研 `zm_session` cookie + 会话表(PBKDF2、LRU、锁定、限流全在业务层,已与 HTTP 栈解耦)。

* 旧 `AuthAndTouch` 接收 `ZmHttpdTask*` → 迁移后接收 `HttpRequestPtr`/`HttpResponsePtr`:

  * 读:`req->getCookie("zm_session")`

  * 写 `Set-Cookie`:`resp->addHeader("Set-Cookie", ...)`(登录/登出/心跳续期)

  * cookie 属性(Path/Max-Age/SameSite/Secure)原样保留。

### 11.6 模块接口改造映射

| 现有接口                                                         | 迁移后                                                                            |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| `XxxModule::DispatchRest(loop, verb, path, task, body, len)` | `XxxModule::RegisterDrogonRoutes()`(逐条 `RegisterCoro`)+ 内部 `Handle*(req, ...)` |
| `XxxModule::RegisterHttpRoutes(HttpServerManager*)`(80 端口)   | 并入 `HttpFrontendServer::RegisterRoutes()`(§11.2 表)                             |
| `ServicePortal::RestfulRequestCB`(分发链)                       | 拆散为各模块路由注册(§11.4),保留顺序等价性:auth → filehub → audio → portal → ping               |
| `ServicePortal::JrpcRequestReadCB`                           | `HttpJsonRpcServer::RegisterRoutes()`(§11.3)                                   |
| `ZmReqLoopRest::ResponseJson/ResponseError`                  | `rest_util.h` 响应助手(§4.4)                                                       |
| `ZmReqLoopJrpc::ResponseJson`                                | `HttpJsonRpcServer` 信封封装(§11.3)                                                |

***

## 12. 构建与依赖

| 项   | 说明                                                                                                               |
| --- | ---------------------------------------------------------------------------------------------------------------- |
| 链接库 | `drogon.lib` + `trantor.lib` + 传递依赖(`jsoncpp`/`brotli*`/`cares`/`openssl`/`zs`/`lz4`),来自 `ZiMoPublic\drogon\lib` |
| CRT | `/MT`,与 x64-windows-static 一致                                                                                    |
| 符号  | 无自编译 sqlite3 → **Drogon ORM 可链接**(drogon.lib 的 sqlite3 后端直接使用);OpenSSL 统一用 Drogon 的 `libssl/libcrypto`           |
| 头文件 | 增加 `ZiMoPublic\drogon\include` 已就绪                                                                               |

***

## 13. 验收

### 13.1 验收映射(需求 → 设计)

| 需求           | 设计落点                           |
| ------------ | ------------------------------ |
| FR-01\~04    | §2.2/§3 生命周期协调/相位契约/优雅关闭       |
| FR-05\~09、24 | §4 路由/filter/advice/响应助手/JSONP |
| FR-10\~11    | §5 TLS(证书全局化保证热加载)             |
| FR-12\~15    | §6 文件传输(Range 内部解析)/水位修正/流式/上传 |
| FR-14        | §7 deadline + TryReply 门       |
| FR-16        | §9 WebSocket(onAuth 收完整请求)     |
| FR-17、23     | §10 可观测(Init 一次性注册)         |
| FR-18        | §3.1 压缩接口                      |
| FR-19        | §3.1 工作池 + §8 RunOnPool        |
| FR-20\~22    | §11.2 前端面 + CORS + 重定向         |
| FR-25        | §2/§11 派生结构                    |

### 13.2 行为验收清单(与旧版一致)

1. 三个端口(80/443、39440、39441)行为与迁移前一致:路径、状态码、响应体、cookie、CORS 头
2. 前端:静态文件 / 页面别名 / SPA 刷新回落 / 自定义 404 / `/share/{token}` 302 均正常;`doc/` 目录不可达
3. JRPC:`POST /zimo/jrpc` 的 `ping` 返回 `{"pong":true}`;非法 JSON 返回 -32700;未知 method 返回 -32601
4. RESTful:auth 全端点、portal 信息/用户管理、filehub 全端点(含 Range 206、>10GB 大文件、zip 打包流)、音频流、`/ping` 全部通过
5. 鉴权:会话失效 401、模块未授权 403、`zm_session` cookie 读写一致
6. 线程纪律:handler 内无阻塞操作(代码评审);文件中心/音频/登录在高并发下无事件循环卡顿
7. 生命周期:Stop 无崩溃、无泄漏;证书热重载生效
8. 关闭顺序:在飞请求 graceful 收尾,无 UAF/崩溃(回归现内存纪律)
9. JSONP:带 `callback` 返回 JS 包装、非法名拒绝、无 callback 走常规 JSON
10. 派生:三服务器面独立实例化/注册/状态查询,共享 app() 不串扰

***

## 14. 分阶段实施计划

| 阶段           | 内容                                                                   | 产出                  |
| ------------ | -------------------------------------------------------------------- | ------------------- |
| P0           | vcxproj 增加 drogon 头文件/库目录 + 链接项;最小 `app()` 引导跑通 80 端口                | 可编译、可 curl 通        |
| P1           | 前端服务器:document root + 页面别名 + SPA 回落 + 404 + 分享 302                   | 现有前端页面完整可用          |
| **P1.5(本期)** | **三面 + 测试接口**:JRPC 信封、REST 测试组、WS echo、Range/deadline 验证;业务路由**不接入** | 三端口测试接口全绿           |
| P2           | RESTful 基础:响应助手 + 鉴权 helper + CORS + auth/portal 路由                  | 登录/门户/用户管理可用        |
| P3           | RESTful 文件中心:filehub 全路由 + 上传/下载(Range)/zip 流式                       | 文件中心可用              |
| P4           | RESTful 音频 + JSON-RPC:stream/status 流式 + `/zimo/jrpc` 分发器            | 远程音频 + JRPC ping 可用 |
| P5           | 收尾:证书热载、关闭顺序、压测(线程模型纪律)、广播回归                                         | 全量回归通过              |

***

## 15. 改动清单(v2.6:生命周期静态化重构)

1. 新增 `net/drogon_http_common.h`(公共类型 + RunOnPool 声明)
2. 新增 `net/drogon_http_server.h/.cpp`(具体基类 + 静态生命周期 Init/Open/Close + Options + 去重 + 响应助手 + advice 挂点)
3. 新增 `net/http_frontend_server.h/.cpp` / `net/http_jsonrpc_server.h/.cpp` / `net/http_restful_server.h/.cpp`
4. 新增 `net/rest_util.h`(响应助手别名 + `RequireModule` 声明)
5. **重建**既有空壳 `net_dock.h/.cpp`(持有三 Manager,`Init` 内先 `ZmHttpServer::Init(opts)` 全局一次再配置三面,Open/Close 转发静态,暴露 GetXxxServer)与 `http_server_manager.h` / `http_jsonrpc_manager.h` / `http_restful_manager.h`(.cpp)(各持有并**仅配置**本面服务器,无生命周期委托)
6. `service_center.cpp`:`OnStart` 建 NetDock → `NetDock::Init()`(全局 Init + 三面配置)→ 建 Portal(注册路由,Phase1)→ `NetDock::Open()`(静态);`OnStop`:`Portal.Shutdown()` → `NetDock::Close()`(静态)
7. `service_portal.h/.cpp`:构造时经 NetDock 取三面 server 引用注册路由(本期测试路由);`Shutdown()` 业务收尾
8. `ZiMoService.vcxproj`:接入 drogon 库目录 + `DROGON_STATIC_DEFINE/TRANTOR_STATIC_DEFINE` + 链接项(drogon/trantor/jsoncpp/cares/lz4/sqlite3/libssl/libcrypto),移除旧 `libcrypto_static/libssl_static` + 新增文件

