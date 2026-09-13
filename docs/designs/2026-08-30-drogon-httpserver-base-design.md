# ZmHttpServer 基类设计文档(含三个服务器面详细设计)

> 状态:已实施 · 版本:v2.19 · 日期:2026-09-12
> 范围:`ZmHttpServer` 基类(平台层,`ZiMoPublic/net`)+ 三个派生服务器面(前端/JRPC/RESTful)的详细设计
> 需求:基于 `2026-08-30-drogon-httpserver-requirements.md`(25 条 FR + D1\~D7)
> 依赖:Drogon 1.9.13(头文件 + 静态库在 `ZiMoPublic\drogon`)、`ZmThreadPool`(`zm_util_thread.h`)、Drogon ORM
> 修订:v2.6 生命周期重构(用户决策,对齐 drogon 单次 run 硬约束):实例级 Open/Close/BootCoordinator 引用计数 → **进程级静态状态机** `Uninit→Initialized→Opened→Closed`;全局参数收敛进 `ZmHttpServer::Options` 经 `Init(opts)` 一次性注入;全局 advice(/ping/访问日志/JSONP)从"首个 Open 经 once_flag"改为 `Init` 内注册;派生面收敛为"端口+路由登记"(删除实例 Open/Close/IsOpen);三个 Manager 与 NetDock 生命周期委托同步删除。运行期唯一可热更新能力:证书 `reloadSSLFiles()`;不支持运行期单端口启停/热重启(重启须进程级)。
> 修订:v2.1 评审联动:①SPA 回落改用 advice(setImplicitPage 语义为"目录解析",非 SPA 回落,头文件核实);②补四个 advice 挂点到基类接口;③新增 per-port 门禁纪律(全局路由表下恢复旧"端口隔离"行为);④AccessLogger 经 `loadConfigJson` 注入最小配置;⑤方案乙改为定时器链驱动;⑥deadline 定时器放连接所属 loop;⑦AddFilter/RegisterCoro 顺序赋约束;⑧JsonpResponse 改收 req;⑨命名统一 DrogonHttpServer;⑩宿主分层(NetDock/Manager/Portal)并入 §2.5(本期交付 = 基类 + 三面 + 测试接口,业务路由延后);v2 并入原 network-layer-design 的三面细节并修正其过时说法;v1.1 根据代码评审修正(共享 app() 协调、水位机制、证书热加载、Range 解析、Filter 适配、WS onAuth、去重纪律、基类形态);v2.4(还原恢复)流式接收定版:`RegisterStreamCoro` 增 `maxBytes` 路由级上限(X-File-Size 早拒内置 + attributes 透传),`SaveStreamToFile` 落盘助手;全局上传上限 4GB→10GB(FR-03/FR-15);FR-17 访问日志 = PreRouting/PostHandling advice(`PUBLIC_LOG_*` 单行),弃 AccessLogger 插件;v2.5 JSONP 改自动识别型:全局 PreSending advice(GET + 白名单 callback + JSON 响应 → 自动包装),白名单抽 `IsValidJsonpCallback` 复用,开关 `SetAutoJsonp`(默认开),`JsonpResponse` 保留显式通道;v2.6 一对象一端口:`AddListener` 单次设置(重复报错忽略),`GetPorts/GetBindIps` 删、改 `GetPort()/GetBindIp()`,`IsHttps()`=监听 useSSL;前端 HTTPS 模式拆两实例(ZmHttpFrontendServer redirectOnly);Manager 持 primary+redirect;门禁按单端口比较;v2.7 关闭语义修正:FR-04"在飞请求 graceful 收尾"边界——drogon 无 drain API,`quit()` 后挂起协程不再调度,在飞 HTTP 由业务层保障(守护线程 join/断点),验收清单同步
> 修订:v2.9(2026-09-12 评审 P1,已实施并实测)路由归属机制:新增 §4.6 —— root 三态(前缀 / 空 / 兜底 `/`)+ **R1** 业务路由归属校验(跨面注册拒绝)+ **R2** 平台路由必须归属具体面(`/ping` 经 `MarkShared` 声明共享;`metricsPath` 游离 → **Open() 拒绝启动**)+ **R3** 运行期 `[ROUTE-LEAK]` 归属网;前端门禁外来前缀由宿主手工登记改为**按归属表推导**(`AddOtherRootPath` 降级为兜底口子);`/ping` 名单单源化(去除各面硬编码)。背景:原 `/zimo/metrics` 从公网前端端口泄漏(commit c7b5814);实测中另发现并修复"业务前端页面 advice 无端口判定,把 `/ping` 在 39440/39441 一并 302 到 /login(FR-23 名存实亡)"。
> 修订:v2.10(2026-09-12 评审 P2,已实施并实测)业务/平台边界收敛:新增平台响应助手 **`FileResponse`/`RedirectResponse`/`NotFoundResponse`**(业务层不再构造 drogon 响应 —— 业务出现 `drogon::HttpResponse::new*` 即越界,§2.4);**页面响应条件请求补齐**(实测 `/login` → 200+ETag/Last-Modified,+INM → **304**;此前 §16.1.1 宣告的页面 304 因落在无调用者的平台 SPA 分支上而未生效);**删除平台侧重复实现** `AddSpaFallback`/`m_spaFallbacks`(页面/SPA 策略归业务 module_gate,平台只留 docroot 策略 `AddDeniedPath`);§11.2 页面路由表改为「行为契约 / 实现位置」双列。
> 修订:v2.11(2026-09-12 评审 P3,已实施并实测)路由注册双路径:新增 **`RegisterCoroWithPathParams`**(路径参数写形参 → drogon **原生绑定**,不再走正则;arity==0 编译期 `static_assert`、路径无 `{N}` 运行期拒绝);**更正 §4.1 根因**——`{1}` 路径的 `exit(1)` 源于类型擦除致 `paramCount()==0`(`HttpBinder.h:217` + `HttpControllersRouter.cc:368`),**不是"访问违例"、不是本捆绑缺陷**;admin 模块 9 条带参路由迁至新入口,**删除 `UidOf` 的手写路径解析**(其 `getParameter("1")` 分支恒为空、marker 硬编码、解析失败静默返回 0);实测 9 条路由全部正确绑定(非法 uid→400、不存在→404、嵌套参数→403 业务守卫),另修复既有缺陷 **DEF-5(2026-09-12)**:`HandleRole`/`HandlePermissions` 缺存在性校验(对不存在的 uid 谎报成功:role→200、permissions→400)→ 补 `FindByUid` → **404 USER_NOT_FOUND**,同模块 9 个带 uid 的 handler 全数核查通过;实测:不存在 uid→404、正常改角色/授权→200、自身操作→403、等级压制→403/400 全部符合预期。
> 修订:v2.12(2026-09-12 评审 P4,已实施)**文档与代码对齐**(纯文档,无行为变更):§3.1 类摘要按 `zm_net_http_server.h` 重写(补齐 5 个 `Options` 字段:`nonStreamBodyLimit`/`clientMaxMemoryBodySize`/`maxConnectionsPerIP`/`metricsPath`/`corsAllowedOrigins`;补 P1–P3 新增接口;删不存在的 `SetTicketDisabled`/`SetImplicitPage`,标出 `AddOtherRootPath` 属前端面);标题与 §2.5/§7/§11/§15 更正文件名与类名(`DrogonHttpServer`→`ZmHttpServer`、`drogon_http_common.h`/`drogon_http_server.h`/`net/rest_util.h`/`http_server_manager.*` 均不存在或已并入);§11.4 鉴权 helper 按实现改写为 `ZmAuthGateModule` 三层(`SetupRestfulGate`/`Authorize`/`CheckCtxSync`,`RequireModule` 未采纳);§11.3/§11.4/§14 标注**实施状态**(filehub/音频/JRPC 业务未实施 —— 文档此前把设计目标写成了现状)。
> 修订:v2.13(2026-09-12 P6,已实施并实测)自动 JSONP 改为**机制全局 + 授权逐路由 + 行为差量**:新增 `ZmJsonpOptions`(全局基线:`paramNames`/`wrapErrors`/`maxBodyBytes`/`enabled`)与 `ZmJsonpOverride`(**optional 差量**:未设置 = 继承基线)、`SetJsonpEnabled(prefix, over)`(**前缀匹配**,兼容 `{N}` 路由);删除全局开关 `SetAutoJsonp`。默认保持 `enabled=true`(观察期,**避免未声明的老客户端静默失效**),对未声明却被包装的路由按路径打一次 WARN;启动期打印各声明的合并选项。**不做逐路由**:回调名字符集(安全边界)、包装语法、默认姿态。实测:未声明路由 `callback` 包装生效且告警仅 1 次;`paramNames`/`wrapErrors`/`maxBodyBytes` 三项覆盖均生效(含 `{N}` 路径前缀命中);非法回调名/静态资源不受影响。
> 修订:v2.14(2026-09-12 P6 收尾,已实施并实测)deadline 收尾优化:超时状态 `ZmDeadlineState` **共享**给定时器,响应回调在发出响应那一刻置空(**不再被定时器按值捕获而滞留到 deadlineMs** —— 该回调持有连接与请求对象含 body);业务先完成时 `invalidateTimer(tid)`(注意 trantor 语义:取消只删 id,对象仍留到到期,**收益主要是释放 cb**);新增 `deadlineMs==0` 注册期守卫(否则 runAfter(0) 会让每个请求立即 504);§7 同步更新。实测(?ms=50 / deadline 1000ms):cb 在响应时刻(0ms 延迟)释放并注销定时器 tid;慢 handler(3s/1s)→ 504 @1.21s;守卫 → 注册期 ERROR + 路由 404。
> 修订:v2.15(2026-09-12 用户决策,已实施并实测)**移除手写指标端点**:删除 `Options.metricsPath`、`Init` 中的注册块、全部进程级计数(`s_metricTotal`/`2xx-5xx`/`inflight`/`lat[4]`/`s_startupEpoch`)及 PreRouting/PreSending 中的计数更新(访问日志与请求 ID 完整保留,实测 `X-Request-Id` 与日志行首 id 正常)。理由:无消费者 + 位于受 RESTful 会话门禁约束的 `/zimo/api/metrics`(无 cookie → 401 → **监控无法抓取**)+ 无 label/无历史。需要监控时改用 `utils/monitoring` + `PromExporter`(§16.3)。连带:§4.6 R2 闸门(`DeferPlatformRoute`)失去唯一调用者,保留为后续平台路由的归属闸门(头文件注释已标明);R2 错误提示纠正为"**仅 MarkShared 不满足本闸门**"(此前"或改用 MarkShared"具误导性)。
> 修订:v2.16(2026-09-12 用户决策,已实施并实测)**移除 R2 平台路由闸门**:删除 `DeferPlatformRoute` + 入队结构 + Open 期"平台路由必须归属具体面"校验(其唯一调用方为手写指标端点,已于 v2.15 移除);Open 期仍保留 ①root 声明冲突 ②已登记路由归属复检 + 只读快照固化 + JSONP 授权快照。R1(业务路由注册期归属校验)与 R3(运行期 `[ROUTE-LEAK]` 归属网)不变。**替代约束(写进文档,靠约定而非机制)**:新增平台路由须自行确保落在某个面的 root 下或 `MarkShared` —— 否则会在所有端口可达(含公网前端);R3 会在它被实际服务时告警,但**只告警不拦**。实测:启动日志不再有"平台路由 … 已注册"行、`/ping` 三面 200、admin/portal 200、metrics 404。
> 修订:v2.17(2026-09-12 用户决策,已实施并实测)**CORS 白名单移出 `Options`**:`Options.corsAllowedOrigins` → **`SetCorsAllowedOrigins(vector)`**(Open 前声明,运行期 ERROR 拒绝;与 `SetJsonpDefaults`/`SetRootPath` 同款)。至此 `Options` 回归**纯传输/框架参数**(17 项),业务策略一律经 `SetXxx` 声明 —— 原先"传输参数与业务策略混装"的问题从结构上消除(不再需要注释分组)。顺带**修正语义描述**:白名单是**跨站**许可表,**同站跨端口**(Origin 与 Host 同 host)由业务侧硬逻辑放行、不经名单;空名单 = 只放行同站跨端口(原文"空 = 拒绝一切跨域"不准确)。实测三条路径:同站 → 200 + ACAO;仅靠名单(`http://127.0.0.1` vs Host `localhost:39441`)→ 200 + ACAO;`https://evil.example` → 预检 403 且响应无 ACAO。
> 修订:v2.18(2026-09-12 用户决策,已实施)**移除 JSON-RPC 面的内建 `ping` method**:`ZmHttpJsonRpcServer` 构造不再注册任何 method(改 `= default`),业务经 `RegisterMethod` 注册;HTTP 层的存活探针沿用全局 `/ping`(共享路径,三面可达)。§11.1/§11.3/§13.2 同步。
> 修订:v2.19(2026-09-12 用户决策,已实施)JSONP 逐前缀**例外**:`ZmJsonpOverride` 增 `std::optional<bool> enabled`(未设置 = 继承基线,声明默认启用),`SetJsonpEnabled(prefix, {enabled = false})` 可把某前缀在全局 `enabled=true` 的观察期下单独关掉 —— 此前只能"全局翻 false + 逐条正面声明"。替代旧手法:把 `paramNames` 差量置空(靠"候选名一个都不命中"绕行),该写法无文档、无日志且极易写错 —— `o.paramNames = {}` 在 `std::optional` 语义下是**重置为未设置**(实测 MSVC:has_value=0),静默回到继承基线,须显式写 `std::vector<std::string>{}`。判定序在命中声明前缀后补 `enabled` 检查(例外前缀同时免打"未声明"观察期告警,它是显式决策);启动日志行改「JSONP 声明」并补 `enabled=`。v2.13"不做逐路由默认姿态"的边界据此澄清:全局基线只决定**未声明**路由的姿态,例外是逐前缀的显式决策。验证:已按目标形态落地并实测(见 §4.7 落地状态) —— 未声明路由带 `callback` → 裸 JSON;`/ping` 声明后 → `cb(...)`(39441 与 443 两面一致);`/ping` 声明为例外 + 全局开 → 裸 JSON,同刻未声明的 `auth/me` 照旧被包(例外分支被真实覆盖);非法回调名 → 裸 JSON;启动日志打印 `enabled` 位。
> 修订:v2.20(2026-09-13 评审 P7,已实施并实测)正确性与边界收口:**① 方案乙状态机归位连接 loop** —— 原定时器与读回执投 `app().getLoop()`,导致跨线程读写非原子 `bytesSent_`(`TcpConnectionImpl.h:262`;x64 对齐读实际良性,但严格意义是未定义行为),且所有并发下载的块发送串行挤在主 loop;改为 `Run()` 时由 `connWk` 定型 `m_loop`(取不到退回主 loop,与 `ZmDeadlineState` 同款取值顺序),此后全部回执与定时器投连接 loop —— 状态单线程、停滞判定同线程读写、主 loop 不再承担分块发送。**② `RegisterCoroWithPathParams` 增占位符编号校验** —— 编号越界(`N > arity`;协程特化的 arity **不含** `HttpRequestPtr`)/编号 0/重复编号/超长编号(会让 drogon 的 `std::stoi` 抛未捕获异常)一律注册期 ERROR + 拒绝注册,此前这些形态会被 drogon 的 `addHttpPath` 直接 `exit(1)` 杀进程(`HttpControllersRouter.cc:368-383`);守卫自此真正兑现"用错就失败得早"。**③ 路由归属冲突改为按当前声明重算** —— 删掉只增的 `s_ownerConflicts`,改 `s_rootClaims`/`s_portClaims`(一对象一条,覆盖式写入)+ 由声明表重建归属表,Open 期分别按当前声明重算 root 冲突与**端口冲突**(原只有 root;端口侧另有"重登记不释放旧端口"的残留),改掉声明即自愈 —— v2.9 起"修正配置后可重试 Open"至此名副其实。**④ 运行期只读纪律补齐** —— `RegisterMethod` 增 run 后拒绝守卫(与 `RegisterCoro` 同款);`SetNotFoundPage` 路径改 UTF-8 → wide(原窄串经 filesystem 按 ANSI 解码,中文安装路径下静默降级为框架默认 404 页;drogon 读文件自身经 `toNativePath` 转 wide,故校验一处即可)。**⑤ 限流专项桶改为挂在 overlay 规则上**(`ZmQuotaSlot` 随规则存亡、桶懒建、算法由首建者定型、参数未变则沿用槽),删除 `Impl` 内无界的 `quotaBuckets` 与死变量 `quotaCount`(原注释称"与 maxEntries 共用上限"但从未实现;专项桶也不该有驱逐 —— 驱逐等于重置额度放行一波),`Check` 命中专项规则时稳态不再取锁。**⑥ 上传成功补发终态进度** —— 原 `written == len` 判据不成立(读的是累计落盘量 vs 当前块长),业务实际收不到 100%;改由写线程回执带回落盘总量、成功路径在 `done` 之前报满量,并把 `onProgress`/`done` 回调线程由主 loop 统一到**请求所属 loop**(与 `RunOnPool` 的取值一致)。**⑦ 方案乙读盘异常留痕** —— 读失败记 ERROR(+`GetLastError`)与"提前读到文件尾"记 WARN;两条路径都按"发完"收尾(流正常关闭),纯 200 无 Content-Length 客户端无从察觉、206 可由 `Content-Range` 判定 —— 这是文档化的边界,不再是无痕静默。**⑧ 可观测收口** —— `X-Request-Id` 属性缺失时补 `zm-unknown` 且与访问日志行首共用同一取值(对齐同函数内 `ZmAccessStartMs` 的既有防御),并记录"`RecordAccessStart` 必须是首个 PreRouting advice"的顺序不变式(短路 advice 的响应仍经 PreSending 结算,位置后移即出现空 ID);框架层解析期拒绝(如超大 Content-Length)根本不走 advice 管道,**没有**该头与安全头属既有边界。另清理头文件错位/陈旧注释(悬空 `///<`、错挂在 `s_corsOrigins` 上的 JSONP 校验块),把回调名白名单契约归位到 `ZmJsonpOptions::paramNames`。实测:9 条 admin 路由注册正常、归属快照固化(面 3/共享 1/端口 4)、`/ping` 三面 200 与 JRPC 信封正常;方案乙 5MB+12345(5×1MB+尾块)下载字节级一致、16MB 流式上传进度末笔为全量且落盘 sha256 一致;源文件被写入者独占期间下载返回 **200 + chunked + 0 字节**(客户端无从察觉)且服务端 ERROR 精确落点;`X-Request-Id` 在正常 200 与"我方闸门 413(PreRouting 短路)"两类响应上均为真实 ID。

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

* **一对象一端口(v2.6)**:每个服务器面对象仅绑定一个监听——`AddListener` 单次设置(重复设置报错忽略);查询 `GetPort()`(未设置=0)、`GetBindIp()`;`IsHttps()` = 本面监听 useSSL;多端口面用多个实例(前端 HTTPS 模式 = 443 完整实例 + 80 重定向实例)。

* 关闭顺序(FR-04):业务线程先 join → 最后 `ZmHttpServer::Close()` 触发 `quit()`+join。

* **在飞 HTTP 语义(v2.7 修正,drogon 无 graceful-drain API)**:`quit()` 后事件循环不再调度回调——挂起协程(await RunOnPool/上传/打包流)直接丢弃,`RunOnPool` 工作池不 join(进程退出中止)。凡业务上需"跑完"的工作须走**业务层自保障**:守护线程 `Shutdown()` join,或可中断+断点(上传落盘临时文件后 rename);框架只保证"停得干净"(无崩溃/UAF/线程 join)。

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

* **ServicePortal** 职责:路由注册与 handler 业务逻辑(全部)。**业务/平台边界(v2.10 精确化,替代原"不感知 Drogon API"口号)**:业务可持有 drogon 的**值类型与协程类型**(`HttpRequestPtr`/`Task`,它们是 ORM 与 handler 签名的通用货币,封杀无收益);但**响应构造与缓存/条件请求语义必须经平台助手**(`JsonResponse`/`ErrorResponse`/`FileResponse`/`RedirectResponse`/`NotFoundResponse`),业务层出现 `drogon::HttpResponse::new*` 即越界(可 grep 检查)。理由:`module_gate.ServeIndex` 曾自行 `newFileResponse` 组装页面,漏掉 Last-Modified/ETag/304 → §16.1.1 宣告的页面 304 在真实路径上不生效,平台侧另有一套 SPA 回落(无调用者)形成双实现 —— 机制必需单源。

* 三个面均无业务可挂时(本期测试),仅注册测试路由;`RegisterRoutes()` 纯虚仍由各派生面实现(实现为"本面内置路由 + 测试路由")。

### 2.5 文件划分

> v2.3 迁移(用户决策):基类与三个服务器面为平台能力,按现有命名规范下沉 **`ZiMoPublic\net`**(`zm_net_*` / `Zm*` / `ZM_NET_*_H`);服务工程仅保留宿主层。

| 文件                                   | 位置             | 内容                                                                       |
| ------------------------------------ | -------------- | ------------------------------------------------------------------------ |
| `zm_net_http_server.h/.cpp`          | ZiMoPublic/net | 基类 `ZmHttpServer`(静态生命周期 Init/Open/Close + Options + 响应助手 + RunOnPool + advice 挂点 + 公共类型) |
| `zm_net_http_frontend_server.h/.cpp` | ZiMoPublic/net | 前端面 `ZmHttpFrontendServer`:docroot 静态 + 自定义 404 + 路径封禁(`AddDeniedPath`)+ 80→443 重定向;**页面/SPA 归业务层 advice**(§11.2,v2.10) |
| `zm_net_http_jsonrpc_server.h/.cpp`  | ZiMoPublic/net | JRPC 面 `ZmHttpJsonRpcServer`:`/zimo/jrpc` handler + 信封分发                 |
| `zm_net_http_restful_server.h/.cpp`  | ZiMoPublic/net | RESTful 面 `ZmHttpRestfulServer`:业务路由注册 + WebSocket + CORS 挂点             |
| `net_dock.h/.cpp`                      | ZiMoService(根目录) | 宿主:持有三 Manager,Init(全局 Options + 三面配置),Open/Close 转发静态,暴露 GetXxxServer() |
| `http_frontend_manager.h/.cpp` 等      | ZiMoService    | 三个宿主 Manager(前端/JRPC/RESTful):各持有本面服务器派生类实例,仅配置(AddListener/Setup/GetServer),无生命周期委托 |

***

## 3. 生命周期与运行(FR-01\~04)

### 3.1 基类(抽象基类:一个纯虚 `RegisterRoutes()`,其余 virtual 可覆写)

```cpp
// 权威定义:ZiMoPublic/net/zm_net_http_server.h(本节为摘要,v2.12 起与代码对齐)
//   历史注:曾计划拆分 net/drogon_http_common.h(公共类型)与 net/drogon_http_server.h,
//   二者均已并入基类头文件;实际文件位置与命名见 §2.5。

using ZmHttpCoroHandler = std::function<
    drogon::Task<drogon::HttpResponsePtr>(drogon::HttpRequestPtr)>;   // 首参按值!见 §4.1
using ZmHttpStreamHandler = std::function<drogon::Task<drogon::HttpResponsePtr>(
    drogon::HttpRequestPtr, drogon::RequestStreamPtr)>;

struct ZmHttpSendFileOptions {                 // 方案乙行为参数(FR-12,§6.1)
    size_t  chunkSize      = 1 * 1024 * 1024;
    size_t  interBlockMs   = 50;               // 0 = 发完即调度(自适应)
    size_t  watermarkBytes = 8 * 1024 * 1024;  // 可选增强
    int64_t stallAbortMs   = 120 * 1000;
    std::function<void(uint64_t sent, uint64_t total)> onProgress;
};

class ZmHttpServer              // 抽象基类:一个纯虚 RegisterRoutes(),其余 virtual 可覆写
{
public:
    // ── 全局运行参数(进程级;经 Init 一次性注入,运行期不可改) ──
    struct Options {
        size_t threadNum = 0;                  // 事件循环线程数(0 = 自动 = CPU 核数)
        size_t maxConnections = 8192;          // 连接数护栏
        size_t clientMaxBodySize = 10ULL * 1024 * 1024 * 1024;   // 框架级上限(**含流式**)
        size_t nonStreamBodyLimit = 256ULL * 1024 * 1024;        // 非流式 PreRouting 预检 413(0=关)
        size_t clientMaxMemoryBodySize = 64 * 1024;              // 超限落临时文件(drogon 默认 64KB)
        size_t maxConnectionsPerIP = 0;        // per-IP 连接数(0=不限;⚠ 单机压测同源 IP)
        size_t idleTimeoutSec = 90;            // keep-alive 空闲回收
        size_t keepaliveRequests = 0;          // 单连接请求数上限(0=不限)
        bool   enableRequestStream = true;     // 流式上传开关(业务依赖,保持 true)
        size_t workPoolSize = 8;               // RunOnPool 工作池(切勿设 0)
        bool   gzip = false, brotli = false;                 // 动态压缩
        bool   gzipStatic = false, brotliStatic = false;     // 静态孪生文件发送(非现场压缩)
        bool   ticketDisabled = false;         // TLS SessionTicket 禁用(FR-11)
        std::string certFile, keyFile;         // 全局证书(空 = 纯 HTTP)
    };

    // ── 静态生命周期(进程级一次;Uninit→Initialized→Opened→Closed) ──
    static bool Init(const Options& opts);
    static bool Open();
    static void Close();
    static bool IsInitialized();   static bool IsOpened();
    static bool ReloadCertificates();          // 运行期唯一可热更新能力(FR-10)

    // ── 本面属性 / 监听配置(一对象一端口,v2.5) ──
    bool IsHttps() const;   uint16_t GetPort() const;   std::string GetBindIp() const;
    virtual void AddListener(uint16_t port, bool useSSL = false,
                             const std::string& ip = "0.0.0.0", bool useOldTLS = false,
                             const std::vector<std::pair<std::string, std::string>>& sslConfCmds = {});

    // ── 路由归属(P1/v2.9;设计 §4.6) ──
    virtual void SetRootPath(const std::string& path);   // 前缀 | 空(不拥有) | "/"(兜底)
    virtual const std::string& GetRootPath() const;
    bool IsCatchAllRoot() const;   bool HasRoot() const;   std::string FaceDesc() const;
    static const ZmHttpServer* LookupOwner(std::string_view path);
    static bool IsSharedPath(std::string_view path);
    static void MarkShared(const std::string& path);

    // ── 路由注册(§4.1 三条形态) ──
    virtual void RegisterCoro(const std::string& path, drogon::HttpMethod m,
                              ZmHttpCoroHandler h,
                              const std::vector<std::string>& filters = {});
    template <typename F>                       // P3/v2.11:路径参数即形参 → drogon 原生绑定
    void RegisterCoroWithPathParams(const std::string& path, drogon::HttpMethod m, F&& h,
                                    const std::vector<std::string>& filters = {});
    virtual void RegisterCoroWithDeadline(const std::string& path, drogon::HttpMethod m,
                                          ZmHttpCoroHandler h, size_t deadlineMs,
                                          const std::vector<std::string>& filters = {});
    virtual void RegisterStreamCoro(const std::string& path, drogon::HttpMethod m,
                                    ZmHttpStreamHandler h,
                                    const std::vector<std::string>& filters = {},
                                    uint64_t maxBytes = 0);
    virtual void RegisterMultipartCoro(const std::string& path, drogon::HttpMethod m,
                                       ZmMultipartHandler h,
                                       const std::vector<std::string>& filters = {},
                                       uint64_t maxBytes = 256ULL << 20);
    virtual void RegisterWebSocket(const std::string& path, const WsCallbacks& cb);

    // ── Filter / advice 挂点(§4.2/§4.3;签名与 Drogon 1.9.13 一致) ──
    virtual void AddFilter(const std::string& name,
                           const std::function<bool(const drogon::HttpRequestPtr&,
                                                    drogon::HttpResponsePtr&)>& f);
    virtual void RegisterPreRouting(...);    virtual void RegisterPostRouting(...);
    virtual void RegisterPostHandling(...);  virtual void RegisterPreSending(...);

    // ── 响应助手(业务层构造响应的唯一入口;P2/v2.10 边界,§2.4) ──
    static drogon::HttpResponsePtr JsonResponse(int status, const ZMJSON& data);
    static drogon::HttpResponsePtr ErrorResponse(int status, const std::string& msg);
    static drogon::HttpResponsePtr JsonpResponse(const drogon::HttpRequestPtr& req, const ZMJSON& data);
    static drogon::HttpResponsePtr FileResponse(const drogon::HttpRequestPtr& req,
                                                const std::string& filePath);      // 页面/小文件(含 304)
    static drogon::HttpResponsePtr RedirectResponse(const std::string& url, int status = 302);
    static drogon::HttpResponsePtr NotFoundResponse(const drogon::HttpRequestPtr& req = nullptr);
    static void SetCorsAllowedOrigins(const std::vector<std::string>& origins);   // v2.17:业务策略
    static bool IsCorsOriginAllowed(const std::string& origin);                  // 判据(业务 advice 调用)
    static ZMJSON FromDrogonJson(const Json::Value& v);
    static Json::Value ToDrogonJson(const ZMJSON& v);

    // ── 文件传输 / 流式(FR-12/13) ──
    virtual drogon::Task<drogon::HttpResponsePtr> SendFileCoro(...);          // 方案甲(含 Range/304)
    virtual drogon::Task<drogon::HttpResponsePtr> SendFileStreamCoro(...);    // 方案乙(定时器链)
    virtual drogon::Task<drogon::HttpResponsePtr> SendFileHybridCoro(...);    // 阈值自动路由
    using StreamCb = std::function<void(drogon::ResponseStreamPtr)>;
    static drogon::HttpResponsePtr MakeStreamResponse(StreamCb cb, bool disableKickoff = true);
    static drogon::Task<bool> SaveStreamToFile(drogon::RequestStreamPtr stream,
                                               const std::string& destPath,
                                               const ZmHttpUploadFileOptions& opts = {},
                                               bool* tooLarge = nullptr);

    // ── 限流(§16.4) / 阻塞离核(FR-19) ──
    static drogon::RateLimiterPtr CreateRateLimiter(drogon::RateLimiterType type,
                                                    size_t capacity, double timeUnitSec);
    class ZmIpRateLimiter { public: static Create(...); bool Check(req, resp); };
    static void SetIpBlocked/UnblockIp/SetIpQuota/SetIpAllowed/RemoveRateRule(...);
    static bool IsRateRuleHit(const std::string& ip);
    template <typename T> static drogon::Task<T> RunOnPool(std::function<T()> fn);
    static void SetWorkPoolSize(size_t n);   static size_t GetWorkPoolSize();

protected:
    virtual void RegisterRoutes() = 0;        // 派生面:注册自己路径前缀的路由
    bool CheckRouteOwnership(const std::string& path, const char* what);   // R1
    static bool ValidateRouteOwnership();     // Open 期一致性校验 + 只读快照固化
    struct ZmFileMeta { bool found, sizeFailed; size_t size; int64_t mtimeSec; };
    static ZmFileMeta FetchFileMeta(const std::string& path);
    static std::pair<std::string, std::string> CacheHeaders(const ZmFileMeta& m);
    static drogon::HttpResponsePtr Maybe304(const drogon::HttpRequestPtr& req,
                                            const ZmFileMeta& m,
                                            const std::pair<std::string, std::string>& cacheHeaders);
    bool IsLocalPortIn(const drogon::HttpRequestPtr& req) const;   // 本地端口 ∈ 本面监听
    struct ZmHttpListener { uint16_t port; bool useSSL; std::string ip; bool useOldTLS;
                            std::vector<std::pair<std::string, std::string>> sslConfCmds; };
    ZmHttpListener m_listener;   bool m_listenerSet = false, m_setupDone = false;
    std::string m_rootPath;
    // 注:`AddOtherRootPath` 为**前端面**专有(兜底拒绝前缀),不在基类;
    //     `SetTicketDisabled` 已收敛进 `Options.ticketDisabled`;
    //     `SetImplicitPage` 从未实现(SPA 走业务页面 advice,§11.2)。
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

### 4.1 路由注册:两条路径(RegisterCoro / RegisterCoroWithPathParams)

`app().registerHandler(path, handler, {method} + filter 约束)`——`HttpBinder` 支持 `Task<HttpResponsePtr>` 返回型。

**两条注册路径(v2.11 定版;按"是否有路径参数"分工)**:

| 入口 | handler 形态 | 路径参数怎么拿 | 注册方式 |
| --- | --- | --- | --- |
| `RegisterCoro(path, m, h, filters)` | `std::function<Task<HttpResponsePtr>(HttpRequestPtr)>`(**类型擦除**) | `req->getRoutingParameters()[N-1]`(或平台助手) | 无 `{N}` → `registerHandler`;含 `{N}` → `registerHandlerViaRegex` + 手工转换 |
| **`RegisterCoroWithPathParams(path, m, h, filters)`** | **带形参的任意可调用体**(`[](HttpRequestPtr req, std::string uid) -> Task<HttpResponsePtr>`) | **函数形参**(`{1}` → 第 1 个形参) | `registerHandler` **原生绑定**(不经正则) |

**⚠ 路径参数与 `getParameter` 无关(常被误解)**:`getParameter("k")` 只读 **query 串**(`HttpRequestImpl.h:200`),路径参数走 `routingParameters_` —— drogon **原生注册也不填** `getParameter("1")`。故读路径参数只有两种正当方式:形参(推荐)或 `getRoutingParameters()`;**手工 `find("/xxx/")` 切字符串是错的**(前缀一改即静默失效)。

**为什么必须有第二个入口(v2.11 根因,已源码核实)**:`ZmHttpCoroHandler` 把形参擦成 `std::function<... (HttpRequestPtr)>` → `HttpBinder::paramCount()` = `traits::arity` = **0**(`HttpBinder.h:217`)→ 含 `{N}` 的路径命中 `addHttpPath` 的占位符校验 `place > paramCount` → **`LOG_ERROR` + `exit(1)`**(`HttpControllersRouter.cc:368-374`)——是**类型擦除的必然结果,不是本捆绑缺陷、升级 drogon 也修不掉**。v2.2 曾把它记为"访问违例",v2.11 更正。带形参的 handler `arity ≥ 1`,paramCount 正确,`{N}` 自然可用。

**守卫(用错就失败得早)**:`RegisterCoroWithPathParams` 的 `arity==0` → 编译期 `static_assert`;路径无 `{N}`、**编号越界/编号 0/重复编号/超长编号** → 运行期 ERROR + 拒绝注册(v2.20 补齐:越界与重复会被 drogon 的 `addHttpPath` 判为致命并 `exit(1)`,超长编号则让其 `std::stoi` 抛未捕获异常,故一律前置拦截 —— 编号上界是 `arity`,而协程特化的 arity **不含** `HttpRequestPtr`;占位符少于形参不拦,drogon 对缺失参数回落 `req->as<T>()`)。**语义差异**:原生 `{N}`→`([^/]*)`(允许空段),旧 `PathPatternToRegex` 用 `([^/]+)`(要求非空)——迁移后 `/xxx/` 这类空段请求会进入 handler(业务自校验),不再被路由层 404。

**v2.2 实测修订(仍然有效)**:

* `CoroHandler` 形参须**按值** `HttpRequestPtr`——本捆绑 drogon 的 `FunctionTraits` 协程特化仅匹配 `Task<Resp>(*)(HttpRequestPtr, ...)`(member/functor 链落点;const 引用会静默落入无 `first_param_type` 的基础匹配)。`RegisterCoroWithPathParams` 同样受此约束(已在 `static_assert` 文案中提示);

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

* 前端面:请求本地端口 ∈ {本面监听端口}(经 `IsLocalPortIn`)且路径**归属其他面**(`LookupOwner(path) != this`,按归属表推导)→ 404;

* JRPC 面:本地端口 ∈ {39440} 且路径非本面 root(`/zimo/jrpc`)且非**平台共享路径** → 404;

* RESTful 面:本地端口 ∈ {39441} 且路径非本面 root(`/zimo/api`)及其子路径且非**平台共享路径** → 404。

**外来前缀的来源(v2.9)**:由**归属表推导**(§4.6),不再由宿主手工登记 —— 新增服务器面或改 root 自动生效(旧 `AddOtherRootPath` 降级为兜底口子)。**共享路径名单亦单源**:`IsSharedPath`(当前仅 `/ping`),各面门禁不再各自硬编码。

门禁 advice 注册顺序保持在 SPA/重定向 advice **之前**,保证裁决唯一;`/ping`(基类 `MarkShared` 声明,FR-23)三方均可达(旧版里 `/zimo/api/ping` 与全局 `/ping` 并存,路径不同无冲突)。

> ⚠ 实测注意(2026-09-12):业务层的前端页面 advice(module_gate)无端口判定,会把非 `/zimo/` 路径(含 `/ping`)在所有端口一并 302 到 `/login` → 共享路径必须在业务门禁内显式放行,**否则 FR-23 名存实亡**(本次已修)。

### 4.6 路由归属机制(P1/v2.9;把"归属"从约定变成机制,已实施)

> 动机:`/zimo/metrics` 曾从公网前端端口(80/443)泄漏 —— 它由 `Init` 注册为**游离全局路由**:不属于任何面的 root,各面 per-port 门禁(黑名单式)全都放行它(commit c7b5814 靠"把路径挪进 `/zimo/api`"修好,机制未改)。同类风险还有**跨面注册**:门禁按端口判定,`fe.RegisterCoro("/zimo/api/x")` 完全合法且静默可达。本节把归属写成可验证、可失败的机制。

**不变式**:每条已注册路由都归属于某个声明了 root 的服务器面,或显式声明为平台共享。

**root 三态(`SetRootPath`)**:`SetRootPath` 声明本面 root(v2.20:声明写进 `s_rootClaims` —— 一对象一条、覆盖式;归属表由声明表**重建**,两者不会各自漂移)。

| 取值 | 语义 | 例 |
| --- | --- | --- |
| `"/zimo/api"` | 本面**拥有**该前缀(最长段前缀匹配) | JRPC `/zimo/jrpc`、RESTful `/zimo/api` |
| 不调用(空) | **不拥有任何前缀**,不得注册业务路由(纯 advice 垫片) | 前端 `redirectOnly` 实例 |
| `"/"` | **兜底归属**(全局至多一个实例;未被其他面认领的路径归它) | 前端完整面 |

重复 root / 多个兜底 / 同一端口被两面登记 → `Open()` 按**当前声明表**重算冲突并拒绝启动(v2.20:改掉声明即自愈 —— 原为累积的冲突记录,改配置也照样拒启,"修正后可重试 Open"名不副实)。

**两条规则(v2.16:平台路由闸门 R2 已移除)**:

* **R1 业务路由**(`RegisterCoro` / `RegisterCoroWithDeadline` / `RegisterStreamCoro` / `RegisterMultipartCoro` / `RegisterWebSocket`):路径归属必须是本面,否则 ERROR + **拒绝注册**(既不注册也不会泄漏);`Open()` 后不可再注册路由(否则会绕过 R3 快照)。
* ~~**R2 平台路由闸门**~~(**v2.16 移除**):原规则要求"由 `Init` 注册的平台路由必须归属某个具体面,否则拒绝启动";其唯一调用方是手写指标端点(v2.15 已删),故整条移除。**替代约束**:新增平台路由时须自行确保它落在某个面的 root 下或 `MarkShared` —— 不归属任何面的路由会在**所有**端口可达(含公网前端,历史泄漏见本节动机),运行期由 **R3 归属网**告警兜底(只告警、不拦)。
* **R3 运行期归属网**(PreSending 统一出口;快照零锁两次查表):已服务响应(状态 < 400)的归属面 ≠ 端口所属面 → `[ROUTE-LEAK]` WARN —— 命令式门禁漏网时的唯一可见信号。豁免:共享路径、未声明 root 的垫片实例(它只发 301/302,不是路由)。

**门禁来源改为推导**:前端门禁不再由宿主手工登记外来前缀,改为 `LookupOwner(path) != this` → 404;新增服务器面或改 root 自动生效。`AddOtherRootPath` 降级为"归属表之外的额外前缀"兜底口子。

**实现要点**:`SetRootPath` 从内联改为实现(写声明表 → 重建归属表);Open 期校验三项并按当前声明重算 ①root 声明冲突 ②**端口登记冲突**(v2.20 补;端口侧与 root 同病:两面同端口、且重登记不释放旧端口)③已登记路由归属复检(平台路由闸门 R2 于 v2.16 移除);`LookupOwner/IsSharedPath` 在 `Open()` 固化快照后零锁;声明表与归属表均为进程级、仅 Phase1 写(声明可覆盖,故"只增不清"仅指路由登记表)。

**验收(P1,2026-09-12 实测通过)**:① ~~平台路由游离 → 拒绝启动~~(**v2.16 随 R2 一并移除该闸门**;历史实测见 v2.9 修订行);② 跨面注册 → 注册期 ERROR + 不可达;③ `/ping` 三面 200、外来前缀三面 404、`/zimo/api/metrics` 仅 39441 可达(业务门禁 401);④ 正常配置下无 `[ROUTE-LEAK]` 误报(80→443 重定向实例已豁免)。

### 4.7 自动 JSONP:机制全局 + 授权逐路由 + 行为差量(P6/v2.13,例外开关 v2.19,已实施)

> 动机:JSONP 的**转换规则**是通用的(不含业务语义),所以出口(PreSending advice)保持全局;但"**这个接口是否允许被任意站点的 `<script>` 跨站读取**"是逐接口的安全决策 —— JSONP 天然绕过同源/CORS,与 CORS 白名单(管"允许哪些 Origin")方向相反。原实现是全局开关 `SetAutoJsonp`(默认开),等于给所有 GET JSON 接口开了跨站读取口子。

**三层职责**:

| 层 | 接口 | 说明 |
| --- | --- | --- |
| 全局基线 | `SetJsonpDefaults(ZmJsonpOptions)` | `paramNames={"callback"}` / `wrapErrors=true` / `maxBodyBytes=256KB` / `enabled=true`(观察期默认,见下) |
| 逐路由授权 | `SetJsonpEnabled(prefix, ZmJsonpOverride)` | **前缀匹配(含子路径,最长前缀优先)** —— 用前缀而非精确路径,因为路由模式含 `{N}` 时实际请求路径与声明串不相等;声明默认启用,`enabled=false` = **例外**(不包装,也不打未声明告警) |
| 差量覆盖 | `ZmJsonpOverride{enabled / paramNames / wrapErrors / maxBodyBytes}` | **`std::optional` 语义:未设置 ≠ 设为默认值**(否则空结构体会把全局基线拍回去);其余字段继承基线。注意 `o.paramNames = {}` 在此语义下是**重置为未设置**,要置空列表须写 `std::vector<std::string>{}` |

```cpp
// 业务层(Open 前;与归属声明同期)
ZmHttpServer::SetJsonpEnabled("/zimo/api/legacy/");                       // 用全局基线
ZmHttpServer::ZmJsonpOverride o;  o.paramNames = {"cb"};                   // 只改回调名
ZmHttpServer::SetJsonpEnabled("/zimo/api/legacy/old/", o);
ZmHttpServer::ZmJsonpOverride ex; ex.enabled = false;                      // 例外(v2.19)
ZmHttpServer::SetJsonpEnabled("/zimo/api/secret", ex);
```

**判定顺序**(PreSending,零锁读启动期快照):GET? → `CT_APPLICATION_JSON`? → 命中声明前缀?(命中 → 该前缀 `enabled=false` 则放行;未命中 → 用全局基线,`enabled=false` 则不包)→ `wrapErrors`/`maxBodyBytes` → 回调名合法? → 包装。例外与授权同表竞争,仍按最长前缀优先。

**迁移路径(为什么默认仍是 en=true)**:任何依赖 JSONP 的调用方都在工作,直接翻默认会让未声明的老客户端静默失效。故:**观察期**保持 `enabled=true`,但对"**未声明路由却被包装**"按路径打一次 `[JSONP] 未声明路由 ...` WARN;确认无外部调用方后置 `enabled=false`(一行),此后只有 `SetJsonpEnabled` 声明的路由可被 JSONP 读取。启动期会打印每条声明的合并后选项(便于发现拼错的前缀)。

**明确不做**:回调名**字符集白名单**不做逐路由覆盖(它是 XSS 边界,放宽须全局单点评审);包装语法形态(`cb(json);` 等)与未声明路由的默认姿态也只留在全局 —— 它们是全局语法/安全契约,逐路由分化会让客户端与审计都不可控。v2.19 的逐前缀 `enabled` 是**例外开关**不是默认姿态:它只决定"这条已声明前缀包不包",不改变未声明路由的姿态。

**边界**:JSON-RPC 面为纯 POST 信封,天然不触发;静态资源/HTML 因 CT 不符不触发;显式 `JsonpResponse` 的产物已是 JS(CT 非 JSON),不会被二次包装(§4.4)。

**本服务落地状态(v2.19,目标形态已切)**:`net_dock.cpp` 置全局 `enabled=false` 并声明 `SetJsonpEnabled("/ping")`(探针保留 JSONP,`/ping` 为三面共享路径故各面一致)。切换依据:12 天运行日志(2026-08-31\~09-12)中 36 次带 `callback` 的请求**全部来自 127.0.0.1**(两个非回环客户端 192.168.1.7/.20 从未使用过该参数),即无真实 JSONP 调用方;早先被自动包装的 9 条路径(`/ping`、`/zimo/api/auth/me`、`/zimo/api/admin/users` 等)均为本机自测触发。**代价**:此后"漏声明"的症状由"照包 + WARN"变为"静默不包"(前端 `<script>` 会拿到裸 JSON),故新增需跨站读取的接口时须同步 `SetJsonpEnabled`。

### 4.4 响应助手(FR-08/09/24)

| 助手                           | 行为                                                                                                                                      |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `JsonResponse(status, data)` | `newHttpJsonResponse(data)` + `setStatusCode`;成功为**裸 JSON**(与 auth.js 一致)                                                               |
| `ErrorResponse(status, msg)` | 统一错误包 `{error:{code,message}}`(与 auth.js 一致)                                                                                            |
| `JsonpResponse(req, data)`   | 显式助手:读 `req->getParameter("callback")`:有合法值 → `cb(json);`(Content-Type `application/javascript`);白名单 `[A-Za-z0-9_.]`(复用 `IsValidJsonpCallback`),非法值返回 400;无参数 → 常规 JSON                                                                   |
| `FileResponse(req, path)`   | **页面/小文件**(P2/v2.10):单次 stat → `Last-Modified` + 强 ETag;`If-None-Match` 优先 / `If-Modified-Since` 兜底 → 304(无 body);文件不可用 → 404(记 WARN);Cache-Control 由调用方设置(平台不覆盖)。⚠ 不带 Range —— 大文件/断点续传走 `SendFileCoro`/`SendFileHybridCoro` |
| `RedirectResponse(url, status=302)` | 跳转响应(默认 302;需要 301 语义时显式传 301/303/307/308) |
| `NotFoundResponse(req=nullptr)` | 404(当前线程为服务器 loop 且已 `SetNotFoundPage` 时回自定义 404 页) |

> 接口修正:v2.1 起 `JsonpResponse` 接收 `req`(FR-24"检测 query callback 参数"由基类完成,业务层只传数据)。
>
> **v2.5 自动识别型 JSONP(与主流 Koa/Spring 中间件语义一致;v2.13 起授权逐路由)**:`Init` 注册一条全局 **PreSending advice**——规则:GET + 合法回调名(白名单 `[A-Za-z0-9_.]`,≤128)+ 响应 `CT_APPLICATION_JSON` → 改写 `resp` 为 `cb(json);` + `CT_TEXT_JAVASCRIPT`;任一不满足 → 原样返回(不污染普通 REST/前端静态/WS 升级)。**授权与行为已收到逐路由**(`SetJsonpEnabled`/`SetJsonpDefaults`),详见 **§4.7**。<br/>**边界**:JSON-RPC 面(39440)为纯 POST 信封,天然不触发;访问日志先于 PreSending 执行,记录的字节数为包装前大小。

* handler 未捕获异常由 Drogon `HttpBinder` 自动转 500(FR-08);404 页由 `SetNotFoundPage` 承载。

* 可选内存限流:基类提供 `CreateRateLimiter(type, capacity, timeUnit)`(封装 `drogon::RateLimiter`),业务层在 filter 内 `isAllowed()`;持久化限流留在业务层。

* **JRPC 信封注意**:39440 面是 JSON-RPC 信封(`{"jsonrpc":"2.0","error":{code,message},"id":...}`),**不复用** REST 的 `ErrorResponse`,由 `HttpJsonRpcServer` 单列自己的响应封装(§11.3)。

***

## 5. TLS(FR-10/11)

* **证书统一走全局** **`app().setSSLFiles(cert, key)`**;`AddListener(port, useSSL=true)` **不再传 per-listener cert**——因为 `reloadSSLFiles()` 只重载全局证书,per-listener 证书热加载不生效(头文件契约核实)。无证书则为 HTTP(80)。

* `ReloadCertificates()` → `app().reloadSSLFiles()`(热加载,换内容不换路径)。

* `Options.ticketDisabled = true`(经 `Init` 全局 `setSSLConfigCommands` 追加 `Options=-SessionTicket`,FR-11 开关,默认不启用)。

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
回调(newAsyncStreamResponse 内 —— 框架在连接 loop 上调用,HttpServer::sendResponse 断言在 loop 内):
  St::Run()   : 由 connWk 定型 m_loop(连接 loop;取不到退回 app().getLoop())
                → 打开文件句柄(UTF-8→wide,FILE_SHARE_READ)→ SetFilePointerEx 定位(Range 起点)→ 驱动第一块
  St::Next()  : 读一块的任务投 HttpIoPool(I/O 线程 ReadFile,绝不阻塞事件循环)
                → 读毕 queueInLoop 回执 m_loop → stream->send(块) → 推进进度
                → m_loop->runAfter(interBlockMs, St::Next) 让发送与缓冲排水
                → send() 返回 false(连接已关)或已发完 → Finish()(幂等:在途读归零后关句柄 + 关流)
  停滞判定    : 每次 Next 比较 conn->bytesSent() 差值,冻结超过 stallAbortMs → 放弃(客户端可 Range 续传)
                (send() 成功只代表排入发送缓冲,不代表对端消费,故以套接字实发字节为准)
```

预读窗口 = 1 块(**内存有界**,不以字节计数,验收词相应放宽);块间延时与 `chunkSize` 可配(`SendFileStreamOptions` 增加 `interBlockMs`,默认 50ms)。

* **方案乙编码注记(头文件契约)**:`drogon::ResponseStream` 内部自做 chunked 帧(`send` 包装 `hex(len)\r\ndata\r\n`,`close()` 发 `0\r\n\r\n`),因此方案乙**不设 Content-Length**(Transfer-Encoding: chunked);`Content-Range`/206 头仍照常设置,客户端 Range 续传语义不变。

* **停滞放弃**:持续无进展 > `stallAbortMs`(默认 120s)→ `stream->close()`(客户端 Range 续传)。`onProgress` 可选回调。

* **读盘异常与截断(v2.20)**:读失败记 ERROR(带 `GetLastError`)、提前读到文件尾记 WARN;两条路径都按"发完"收尾 —— 流正常关闭、不发错误帧,故 **纯 200 响应(无 Content-Length)客户端无从察觉**,只有 206 能靠 `Content-Range` 声明长度判定。现实可构造的触发是"文件存在但读打开失败"(例:正被写入者独占 —— 存在性检查走 `GetFileAttributesW`,不打开文件,故能通过;此时响应已是 200 + 空体,服务端 ERROR 是唯一线索);"发送中删/截断"因句柄以 `FILE_SHARE_READ` 打开(未授 WRITE/DELETE)而不可达 —— 写者/删者/改名者都会拿到共享冲突。方案甲不适用本条:它带 Content-Length(或 sendfile 区间长度),body 与声明长度一致,文件缩小还会在框架重新 stat 时得 416。

```cpp
// 方案甲
resp = drogon::HttpResponse::newFileResponse(path, offset, length, true, dispName, CT_NONE, "", req);
```

> 注(v2.20 更正 v2.1 的落点):`AsyncStream` 无 `getLoop()` 接口,定时器与读回执一律投**连接所属 loop**(`m_loop`)。状态机因此全程单线程:`m_loop` 既是工厂回调的执行线程,也是 `conn->bytesSent()` 的唯一读写线程(该值是非原子 `size_t`,跨线程读属未定义行为);主 loop 不承担分块发送,并发下载之间也不再互相串行。`ResponseStream::send` 本身可跨线程调用(trantor `AsyncStreamImpl` 内部 `queueInLoop`),但同 loop 归属省掉一次跨线程唤醒与一次整块拷贝。

**Hybrid**:文件 < `threshold`(默认 2GB)→ 方案甲;≥ → 方案乙。

### 6.2 流式(FR-13)

`MakeStreamResponse(cb, disableKickoff=true)` 为**静态工厂**,返回 `newAsyncStreamResponse` 的响应,调用方在返回的 `HttpResponsePtr` 上设置头部后再 `cb(resp)`。支撑音频流(订阅者线程 `stream->send`,trantor 线程安全)、zip 打包流、SSE。

* 音频流帧格式(len+seq+Opus 20ms)与 zip 分块逻辑均在业务层,仅把旧 `task->SendReplyChunk` 换成 `stream->send`。

* 现 `m_sseGone`/`m_tasksGone` 收尾防 UAF 纪律继续保留(stream 在业务线程 close 前需判活)。

### 6.3 上传(FR-15)

全局 `setClientMaxBodySize(10GB)`(单请求上限)+ `enableRequestStream(true)`;**流式接收封装(v2.4 定版,与代码一致)**:`RegisterStreamCoro(path, method, h, filters, maxBytes=0)`——业务协程 handler 携带框架注入的 `RequestStreamPtr`(内部绑定 Drogon stream-handler 三参回调 → `async_run` 桥接协程);`maxBytes` 为**路由级上限**(默认 0 = 不额外限制,全局 10GB 兜底):基类入口按 `X-File-Size` 自动早拒(超限 → null reader 丢弃 + 413,不进入业务),并写入 `req` attributes(`ZmStreamMaxBytes`)供业务落盘兜底取用;`SaveStreamToFile(stream, destPath, opts, &tooLarge)`:落盘状态机 —— 事件循环入队(FIFO 有界,积压超限即中止,内存有界)、专用写线程顺序落盘(磁盘 I/O 不占事件循环)、三种终局统一收尾(写盘失败/超限删半成品,排空后保留)。**进度回调(v2.20 定版)**:按 `opts.progressIntervalMs` 节流(首块必过闸),**成功路径由写线程回执带回落盘总量、在 `done` 之前补发一次满量回调** → 业务必然收到 100%(原判据 `written == len` 不成立:读的是累计落盘量、比的是当前块长,而末块在 `OnData` 时刻尚未落盘);进度与完成回调都在**请求所属 loop** 执行(与数据块回调同线程,业务侧无需加锁)。

***

## 7. 请求超时(FR-14)

连接级空闲超时框架内置;业务级 deadline 基类封装 `RegisterCoroWithDeadline`,内部**复刻旧版 TryReply 门**:

```cpp
// 实际实现:`ZmHttpServer::RegisterCoroWithDeadline`(注册期包装为 callback-form handler)
// ── v2.14 定版:超时状态**共享**给定时器,响应回调在"发出响应那一刻"清空 ──
struct ZmDeadlineState {
    std::atomic<bool> answered{false};                 // 原子门(TryReply 门):只回一次
    std::function<void(const HttpResponsePtr&)> cb;    // 完成即置空
    std::weak_ptr<trantor::TcpConnection> connWk;      // 弱引用守卫
};
loop = 连接所属 loop(v2.1:不用主 loop)
tid  = loop->runAfter(deadlineMs/1000.0, [st]{          // 保存 TimerId 供注销
           if (st->answered.exchange(true)) return;    // 业务已回
           连接不在/已断 → return;  否则 st->cb(504) 后 st->cb = nullptr;
       });
finish(resp):                       // 业务成功/异常共用收尾
    if (st->answered.exchange(true)) return;           // 已被超时占位
    loop->invalidateTimer(tid);                        // 幂等;内部 runInLoop → 跨线程安全
    连接在 → st->cb(resp);  最后 st->cb = nullptr;      // ← 关键
```

**为什么要 v2.14(旧写法的两个代价)**:① 旧实现把 `cb` **按值捕获进定时器** → 该回调持有的 `TcpConnectionPtr` 与请求对象(**含 body**)会被多留到 `deadlineMs`,大 body + 长 deadline 下滞留可观(实测:50ms 完成的请求,旧写法要到 1s 才释放);② 定时器没注销 → 到点空转一次。
**注意 trantor 的取消语义**:`invalidateTimer` 只从 id 集合删除,定时器对象本身仍留到到期才弹出 —— 所以**真正的收益来自"完成即清空 cb"**,注销只是顺手(避免空转)。
**footgun 守卫**:`deadlineMs == 0` 会 `runAfter(0)` → 每个请求立即 504,故注册期拒绝并提示改用 `RegisterCoro`。

**纪律**:原子门保证只回一次;弱引用 + `connected()` 保证晚到不碰已销毁连接;流式/下载端点不设死线。

***

## 8. 异步基础设施(FR-19)

### 8.1 RunOnPool

```cpp
template <typename T>
drogon::Task<T> ZmHttpServer::RunOnPool(std::function<T()> fn)
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

* 访问日志:`ZmHttpServer::Init` 一次性注册一对 advice(PreRouting `RecordAccessStart` 记起始时间 + 生成/透传请求 ID 入 req attributes;**PreSending `FinalizeResponse` 结算**耗时、回写 `X-Request-Id`、写单行 `PUBLIC_LOG_*`),覆盖 方法/路径/状态码/耗时/字节数/两端端口;不自建 access.log、不使用 AccessLogger 插件(v2.4 定版;v2.6 注册点从"首个 Open"移入 Init;结算点 v2.20 据实测更正 —— drogon 1.9.13 的 PostHandling 只覆盖 controller/binder 路径,静态/304/Range/重定向/拦截响应都不经,故统一挂 PreSending)。
  **顺序不变式(v2.20)**:`RecordAccessStart` 必须是**首个** PreRouting advice —— 后续 advice 一旦短路(如 body 闸门 413),该响应仍会经 PreSending 结算,若本 advice 被排到短路者之后,那些响应会取到缺失的 `ZmRequestId`(drogon 对缺失键返回空串)。`FinalizeResponse` 对缺失值补 `zm-unknown`,响应头与日志行首共用同一取值(对齐同函数内 `ZmAccessStartMs` 的既有防御)。框架层解析期拒绝(超大 Content-Length 等)根本不走 advice 管道,该头与安全头都不会出现 —— 既有边界,非本层可修。

* 健康检查:`/ping` 在 `ZmHttpServer::Init` 内默认注册 → `{"pong":true}`(FR-23),并经 `MarkShared("/ping")` 声明为**平台共享路径**(各面门禁与归属网单源豁免,§4.6 R2)。

***

## 11. 派生服务器面详细设计(FR-25/D7)

### 11.1 三面总览

| 面                    | 端口     | 职责              | 关键注册                                                                                                                                         |
| -------------------- | ------ | --------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `HttpFrontendServer` | 80/443 | 静态(docroot)+ 自定义 404 + 路径封禁 + 80→443 重定向 | `SetDocumentRoot` + `SetNotFoundPage` + `AddDeniedPath`(docroot 策略)+ HTTPS 模式 80→443 重定向(FR-22)+ per-port 门禁(§4.4/§4.6);**页面/SPA/页面跳转由业务层 advice(module_gate)承载**,响应经 `FileResponse`/`RedirectResponse` 助手(v2.10) |
| `HttpJsonRpcServer`  | 39440  | JSON-RPC        | `RegisterCoro("/zimo/jrpc", Post, ...)` + method 分发器(-32700/-32601),**JSON-RPC 信封单列** + per-port 门禁                                     |
| `HttpRestfulServer`  | 39441  | ★ 业务 API        | 各业务模块 `RegisterCoro("/zimo/api/...", ...)` + `RegisterWebSocket` + **显式挂 CORS** + per-port 门禁                                                |

三者共享 `app()`;路由靠路径前缀区分;全局项(/ping/访问日志 advice/JSONP)由 `ZmHttpServer::Init` 一次性注册(去重纪律 §2.3)。

### 11.2 前端服务器 `ZmHttpFrontendServer`(80/443)
> **v2.6 一对象一端口**:HTTPS 模式下前端由**两个实例**构成——`ZmHttpFrontendServer(redirectOnly=false)` 挂 443(完整面:静态/SPA/404/门禁)+ `ZmHttpFrontendServer(redirectOnly=true)` 挂 80(**仅** 80→443 重定向 advice,FR-22);无证书模式仅一个完整面(80,HTTP)。SPA 回落/封禁前缀由业务层 `AddSpaFallback/AddDeniedPath` 配置,平台不硬编码页面路径。

**静态文件**:`SetDocumentRoot(wwwRoot)` 内置防目录穿越、MIME、Range;Cache-Control 语义保留:HTML 不缓存、JS/CSS 靠 `?v=` 破缓存(经 `SetStaticFileHeaders` 或页面别名 handler 按扩展名设置,(实现见 §16.1.2 `SetStaticCachePolicy` + PreSending 加头;旧 libevent 版 SendFile 已随宿主层移除))。
> **路径编码契约(v2.20)**:文档根与自定义 404 页(`SetDocumentRoot` / `SetNotFoundPage`)的路径参数均为 **UTF-8**,平台侧一律先 `UTF-8 → wide` 再进 `filesystem`;窄串会被按 ANSI 码页解码,中文安装路径下会静默降级(404 页退回框架默认页)。drogon 自身读文件经 `utils::toNativePath` 转 wide,故校验一处即可。

**页面路由表(前端端口)**:

> **v2.10 更新(P2)**:页面/SPA 路由**不由平台面实现**,而由业务层页面 advice(`module_gate`:
> 白名单页/`/portal` SPA/`/force-reset`/`/` 跳转,含会话判定)**承载**;平台只提供
> docroot 静默服务(StaticFileRouter)、`SetNotFoundPage`、路径封禁口子,以及页面响应的
> **条件请求助手**(`FileResponse`)。下表为**行为契约**(由业务 advice 实现),非平台注册表;
> 平台侧原 `AddSpaFallback`/`m_spaFallbacks` 分支已删(P2:双实现且无调用者)。

| 路径                                                      | 行为                                        | 实现位置(v2.10)                                                          |
| ------------------------------------------------------- | ----------------------------------------- | --------------------------------------------------------------------- |
| `/`                                                     | 无会话 → 302 `/login`;有会话 → 302 `/portal`      | 业务页面 advice(`module_gate`)                                             |
| `/login` `/register` `/reset` `/404`                     | 白名单页(已登录访问 → 302 `/portal`;`/404` 恒渲染)    | 同上 + `ZmHttpServer::FileResponse`(条件请求 304 同源)                        |
| `/force-reset`                                          | 无会话 → 302 `/login?redirect=…`;有会话 → SPA 页壳  | 同上                                                                    |
| `/portal` 与 `/portal/*`                                 | SPA 页壳(无会话 → 302 `/login?redirect=<path>`)   | 同上;`setImplicitPage` 语义为"目录解析",不能做 SPA 回落,故不用                            |
| `/share/{token}`                                        | 302 → RESTful 端口分享页                       | 业务路由(`RegisterCoro` → `ZmHttpServer::RedirectResponse`)                |
| `/assets/*` `/svg/*` `/css/*` `/js/*` `/html/*` `/favicon.ico` | 物理静态文件(drogon StaticFileRouter,含 IMS→304) | `SetDocumentRoot` 直接命中                                                |
| 物理存在的敏感目录(如历史 `www/doc`)                                 | **不可达**(404)                              | `AddDeniedPath`(平台 docroot 访问策略;**当前发布目录无此类目录,故无调用者**)              |
| 其余未匹配                                                   | 自定义 404 页(状态码 404)                        | `SetNotFoundPage("html/404.html")`                                    |

**前端面 advice 链(v2.10,注册顺序):**

```cpp
// 平台面(本面 Setup;仅重定向实例 / 仅门禁与封禁)
// 1. 80→443 重定向(HTTPS 模式,独立实例):本地端口 == 80 → 302(FR-22)
// 2. per-port 门禁(§4.4/§4.6):本地端口 ∈ {80,443} 且路径归属其他面 → 404
// 3. 路径封禁(AddDeniedPath;当前无调用者)→ cb(404)
// 放行规则统一:cc() 交业务页面 advice / StaticFileRouter
// 业务面(module_gate,后注册 → 后执行)
// 4. 共享路径(/ping)放行 → 静态资源放行 → 白名单页/SPA/跳转(会话判定,响应经
//    ZmHttpServer::FileResponse(条件请求 304)/ RedirectResponse / NotFoundResponse)
```

> v2.10:`/portal/*` 的 SPA 回落由**业务层** advice 承担(平台侧重复实现 `AddSpaFallback` 已删);
> 页面响应经平台 **`FileResponse`** 助手 → 页面获得 `Last-Modified`/`ETag`,命中条件请求回 304
> (§16.1.1 宣告的"页面 304"至此在真实路径生效)。advice 按本地端口判定,39441/39440 不受影响。

**防穿越**:`SetDocumentRoot` 自带路径规范化与穿越防护;若业务层自行解析物理路径,沿用旧 `GetFullPathNameW` + 根包含校验逻辑。

### 11.3 JSON-RPC 服务器 `ZmHttpJsonRpcServer`(39440)

> **实施状态(v2.12)**:平台面已实现(`/zimo/jrpc` 单 handler + 协议校验 -32700/-32600/-32602/-32601/-32603);**当前未注册任何 method**(`ServicePortal::RegisterJsonRpcRoutes` 为空,平台内建 `ping` 已移除)。

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
        if (method == "<业务方法>") { rsp["result"] = ...; }
        else                       { rsp["error"]["code"] = -32601; rsp["error"]["message"] = "Method not found: " + method; }
        co_return ZmHttpServer::JsonResponse(200, rsp);   // 注意:JRPC 信封单列,不复用 REST ErrorResponse
    });
```

* 请求/响应信封 `jsonrpc`、`id` 字段按 JSON-RPC 2.0 保留;

* 方法表扩展:业务侧经 `RegisterMethod` 注册(分发器不再内建任何 method);**run 之后调用被拒绝并记 ERROR**(v2.20)—— method 表启动期只写、运行期只读(读取侧在事件循环上无锁),守卫与 `RegisterCoro` 家族同款。

### 11.4 RESTful 服务器 `ZmHttpRestfulServer`(39441)

> **实施状态(v2.12)**:已实现并验证 —— auth(`/auth/*`)、portal(`/portal/*`)、userAdmin(`/admin/users/*`;v2.11 起带参路由走 `RegisterCoroWithPathParams`);**filehub(`/portal/filehub/*`)与音频流(`/portal/serverAudioStream/*`)未实现**(P3/P4 未开工,下方路由表为设计目标);filehub/audio 的权限种子已留在 DB。

> v2.3:根路径默认 `ZM_HTTP_RESTFUL_SERVER_ROOT_URI`(`/zimo/api`),同上可自定义。

**注册策略**:显式注册,路径保留 `/zimo/api` 前缀(省去旧"剥根前缀"步骤)。每个业务模块新增 `RegisterRoutes()`(经 manager 取得本面 server),内部逐条 `RegisterCoro`;旧 `ServicePortal::RestfulRequestCB` 分发链拆散为各模块路由注册,调用顺序等价性保留:auth → filehub → audio → portal → ping。

**路径参数写法(v2.11 约定)**:路径含 `{N}` 的路由**一律用 `RegisterCoroWithPathParams`**,参数写成 handler 形参(如 `[](HttpRequestPtr req, std::string uidStr)`);读参数用形参,**不得**手工解析 `req->path()`(前缀一改即静默失效),也不用 `getParameter("1")`(它只读 query 串,路径参数恒为空)。无路径参数的路由用 `RegisterCoro`。

**路由表(前缀** **`/zimo/api`)**:

* 认证 `/auth/`(免登录):POST `/auth/login` `/auth/register` `/auth/reset` `/auth/complete-change` `/auth/logout`;GET `/auth/me` `/auth/heartbeat`。

* 门户 `/portal/`:GET `/portal/info`;GET `/portal/userManager`(分页);GET `/portal/userManager/{id}`;POST `/portal/userManager/{id}/{action}`。

* 文件中心 `/portal/filehub/`:GET `/list` `/search` `/download` `/shares` `/tasks` `/task_status` `/zip_task_download`;POST `/upload` `/mkdir` `/rename` `/move` `/copy` `/delete` `/zip` `/share` `/unshare` `/task_create` `/task_cancel` `/task_delete` `/zip_download` `/share_commit` `/share_cancel`;POST `/portal/filehubAdmin/sync`(手动一致性)。`zip_download` 走 query(`task_id`+`ids`),迁移后保持 query 语义((旧 libevent 版 `HandleZipStart` 已随宿主层移除;filehub 未实施,§11.4))。

* 音频 `/portal/serverAudioStream/`:GET `/stream`(流式,§6.2);GET `/status`(采集快照)。

* 系统:GET `/ping` → `{"pong":true}`。

**鉴权机制(v2.12 按实现更正)**:

**实际落点 = 业务层 `ZmAuthGateModule`**(`modules/module_gate.{h,cpp}`),不是平台 `RequireModule`
(该 helper 未采纳,`net/rest_util.h` 不存在):

| 层 | 接口 | 作用 |
| --- | --- | --- |
| 粗判(advice) | `SetupRestfulGate(rest)` | 同步 PreRouting:非 `/zimo/api` 路径放行;免鉴权接口(register/login)放行;无 cookie → **401**;其余放行进 handler |
| 完整校验(handler 首行) | `drogon::Task<ZmGateResult> Authorize(req, requiredPerm)` | 会话查库(`SessionModule::AuthAndTouch`)+ 状态/删除/强制改密边界 + 可选权限点;`ok=false` 时按 `status/code/message` 直接回 |
| 同步判定 | `ZmAuthGateModule::CheckCtxSync(ctx, path)` | handler 已自行 `AuthAndTouch` 后用(避免嵌套协程 Task 链卡死,见模块头注释) |
| 模块/接口辅助 | `ApiError/ApiOk/ClientIp/ApiPermForPath/IsForceChangeAllowedApi` | 统一响应与权限点映射 |

* **双保险次序**:advice 只做"cookie 存在性粗判"(防无效请求进 handler);真正的会话有效期、账号状态、权限点由 handler 首行 `Authorize()` 完成 —— 与旧 `AuthAndTouch` 语义等价。
* **advice 必须同步回调**(本捆绑 drogon 的 advice/filter 回调跨线程会卡死链),故异步查库一律留在 handler 协程内。

**CORS + 凭据(v2.17 语义定版)**:白名单经 `ZmHttpServer::SetCorsAllowedOrigins`(Open 前声明;**不再放在 `Options`** —— 它是业务策略,与 `SetJsonp*`/`SetRootPath` 同款)注入,平台只提供**判据** `IsCorsOriginAllowed(origin)`;实际行为由业务 advice 决定(`service_portal::RegisterRestfulCors`,仅 RESTful 面):
· **跨站**:Origin 命中白名单 → 预检 200 + 回显 `Access-Control-Allow-Origin` + `Credentials: true`;不在名单 → 预检 **403**、普通响应**不回显**任何 CORS 头;
· **同站跨端口**:Origin 与 Host 同 host、仅端口不同(页面 80/443 → API 39441)→ 业务侧硬逻辑放行,**不经过白名单**;
· **空名单** = 只放行同站跨端口,任何跨站 Origin 一律拒绝。
· 实测(2026-09-12):同站 → 200 + ACAO;`http://127.0.0.1` 对 Host `localhost:39441`(不同站、仅靠名单)→ 200 + ACAO;`https://evil.example` → 预检 403 且响应无 ACAO。

### 11.5 会话与鉴权映射

* **不引入** Drogon 内置 session;沿用自研 `zm_session` cookie + 会话表(PBKDF2、LRU、锁定、限流全在业务层,已与 HTTP 栈解耦)。

* 旧 `AuthAndTouch` 接收 `ZmHttpdTask*` → 迁移后接收 `HttpRequestPtr`/`HttpResponsePtr`:

  * 读:`req->getCookie("zm_session")`

  * 写 `Set-Cookie`:`resp->addHeader("Set-Cookie", ...)`(登录/登出/心跳续期)

  * cookie 属性(Path/Max-Age/SameSite/Secure)原样保留。

### 11.6 模块接口改造映射

| 现有接口                                                         | 迁移后                                                                            |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| `XxxModule::DispatchRest(loop, verb, path, task, body, len)` | `XxxModule::RegisterRoutes()`(逐条 `RegisterCoro`/`RegisterCoroWithPathParams`)+ 内部 `Handle*(req[, uidStr])` |
| `XxxModule::RegisterHttpRoutes(HttpServerManager*)`(80 端口)   | 并入 `HttpFrontendServer::RegisterRoutes()`(§11.2 表)                             |
| `ServicePortal::RestfulRequestCB`(分发链)                       | 拆散为各模块路由注册(§11.4),保留顺序等价性:auth → filehub → audio → portal → ping               |
| `ServicePortal::JrpcRequestReadCB`                           | `HttpJsonRpcServer::RegisterRoutes()`(§11.3)                                   |
| `ZmReqLoopRest::ResponseJson/ResponseError`                  | `ZmHttpServer` 响应助手(§4.4)                                                        |
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
3. JRPC:`POST /zimo/jrpc` 非法 JSON → -32700;未注册 method → -32601(平台不内建任何 method)
4. RESTful:auth 全端点、portal 信息/用户管理、filehub 全端点(含 Range 206、>10GB 大文件、zip 打包流)、音频流、`/ping` 全部通过
5. 鉴权:会话失效 401、模块未授权 403、`zm_session` cookie 读写一致
6. 线程纪律:handler 内无阻塞操作(代码评审);文件中心/音频/登录在高并发下无事件循环卡顿
7. 生命周期:Stop 无崩溃、无泄漏;证书热重载生效
8. 关闭顺序:业务线程先 join;在飞 HTTP 由业务层保障(守护线程/断点),无 UAF/崩溃(回归现内存纪律)
9. JSONP:带 `callback` 返回 JS 包装、非法名拒绝、无 callback 走常规 JSON
10. 派生:三服务器面独立实例化/注册/状态查询,共享 app() 不串扰

***

## 14. 分阶段实施计划

| 阶段      | 内容                                                                   | 产出                  | 状态(v2.12) |
| ------- | -------------------------------------------------------------------- | ------------------- | --------- |
| P0      | vcxproj 增加 drogon 头文件/库目录 + 链接项;最小 `app()` 引导跑通 80 端口                | 可编译、可 curl 通        | ✅ 已完成     |
| P1      | 前端面:docroot + 自定义 404 + 重定向;页面/SPA 归业务 advice(§11.2)                 | 现有前端页面完整可用          | ✅ 已完成     |
| P1.5    | 三面 + 测试接口:JRPC 信封、REST 测试组、WS echo、Range/deadline 验证                | 三端口测试接口全绿           | ✅ 已完成(测试路由已移除) |
| P2      | RESTful 基础:响应助手 + 鉴权 + CORS + auth/portal/userAdmin 路由              | 登录/门户/用户管理可用        | ✅ 已完成(见 `docs/tests/2026-09-06-用户系统验证报告.md`) |
| P3      | RESTful 文件中心:filehub 全路由 + 上传/下载(Range)/zip 流式                       | 文件中心可用              | ❌ 未实施(无模块;权限种子已留) |
| P4      | RESTful 音频 + JSON-RPC:stream/status 流式 + JRPC 业务方法                     | 远程音频 + JRPC 业务可用    | ❌ 未实施(无音频模块;JRPC 仅内建 `ping`) |
| P5      | 收尾:证书热载、关闭顺序、压测、广播回归                                                 | 全量回归通过              | 🟡 部分(证书热载/关闭语义已实现;压测与广播回归未做) |

***

## 15. 改动清单(v2.6:生命周期静态化重构)

1. 新增 `net/zm_net_http_server.h`(公共类型 + RunOnPool 声明;原计划的 `drogon_http_common.h` 已并入)
2. ~~新增 `net/drogon_http_server.h/.cpp`~~(与第 1 项合并为 `zm_net_http_server.h/.cpp`:基类 + 静态生命周期 + Options + 去重 + 响应助手 + advice 挂点)
3. 新增 `net/http_frontend_server.h/.cpp` / `net/http_jsonrpc_server.h/.cpp` / `net/http_restful_server.h/.cpp`
4. ~~新增 `net/rest_util.h`~~(未采纳:响应助手落在基类,鉴权落在 `ZmAuthGateModule`)
5. 重建既有空壳 `net_dock.h/.cpp`(宿主层,置于服务工程根目录)(持有三 Manager,`Init` 内先 `ZmHttpServer::Init(opts)` 全局一次再配置三面,Open/Close 转发静态,暴露 GetXxxServer)与 `http_server_manager.h` / `http_jsonrpc_manager.h` / `http_restful_manager.h`(.cpp)(各持有并**仅配置**本面服务器,无生命周期委托)
6. `service_center.cpp`:`OnStart` 建 NetDock → `NetDock::Init()`(全局 Init + 三面配置)→ 建 Portal(注册路由,Phase1)→ `NetDock::Open()`(静态);`OnStop`:`Portal.Shutdown()` → `NetDock::Close()`(静态)
7. `service_portal.h/.cpp`:构造时经 NetDock 取三面 server 引用注册路由(本期测试路由);`Shutdown()` 业务收尾
8. `ZiMoService.vcxproj`:接入 drogon 库目录 + `DROGON_STATIC_DEFINE/TRANTOR_STATIC_DEFINE` + 链接项(drogon/trantor/jsoncpp/cares/lz4/sqlite3/libssl/libcrypto),移除旧 `libcrypto_static/libssl_static` + 新增文件

***

## 16. 第二期:增强特性(2026-09-03 评审)

在本期(§1-15)之上追加五个增强项,均为**默认保守**配置(不改变现有行为,
net_dock 显式启用后生效)。接口演进:基类(§2 类层次)同一位置追加。

### 16.1 静态资源条件请求与缓存头(FR-12/FR-20 增强)

> 本节分两项(B 档在前):**B 档 = 静态目录 304(内建 + SPA 补齐)**(16.1.0-16.1.1);
> **A 档 = 静态缓存头策略**(16.1.2)。与用户清单逐项对应。

**16.1.0 B 档:静态目录 304(内建验证)**:

**事实盘点(2026-09-03 源码核验,更正早期错误的"仅 Range 判 304"结论)**:
- 静态目录 304 **内建且默认开启**:`enableLastModify_{true}`
  (`StaticFileRouter.h:144`);无 Range 请求在 `StaticFileRouter.cc:433-470` 完整判定
  (If-Modified-Since == Last-Modified → 304),Range 请求 `:331-377` 按 RFC 7233 §3.1
  先判预条件;`.gz/.br` 孪生发送时 `Last-Modified` 取源文件 mtime(`:543-547`),
  与 304 判定同源;
- `SendFile*`(§6)已实现 Last-Modified + 强 ETag(`"size-mtime"`,If-None-Match
  优先弱比较/If-Modified-Since 兜底)→ 304;
- 发送路径事实:文件型响应发送不经 304 特殊分支(`HttpServer.cc:1002-1026`),
  **"PreSending 改写响应为 304"方案不可行**(仍会 sendFile 正文)。

**16.1.1 B 档:SPA/页面回落补齐(编码;v2.10 修正落地位置)**:
原方案只把 `FetchFileMeta/CacheHeaders/Maybe304` 提升为 protected static 并让**平台**的
SPA 分支使用 —— 但该分支(`AddSpaFallback`)无调用者,**生产页面路径是业务层
`module_gate::ServeIndex` 的裸 `newFileResponse`,仍无 Last-Modified/ETag/304**,
即本节宣告的修复此前**未在真实路径生效**(v2.10 实测确认)。
**v2.10 定版**:提升为**公开响应助手 `ZmHttpServer::FileResponse(req, path)`**
(单次 stat → LM + 强 ETag → 304;文件不可用 → 404 并记 WARN),`SendFile*`、业务页面
(ServeIndex)、以及任何后续页面响应共用同一实现(行为单源);平台侧重复的 SPA 分支删除。

**16.1.2 缓存头策略(A 档)**:
drogon 静态响应默认 `Expires: 1970`(`StaticFileRouter.cc:544-546`);1.9.13
仅有全局统一 `setStaticFileHeaders`(`HttpAppFramework.h:952`,无按扩展名 API)。
发布层两态:再校验态(默认,html)→ `public, max-age=0, must-revalidate`;
长缓存态(指纹 js/css 等)→ `public, max-age=31536000, immutable`。

```
struct ZmStaticCacheConfig { std::string defaultPolicy;
                             std::vector<std::pair<std::string,std::string>> extPolicy; };
void SetStaticCachePolicy(const ZmStaticCacheConfig&);   // Open 前调用
```
`RegisterRoutes` 追加一条 PreSending(本端口判定):静态响应特征(无 body +
有 Last-Modified)按 `req->path()` 扩展名查 extPolicy,未命中用 defaultPolicy;
已有 Cache-Control 不覆盖;**只加头不改状态**,与 304 无交互(304 定"传不传",
缓存头定"请求不请求")。指纹命名本体属发布层,不在本期。

**16.1.3 不做**:静态目录 ETag(drogon 无挂点,IMS 已覆盖同语义);
SPA 页的 .gz 分发(现状一致,发布层后续)。

### 16.2 Multipart 表单/文件上传(FR-15 补充)

流式解析 API 已内建:`RequestStream::newMultipartReader(req, headerCb, dataCb,
finishCb)`(`RequestStream.h:110-114`);内存版 `MultiPartParser` 基于 `getBody()`,
超 64KB 语义不可控 → 弃用。

```cpp
struct ZmMultipartResult { std::vector<std::pair<std::string,std::string>> fields;
    struct File { std::string itemName, fileName /*已消毒*/, contentType;
                  uint64_t size; std::string data /*v1 内存*/; };
    std::vector<File> files; };
void RegisterMultipartCoro(path, drogon::HttpMethod m,
    std::function<drogon::Task<drogon::HttpResponsePtr>(req, ZmMultipartResult)> h,
    filters = {}, uint64_t maxBytes = 256ULL << 20);          // 单请求总量上限
static drogon::Task<int64_t> SaveMultipartFile(const ZmMultipartResult::File& f,
                                               const std::string& destPath);
```

**行为链**:Content-Type+boundary 预检(缺 → 400)→ 声明超限 413 → 流式 reader
(Header/Data/Finish 三回调;字段值 ≤1MB;超限换 NullReader + 413)→ 业务协程;
文件名 `SanitizeFileName` 消毒(去 `..`/`/`/`\`/控制符,空 → 非法标记)。
**决策**:v1 文件内容内存交付(受 maxBytes 硬上限);**大文件仍走裸流
(RegisterStreamCoro + X-File-Size)**,multipart 流式落盘留 v2;缓存(1)语义:
同 §16.1,B 档交互仅共用"上限判定"思路,无共享代码。

**前置实验(阶段 0,硬性门)**:100MB 文件在 reader 下的内存峰值(确认
是否经 `clientMaxMemoryBodySize` 落盘 / 纯内存),据此定建议 maxBytes;
消毒攻击集。失败 → 改设计(文件通道复用写线程)。

### 16.3 请求 ID(FR-17 扩展;指标端点已于 v2.15 移除)

```
// 指标端点(v2.15 已移除):原实现为手写进程级计数 + JSON,经 `Options.metricsPath`
// 注册;无消费者,且受 RESTful 面会话门禁限制(无 cookie → 401)无法被监控抓取。
```

**请求 ID**:恒启(无开关)。PreRouting 生成 `zm-<unix秒>-<原子序>` 入
attributes("ZmRequestId"),**透传优先**(请求带 `X-Request-Id` 且合法
`[A-Za-z0-9-_.-]` ≤128 → 原样使用);PreSending 回写 `X-Request-Id`
(含 304/重定向);访问日志行首加 `[zm-...]`(**格式变更,解析脚本需同步**)。
**缺失兜底与顺序不变式(v2.20)**:回写与日志改用**同一个取值**,属性缺失时补 `zm-unknown`;
`RecordAccessStart` 必须是首个 PreRouting advice(详见 §10) —— 否则被短路 advice 处理的响应
会出现空 ID。框架层解析期拒绝的响应不走 advice 管道,本就无该头。
**结算点(实测修正)**:drogon 1.9.13 的 PostHandling advice **仅覆盖 controller/binder
响应路径**,静态目录(含 304)、Range、重定向、advice 拦截响应不经 —— 访问日志与
指标结算统一挂 **PreSending**(handleResponse 统一出口),PreRouting 生成+结算同一对 advice。
**指标端点(v2.15 移除)**:原为 `Options.metricsPath` 注册的手写 GET handler(进程级原子计数:total / 2xx-5xx / inflight / 延迟四桶 / uptime_s),**已整体删除**。
删除理由:① 无任何消费者;② 它位于 `/zimo/api/metrics`,受 RESTful 面会话门禁约束(无 cookie → 401),**监控系统无法抓取**;③ 进程内计数无历史、无 label,不足以当监控用。
需要监控时的方向:drogon 自带 `utils/monitoring`(Counter/Gauge/Histogram)+ `PromExporter` 插件(标准 Prometheus 文本 + label);注意其路由同样要落在某个面 root 下,且要正面解决"门禁 vs 可抓取"的冲突(§4.6)。

### 16.4 限流(**已实施 2026-09-04**;设计→实现,RL 全系验证通过)

> §4.4 曾留"可选内存限流"一句,此为正式方案。底层:捆绑 Drogon 1.9.13
> `RateLimiter.h`(`newRateLimiter(type, capacity, timeUnit)`,取值
> kFixedWindow/kSlidingWindow/kTokenBucket;并发必须用 `SafeRateLimiter` 锁包装,
> filter 会在多个事件循环线程执行)。

**16.4.1 能力分两层(两种语义都覆盖)**:
- **逐 IP 独立配额**:每个 IP 一个独立桶(各用各的额度,互不影响);
- **专项 overlay(针对特定 IP 单独待遇)**:封禁 / 白名单放行 / 不同阈值,
  支持**运行期动态 CRUD**(核心诉求:封禁恶意 IP、临时调额、解封,不重启)。

**16.4.2 接口(基类)`ZmHttpServer::RateLimit` 静态区**:
```cpp
/// 单桶(全局配额;filter 内 isAllowed;仅内部/专属调用方使用)
drogon::RateLimiterPtr CreateRateLimiter(drogon::RateLimiterType type,
                                         size_t capacity,
                                         std::chrono::duration<double> timeUnit);
/// 逐 IP 桶协调器:默认桶有界(默认 10000),满时按插入序驱逐;专项桶见 16.4.3
static std::shared_ptr<ZmIpRateLimiter>
ZmIpRateLimiter::Create(drogon::RateLimiterType type, size_t capacity,
                        double timeUnitSec, size_t maxEntries = 10000);
bool ZmIpRateLimiter::Check(const drogon::HttpRequestPtr& req,
                            drogon::HttpResponsePtr& resp);   // false = 已限流(429 已填)
// ── 专项 overlay(运行期可调;规则量 ≤ 数千条) ──
void SetIpBlocked(const std::string& ip);          // 封禁 → 直接 429(不消费配额)
void UnblockIp(const std::string& ip);
void SetIpQuota(const std::string& ip, size_t capacity,
                std::chrono::duration<double> timeUnit);   // 专项阈值(覆盖默认)
void SetIpAllowed(const std::string& ip);          // 白名单放行(配额不消费)
void RemoveRateRule(const std::string& ip);
bool IsRateRuleHit(const std::string& ip);         // 审计/日志标注
```

**16.4.3 执行链与热路径**:
```
请求(preRouting/filter)→ 查 overlay(COW 快照,原子指针读 = 零锁)
  封禁 → 429(短路);白名单 → 放行;专项 → 取规则上的额度槽(懒建 / 复用,稳态零锁);
  默认  → 有界桶查/建(满则按插入序驱逐) → isAllowed() 失败 → 429
```
- **专项桶的生命周期(v2.20)**:额度槽 `ZmQuotaSlot` 挂在 overlay 规则条目上 —— 规则删除即随
  最后一份快照释放(不留派生态残留)、参数未变则沿用同一槽(额度状态延续)、参数变更换新槽;
  一个 IP 一条规则对应一个桶(**进程级,跨实例/跨面共享**),条目数恒等于规则数,**不做驱逐**
  (驱逐 = 重置额度 = 放行一波)。默认桶相反:key 是来访 IP(攻击者可控)→ 必须有界,
  `maxEntries` 满时按插入序驱逐。两类桶的 key 来源不同,**不可共用一个上限/驱逐队列**
  (共用会让攻击者用洪水挤掉管理侧设的额度状态)。
- **规则表 COW**:`std::atomic<std::shared_ptr<const RuleMap>>`,写 = copy+swap
  (低频、微秒级);事件循环无锁读;
- **适配形态**:`AddFilter("rate-limit", ...)` 注册为 filter,挂到
  `RegisterCoro/RegisterStreamCoro` 的 filters —— 路由级限流一行接入;
  全局粗粒度(如每连接级)用 PreRouting advice 变体。

**16.4.4 动态规则与管理入口(宿主选一/组合)**:
1. **管理 API**(推荐):RESTful 面 `/zimo/api/admin/rate`(POST/DELETE/GET),
   复用 `ZmAuthGateModule` 鉴权(仅 admin 模块);
2. **规则文件热重载**:`Options.rateRulesFile`(JSON 基线表),mtime 变化/信号重注入;
3. TTL 可选:封禁带有效期(惰性过期,不耗定时器);操作加审计日志(PUBLIC_LOG)。

**16.4.5 决策与边界**:
- 429(非 500/503),标准语义;
- **持久化不在本层**:规则桶/动态规则重启即失,跨重启落在业务层(SQLite 重放,
  延续 §4.4"持久化限流留业务层"分工);
- ⚠ **压测适配(重复两节的坑)**:单机压测全部请求同源 IP —— per-IP 默认配额
  必须显式放大(或规则层开白名单),否则压测自 429;自动化脚本同理;
- CIDR/段匹配留 v2(v1 精确全串;`*.` 段匹配见 16.4.6 可选);
- 与连接数护栏分治:per-IP **连接数**(`maxConnectionsPerIP`,已接入)与 per-IP
  **请求速率**(本层)互补不重叠。

**16.4.6 验证要点(RL 系列)**:
RL1 固定窗口 5/min:连发 7 → 第 6 起 429;RL2 滑动窗口边界(恢复期);
RL3 per-IP:A 超限 B 不受影响;RL4 封禁 IP(CIDR 精确)→ 429,解封即 200;
RL5 白名单 IP 穿过限额;RL6 动态 SetIpQuota 降低后立即生效(不重启);
RL7 并发压测下统计:命中数与 isAllowed 数一致(SafeRateLimiter 无竞态)。

### 16.5 验证(第二期验收矩阵)

| 项 | 用例(简) | 预期 |
|---|---|---|
| 静态 304 | 静态 js/index + IMS;Range+IMS;.gz 孪生 + IMS | 304 无 body(内建);U 系列详见前文用例 |
| SPA/页面 | /login → 200 带 LM/ETag/Cache-Control:no-cache;+INM → **304(实测 2026-09-12)**;/portal 未登录 → 302 | 修复点 16.1.1(v2.10 定版:经 `FileResponse` 助手) |
| 缓存头 | 默认再校验态;extPolicy 长缓存(按 .js);非静态无注入 | 16.1.2 |
| Multipart | 字段×2+文件×1;无 boundary 400;2MB>maxBytes 413+无残留;`../../x.sh` 消毒为 `x.sh`;字段-文件混序 | M 系列 |
| 请求 ID | 响应带 X-Request-Id;上游透传;非法字符重生成;日志行首一致 | R 系列 |
| 限流 | 固定/滑动窗口边界;per-IP 互不影响;封禁/解封即时生效;动态阈值;白名单穿越 | RL 系列(§16.4.6)——2026-09-04 实测:RL1 ✅(5→第6个起 429);RL4 ✅(block/unblock 机制生效);RL5 ✅(白名单穿透限额 10/10);RL6 ✅(动态 quota 立即生效 2→3rd 429);RL7 ✅(并发 30 = 5×200+25×429,SafeRateLimiter 无竞态);RL2/RL3 为算法/结构级保证(kSlidingWindow 参数即用;桶 key=IP) |

### 16.6 实施顺序

1. **B 档 + SPA 补齐**(helper 提升,最小依赖)→ 2. **A 档缓存头**(独立)
   → 3. **请求 ID + 指标**(独立)→ 4. **Multipart**(阶段 0 前置实验后)
   → 5. **限流**(§16.4;filter 形态,依赖现有 filters/advice 机制)
各步编译验收:ZiMoService 全量构建(`/p:OutDir=A:/ZiMo/temp_build_out/`);
文档与代码分仓库提交(ZiMoPublic 代码 / ZiMoService docs)。

