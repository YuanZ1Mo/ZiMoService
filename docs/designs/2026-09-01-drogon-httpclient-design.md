# DrogonHttpClient 设计文档(出站 HTTP/HTTPS 客户端)

> 状态:第一期已交付(v0.2/2026-09-02);第二期设计已定稿(2026-09-07 增补,文末 §14–§19);**2026-09-13 代码评审修订**:§15.1 增 3xx 跟随与响应编码校验、§15.2 文件打开改工作池 + 停滞看护并入写侧判据 + 写失败原因保真、§15.3 chunk 尾部 CRLF 加严、§15.4 受理/停止原子性与回调弱引用、§7.2 重试面加 429 与 HTTP-date、§16 增第 9–16 条 · 日期:2026-09-01
> 范围:`ZmHttpClient` 门面 + `ZmHttpConnectionPool` 连接池 + trantor 流式下载通道
> 依赖:Drogon 1.9.13(`HttpClient`/`trantor::TcpClient`/`trantor::EventLoopThread`,捆绑于 `ZiMoPublic\drogon`)、`ZmThreadPool`(`zm_util_thread.h`)、`ZMJSON`
> 需求:服务器向外部服务发起 HTTP/HTTPS 出站请求的能力——第三方 API 调用、Webhook 回调、证书/资源拉取、大文件下载
> 对齐:命名/风格/静态生命周期与 `zm_net_http_server.h` 一致;协程优先;文件下沉 `ZiMoPublic\net`

***

## 1. 总览

| 项    | 决策                                                                                                                          |
| ---- | --------------------------------------------------------------------------------------------------------------------------- |
| 形态   | 进程级静态门面 `ZmHttpClient` + per-target 连接池 `ZmHttpConnectionPool` + 流式下载独立通道                                                   |
| 生命周期 | 静态状态机 `Uninit→Initialized→Closed`,`Init(opts)/Close()`;与服务器 `ZmHttpServer` 并列但独立 Init                                       |
| 业务回调 | **协程优先**(`drogon::Task<ZmHttpResult>`),另备异步回调 / 同步(非事件循环线程)两形态                                                              |
| 结果载体 | `ZmHttpResult` 结构体(网络层错误分类 + HTTP 状态 + drogon 响应泊接),业务不接触 drogon 请求类型                                                       |
| 线程模型 | **独立双 lane,与服务器完全解耦**:普通请求走客户端自建 trantor loop(s)(`HttpClient-Loop`);流式下载走独立下载通道 loop(`HttpClient-DL`,**通道 loop 直写盘**,见 §10) |
| 连接管理 | per-target 池(`key=scheme://host:port`),默认每目标 4 条,least-busy 择路,惰性创建                                                         |
| 重试   | 仅幂等请求 + 可恢复错误(`Timeout/NetworkFailure/5xx`);指数退避 + 上限 + 抖动 + `Retry-After` 优先                                               |
| 重定向  | 默认跟随 ≤5 跳(300 不跟;301/302 非 GET/HEAD 转 GET;跨域跳转剥除 `Authorization/Cookie`)                                                    |
| TLS  | 默认 `validateCert=true`;`trustCA` 追加信任根;mTLS(客户端证书)支持;禁用 TLS1.0/1.1                                                          |
| 大响应  | 普通请求整包缓冲 + `maxBodyBytes` 护栏;大文件下载走**流式通道(本期交付)**                                                                           |
| JSON | 复用 `ZmHttpServer::ToDrogonJson/FromDrogonJson`,业务侧统一 ZMJSON                                                                 |
| 日志   | 出站访问日志单行(方法/URL/状态/耗时/字节/重试数),per-target 统计查询                                                                               |

**本期交付**:门面 + 连接池 + 三种调用形态 + 重试/重定向 + JSON/表单/上传 + TLS + 出站日志统计 + 流式下载(Range 续传)。**本期不做**:HTTP/2(drogon 1.9.13 客户端仅 HTTP/1.1)、CONNECT/HTTP 代理、WebSocket 出站、Unix socket、Cookie 会话池(默认关闭 Cookies)。

***

## 2. 事实基线(drogon 1.9.13 客户端能力,头文件已核实)

| 能力            | 现状(drogon 内建**默认**行为)                                                                                            | 对设计的约束                                                      |
| ------------- | ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| 连接模型          | 每个 `HttpClientPtr` 一条持久连接;断线下次发送自动重连                                                                             | 承载并发必须 per-target 池化                                        |
| 事件循环          | drogon **默认**使用 `app()` 事件循环(`loop=nullptr`);`newHttpClient(host, loop=…)` 亦接受自定义 `trantor::EventLoop*`          | **本设计所有客户端显式传入自建 loop**(不依赖 `app()`);loop 由客户端自启自停          |
| 调用形态          | 回调 `sendRequest` / 同步(死锁断言:非 loop 线程) / 协程 `sendRequestCoro`(失败抛 `drogon::HttpException`)                        | 三种形态均封装为统一 `ZmHttpResult`                                   |
| 超时            | 仅总超时(double 秒,0=不超时),无连接级超时参数                                                                                    | 超时逐请求注入;connect 慢/失败由总超时兜底                                  |
| 重定向/重试        | 无内置                                                                                                              | 门面层自研                                                       |
| HTTP 版本       | `HttpTypes.h` 枚举仅 `kHttp10/kHttp11`                                                                              | 仅承诺 HTTP/1.1                                                |
| 响应体           | 整包缓冲,无流式读取 API                                                                                                   | 护栏 + 流式通道自研                                                 |
| TLS           | `validateCert/useOldTLS/setCertPath/addSSLConfigs`;均 **client 级、创建时固化**                                          | 校验开关只能全局(Options),无法逐请求切换;池化共享 client 不许动态改 TLS 配置          |
| TcpClient     | 构造 `(EventLoop*, InetAddress, name)`——**只收 IP 不收 hostname**;`enableSSL(TLSPolicyPtr)` 须在 `connect()` 前           | 下载通道必须先经 `trantor::Resolver` 异步解析域名;禁止 loop 内同步 getaddrinfo |
| TcpConnection | 公共接口**无** **`startReading/stopReading`**(grep 核验;muduo 系标准法此版未暴露)                                                | 无法暂停内核收包 → 背压不能靠"暂停读"实现,见 §10.2 直写盘方案                       |
| MultiPart     | 捆绑 `MultiPart.h` 仅服务端 `MultiPartParser`/`HttpFile`;`HttpRequest` 无 Multipart 上传 API(grep 核验)                     | multipart 上传需手拼 body(boundary 组装),见 §9                      |
| 协程工具          | `utils/coroutine.h` 现成:`sleepCoro(loop,delay)`、`queueInLoopCoro(workLoop,task,resumeLoop)`、`SwitchThreadAwaiter` | §7.2 退避睡眠与 §4.3 回环恢复直接复用,勿自写 awaiter                        |
| 线程安全          | `sendRequest` 会改动 req;同一 req 跨线程并发不安全                                                                            | 每次尝试新建 `HttpRequestPtr`(重试换新)                               |

trantor(捆绑于 `ZiMoPublic\drogon\include\trantor`)为 drogon 底层网络库:`trantor::EventLoopThread` 提供独立事件循环线程;`trantor::TcpClient` 提供 `connect()/setConnectionCallback()/setMessageCallback(RecvMessageCallback)/enableSSL(TLSPolicyPtr)`——流式下载的基础。**注意**:`ZmEvBaseRunLoop`(libevent)与 trantor 事件循环后端不同,不可互用。

***

## 3. 类结构与文件划分

```
业务模块(协程/回调/同步)
  → ZmHttpClient(门面:URL 解析/请求构建/超时/重试/重定向/JSON/日志统计)
      ├→ ZmHttpConnectionPool(普通请求:per-target drogon HttpClientPtr 池,挂客户端自建 HttpClient-Loop)
      └→ ZmHttpDownloadChannel(流式下载:自建 HttpClient-DL loop + Resolver + TcpClient,**通道 loop 直写盘**)
```

| 文件                                   | 位置             | 内容                                                                                                             |
| ------------------------------------ | -------------- | -------------------------------------------------------------------------------------------------------------- |
| `zm_net_http_client.h/.cpp`          | ZiMoPublic/net | 门面 `ZmHttpClient` + `Options`/`ZmHttpResult`/`ZmHttpRequestOptions` + 三调用形态 + 重试/重定向/JSON/日志统计                 |
| `zm_net_http_client_download.h/.cpp` | ZiMoPublic/net | `ZmHttpDownloadChannel`:`EventLoopThread` + `Resolver` + `TcpClient` 流式下载直写盘 + Range 续传(含 HTTPS via enableSSL) |
| `zm_net_http_pool.h/.cpp`(可并入门面)     | ZiMoPublic/net | `ZmHttpConnectionPool`:per-target 池、惰性创建、least-busy 择路                                                         |

加入 `LibZiMoPublic.vcxproj`,与 `zm_net_http_server.*` 同组。

***

## 4. 生命周期与线程模型(独立双 lane,与服务器解耦)

### 4.1 状态机与 loop 归属

```
Uninit → Initialized(ZmHttpClient::Init(opts)) → Closed(ZmHttpClient::Close(),终态)
```

* `Init(opts)`:注入全局参数 → **创建并启动** `HttpClient-Loop`(普通请求 lane,`trantor::EventLoopThreadPool(normalLoopThreads)` 池,默认 1 线程,可配 1..8)与 `HttpClient-DL`(流式下载通道,`ZmHttpDownloadChannel` 内部单 `trantor::EventLoopThread` 启动,后台线程跑 `loop->loop()`;`enableDownload=false` 不创建,启失败仅告警不影响普通 lane)。`EventLoopThreadPool` 见 trantor 头文件,`getNextLoop()` 轮询/`getLoop(id)` 按号取。**start() 非阻塞:getLoop() 在池线程建好 loop 前返回 nullptr**(回退 app() loop 会导致 trantor FATAL),故启动后**轮询等待全部 lane loop 就位**再暴露使用。

* 所有 `HttpClientPtr` 经 `newHttpClient(host, port, ssl, loop=本客户端 loop)` **显式绑定自建 loop**,全程不依赖 `app()`。

* `Close()`(**三步序,不可颠倒**):① 下载通道——停收新任务 → 对在飞会话逐一请求中止(置 `writerStop_` + 对在途写句柄 `CancelIoEx`)→ 等登记表排空(≤3s)→ `loop->quit()` + join,再对残留登记补叫停后清表(时序细节见 §15.4);② 普通 lane——清空连接池(在飞请求按取消语义终止)→ 各 lane loop 注销登记 → `quit()` + `EventLoopThreadPool::wait()`;③ 置 Closed。幂等;终态。**严禁在任一已登记 loop 线程内调 Close(自锁)**。

* **并发契约**:`Close()` 是进程级终态,也是在飞请求的取消点。各入口已查 `IsReady()` 拒收新请求,但"取到连接"与"评估该连接忙闲"之间仍有窄窗口(该段不能持锁,详见 §6),**业务须在 `Close()` 前停发新请求**——不得依赖 `Close()` 与在飞请求并发安全。

* **与服务器完全解耦**:客户端生命周期独立于 `ZmHttpServer`——服务器未 Open/已 Close 均不影响客户端可用性(出站探活、启动引导等场景可用);代价是客户端自持两个事件循环线程 + 一个小工作池(§4.3)。

### 4.2 双 lane 职责与隔离理由

| lane | 载体                                    | 适用                            |
| ---- | ------------------------------------- | ----------------------------- |
| 普通请求 | 客户端自建 loop(`HttpClient-Loop`,1..N 线程) | 常规 API/回调,短小请求                |
| 流式下载 | 独立下载通道 loop(`HttpClient-DL`)          | 大文件:长时间占用 + 高吞吐,避免抢占普通请求 lane |

* 与服务器隔离:大下载/出站洪峰不抢占三面服务器事件循环(双端互不感知)。

* lane 间隔离:大下载也不阻塞普通出站请求;下载通道单线程内可挂多个 `TcpClient`(并发下载)。

### 4.3 线程纪律(项目既有铁律延续)

* **公面 loop 绝不磁盘读写**;下载通道为专属 loop,**定向豁免(直写盘)**——单块写为一次顺序 syscall,任何单次写盘耗时超 `downloadWriteMaxMs` 或停滞超 `downloadStallAbortMs` 立即 abort 止损(见 §10.2;本设计不再有"背压水位/暂停读",因捆绑版 `TcpConnection` 无 `stopReading/startReading`)。

* 客户端自持小工作池(`ZmThreadPool`,默认 4,可配)承接阻塞小任务(multipart 组装、上传读盘),**不依赖** `ZmHttpServer::RunOnPool`(保持客户端独立可用)。

* **协程帧隔离纪律(2026-09-02 崩溃根治后确立,重要)**:本项目 MSVC 14.44 /std:c++20 实测下,"协程帧内持有复杂值对象(map/string)+ 跨线程 resume(drogon 响应回调线程)"形态可稳定触发 0xC0000005。**现编排 = 堆上 `ZmSendMachine` 回调状态机 + `ZmMachineAwaiter` 薄桥**(帧内仅 3 个指针对齐成员 `shared_ptr<ZmMachineCtx>`/`EventLoop*`/coroutine_handle;全部值对象经 `ZmMachineCtx` 堆传递)。**新代码禁止在协程帧内持复杂值对象,禁止跨线程 resume 含复杂值对象的协程**(演进为 `queueInLoop`/状态机回调链)。

* 同步形态 `SendSync` 仅限业务线程/自持工作池(内部 promise/future 桥接);所有已登记 loop(客户端 lane + 服务器各 loop)线程调用一律拒绝(loop 登记表 + `isInLoopThread` 检查,见 §12)。

* 协程默认**在客户端 lane loop 线程 resume**(drogon 回调直接 resume);业务需线程亲和(如服务器 handler 后续返回写响应)时,经 `ZmHttpRequestOptions::resumeLoop` 回环恢复(见 §7.1)。业务工作池仅承接显式离核的阻塞任务。

***

## 5. 公共类型与 API

```cpp
/// 单次请求运行时选项(每请求覆盖全局默认)
struct ZmHttpRequestOptions
{
    double timeoutSec = 0;              // 0 = 全局默认; -1 = 不超时
    int    retryCount = -1;             // -1 = 全局默认; 0 = 不重试
    bool   idempotent = false;          // 非幂等方法显式声明后可重试(配合业务重放语义)
    bool   followRedirect = true;       // 跟随重定向(上限走全局 maxRedirects)
    trantor::EventLoop* resumeLoop = nullptr; // 协程恢复目标 loop(空 = 客户端 lane loop;线程亲和业务用)
    std::map<std::string, std::string> headers;   // 追加头
    size_t maxBodyBytes = 0;            // 0 = 全局护栏
    // 流式下载(经 Download 系列接口,见 §10)
};

/// 统一结果(业务感知的唯一载体;错误分类对齐 drogon ReqResult)
struct ZmHttpResult
{
    drogon::ReqResult err = drogon::ReqResult::Ok;   // Ok/BadResponse/NetworkFailure/BadServerAddress/
                                                     // Timeout/HandshakeError/InvalidCertificate/EncryptionFailure
    int    status = 0;                 // HTTP 状态码(0 = 无响应)
    drogon::HttpResponsePtr resp;      // 泊接 drogon(头/体),业务按需读取
    double elapsedSec = 0;
    int    retries = 0;                // 实际重试次数
    bool   followedRedirect = false;
    std::string finalUrl;              // 重定向终结 URL
    ZMJSON Json() const;               // Content-Type=json 时解析为 ZMJSON(失败空对象)
    std::string Body() const;
};

class ZmHttpClient
{
public:
    struct Options
    {
        // —— 全局参数 ——
        size_t normalLoopThreads = 1;     // 普通请求 lane 事件循环线程数(1..8;>1 时按 target 哈希绑定 loop)
        size_t workPoolSize = 4;          // 客户端自持阻塞工作池(离核任务,切勿设 0)
        size_t maxConnPerHost = 4;        // 每目标持久连接数
        size_t maxTargets = 256;          // 池容量上限(超限逐出"创建序最旧"池;见 §6)
        double defaultTimeoutSec = 10;    // 总超时默认值(0 不准,-1 不超时)
        int    retryMax = 3;              // 幂等请求最大重试次数
        int    maxTotalAttempts = 8;      // 重试+重定向 总尝试预算(防互跳死循环)
        double retryBaseMs = 200;         // 指数退避基数
        double retryCapMs = 3000;         // 退避上限
        double retryJitter = 0.25;        // 抖动幅度(±)
        bool   autoRedirect = true;       // 跟随重定向
        int    maxRedirects = 5;
        bool   validateCert = true;       // 全局 TLS 校验(**唯一开关**,client 级固化,无逐请求覆盖;应急关闭须日志告警)
        std::string trustCA;              // 追加信任根 PEM(内网自签);实现路径见 §8;空 = 系统信任
        std::string clientCert, clientKey; // mTLS 客户端证书(可选;普通 lane 经 setCertPath,下载通道经 TLSPolicy)
        std::string userAgent = "ZiMoClient/1.0";  // UA 唯一来源;业务经 headers 传的同名头不生效(见 §16.14)
        std::map<std::string, std::string> commonHeaders;  // 全量附加头(日志脱敏字段豁免)
        size_t maxBodyBytes = 100 * 1024 * 1024;   // 普通请求整包缓冲护栏(100MB 默认)
        bool   outboundAccessLog = true;   // 出站访问日志开关
        // —— 流式下载通道 ——
        bool   enableDownload = true;      // 是否创建下载通道
        size_t downloadChunkBytes = 1 * 1024 * 1024;  // 读回调单次写盘分块
        size_t downloadStallAbortMs = 120 * 1000;     // 对端停滞/无写进展放弃
        size_t downloadWriteMaxMs = 200;    // 单次写盘耗时超限 → abort(防慢盘卡死通道 loop)
    };

    static bool Init(const Options& opts);
    static bool IsReady();
    static void Close();

    // —— 协程(推荐;resume 线程默认 = 客户端 lane loop,可经 opts.resumeLoop 回环) ——
    static drogon::Task<ZmHttpResult> SendCoro(drogon::HttpMethod m, const std::string& url,
        const ZMJSON& body = {}, const ZmHttpRequestOptions& = {});
    static drogon::Task<ZmHttpResult> GetCoro(const std::string& url,
        const ZmHttpRequestOptions& = {});
    static drogon::Task<ZmHttpResult> PostJsonCoro(const std::string& url, const ZMJSON& body,
        const ZmHttpRequestOptions& = {});
    static drogon::Task<ZmHttpResult> PostFormCoro(const std::string& url,
        const std::map<std::string, std::string>& fields, const ZmHttpRequestOptions& = {});
    static drogon::Task<ZmHttpResult> UploadCoro(const std::string& url,
        const std::string& filePath, const std::string& field, const ZmHttpRequestOptions& = {}); // multipart 手拼(见 §9)

    // —— 回调(异步兼容) ——
    static void SendAsync(drogon::HttpMethod m, const std::string& url, const ZMJSON& body,
        std::function<void(ZmHttpResult)> cb, const ZmHttpRequestOptions& = {});

    // —— 同步(仅限业务线程/自持工作池;所有已登记 loop 线程一律拒绝,见 §12) ——
    static ZmHttpResult SendSync(drogon::HttpMethod m, const std::string& url,
        const ZMJSON& body = {}, const ZmHttpRequestOptions& = {});

    // —— 流式下载(本期) ——
    struct ZmDownloadResult { bool ok; int status; uint64_t written; std::string error; };
    static drogon::Task<ZmDownloadResult> DownloadCoro(const std::string& url,
        const std::string& destPath, const ZmHttpRequestOptions& = {});
    // 断点续传:自动取 .part/.meta 判定 Range 起点,服务端校验不符随时回退 0(见 §10.3)

    // —— 诊断/统计 ——
    static std::string DumpStats();   // per-target: 请求数/错误分类/字节/重试率
    static void ResetStats();
};
```

***

## 6. 连接池(ZmHttpConnectionPool)

* **key 规范化**:`scheme://host:port`(隐式默认端口 80/443;IPv6 加 `[]`),小写 host。

* **loop 绑定**:所有连接显式绑定客户端自建 loop(`newHttpClient(host, port, ssl, loop,…)`);`normalLoopThreads>1` 时按 `key 哈希 % 线程数` 把 target 固定绑定到某一 loop(同 target 恒同 loop,连接可复用)。

* 结构:`std::mutex + unordered_map<key, Pool>`;`Pool` 持 N 条 `HttpClientPtr`(N=`maxConnPerHost`)。

* **惰性创建**:首个请求建池并建 1 条;使用时选 `requestsBufferSize()` 最小者;连接不足按需补建至 N。

* **锁外评估纪律(实测定界)**:`requestsBufferSize()` 在非 loop 线程是 queueInLoop + future.get() **整线程阻塞**——持池锁调用会与彼 lane 互等死锁;故**锁内仅做 map 结构的查/建/补/驱逐**,busy 评估一律锁外,且先**锁内拷贝 clients 快照**(shared_ptr 列表)、锁外对快照只读求值(避免与锁内补建并发读写 vector)。

* 断线不感知:由 drogon 下次发送自动重连,池不维护保活状态(保持简单);**连接失败换连接**(补充机制,见 §7.1):状态机将连接层失败(无响应)的 client 标记,下一跳经 `GetPoolClient(tgt, avoid)` 规避——**单连接目标时移除将死连接并新建替换**。

* 删除语义:池无业务注册/注销(目标不固定则池可无限增长)→ 提供容量上限守护(`maxTargets`,默认 256),超限逐出**"创建序最旧"**者(在建新 entry 时执行;**不复查 idle**——持锁查 busy 会死锁;安全性由 shared_ptr 全员持有 + drogon 内部 shared_from_this 回调链保证,在飞请求不断链);`Close()` 清空。

**安全**:URL 解析失败/畸形 → `BadServerAddress` 直返不建池;目标白名单/内网封禁属业务层(宿主服务侧)职责,本库不内置 SSRF 防护。

***

## 7. 请求编排:超时 / 重试 / 重定向

### 7.1 协程内部流程

> **实现形态(2026-09-02 修订)**:以下"流程"逻辑保持原语义,但**实现为堆上 `ZmSendMachine` 回调状态机**(重试/重定向/多尝试循环由 drogon 响应回调驱动推进;协程侧仅 `ZmMachineAwaiter` 薄桥,帧内无复杂值对象——MSVC 协程帧崩溃根治,见 §4.3 帧隔离纪律)。`SendSync/SendAsync` 直接发状态机(无协程)。**重定向跳间零延迟**;连接"半关闭窗口"(drogon 1.9.13 客户端 keep-alive 与服务器实际断连失步)由**连接失败换连接**机制兜底:连接层失败(无响应)将上次 client 标记 `lastFailedClient`,下一跳 `GetPoolClient` 规避之(单连接目标时移除并新建替换)——事件化替代早期"20ms 定时缓冲"方案(drogon 1.9.13 源码核实其只是赌"EOF 及时到达",不可靠且冤罚 keep-alive)。

```
SendCoro(m,url,body,opts)
 1 解析并规范化 target → 从池取 client(惰性建)
 2 co_await client->sendRequestCoro(req, timeoutSec)   // 每个尝试用新 HttpRequestPtr
 3 catch drogon::HttpException → err 分类(Timeout/NetworkFailure/...)
 4 需重试? → 指数退避 → goto 2
 5 30x 且 followRedirect? → 解析 Location(相对/绝对/跨 scheme;300 不跟,301/302 转 GET)→ goto 1(剥敏感头)
 6 组 ZmHttpResult(记录 retries/followedRedirect/finalUrl/elapsed;重定向+重试合计预算 maxTotalAttempts 超限即停)
```

注:**恢复线程**默认 = 客户端 lane loop(触发回调者);`opts.resumeLoop` 非空时经自定 awaiter 包装 `sendRequestCoro`(回调内 `resumeLoop->queueInLoop(handle.resume)`),协程最终恢复到该 loop——供线程亲和业务(如服务器 handler 直返响应)使用。

### 7.2 重试规则(对齐既有"指数退避"讨论与主流语义)

* **可重试错误**:`Timeout`、`NetworkFailure`;HTTP `5xx`(502/503/504 优先)与 `429`(限流);`BadServerAddress` / TLS 类错误 / 其余 `4xx` **不重试**。

* **可重试方法**:GET/HEAD/PUT/OPTIONS/TRACE 天然幂等;DELETE 计入(可重放语义);POST/PATCH 仅当 `opts.idempotent=true`(业务显式声明,自行保证重放安全)。

* **退避公式**:`delay = min(base · 2^n, cap)`,加均匀抖动 `±jitter`;默认 base 200ms、cap 3s、最多 3 次;`5xx`/`429` 响应带 `Retry-After` 时**优先尊重**(整秒或 HTTP-date 两种格式,与 cap 取 min;格式不可解析则退回指数退避)。

* **睡眠执行**:循环重试间的延时不在事件循环忙等——协程内 `co_await` 本客户端 loop 的定时器,或经客户端自持工作池睡(不依赖服务器 `RunOnPool`)。

* 每次尝试**必须新建** `HttpRequestPtr`(drogon 改动 req;body 换新重发,不可复用旧对象)。

### 7.3 重定向

* 跟随 `30x` 的 `Location`(301/302/303/307/308;**300 不自动跟随**——主流客户端亦不跟,返回结果);301/302 对非 GET/HEAD **转 GET 丢 body**(对齐 curl 语义);303 强制转 GET;307/308 保留方法与 body。

* **跨域跳转**(scheme/host/port 任一变化)剥除 `Authorization`、`Cookie`、自定义敏感头(主流安全语义)。

* 达 `maxRedirects` 停止;重定向+重试合计不超过 `maxTotalAttempts`(互跳死循环防线),超限返回**最后一跳结果**并置 `followedRedirect`;不把重定向视为错误。

***

## 8. TLS 策略

* **校验开关全局唯一**:`validateCert`/`trustCA`/`clientCert`/`clientKey` 全部在 `Options`(CLIENT 级创建时固化;池化共享实例**不提供逐请求覆盖**——`ZmHttpRequestOptions` 已无该字段)。业务确有"单请求免校验"需求时,走专用 no-validate client(不进通用池),本期不实现。

* 内网自签追加信任(`trustCA`):

  * **下载通道**:`TLSPolicy::defaultClientPolicy(hostname)` + `setCaPath(trustCA)`(确证可用;保持 `useSystemCertStore(true)` 不变,"系统库 + 追加"语义)。

  * **普通 lane**:**已定案(2026-09-02 实测)**——本捆绑 drogon 1.9.13 client 的 `addSSLConfigs` 注入 CAfile 不生效(`-CAfile` 静默无效仍 `InvalidCertificate`;`CAfile + VerifyMode` 变体直接 trantor FATAL),**不执行注入,仅启动告警**。内网自签走**全局 `validateCert=false` 应急**(出站访问日志带 `insecure` 标记,见 §11;正/负对照已实测:开校验→ InvalidCertificate,关校验→ 200)。

  * 应急:全局 `validateCert=false` 可用,但出站访问日志必须输出 `insecure` 标记(见 §11)。

* TLSPolicy 陷阱:`setUseSystemCertStore(false)` + 仅 `setCaPath` → **完全不做任何校验**(头文件注释警告);正确姿势 = 保持系统库再追加 caPath。

* mTLS:`clientCert/clientKey` 普通 lane 经 `setCertPath`,下载通道经 TLSPolicy `setCertPath/setKeyPath`;同 client 级固化,与服务器全局证书完全解耦。

* 版本:默认 `useOldTLS=false`(禁 TLS1.0/1.1),与服务器安全基线一致。

* 证书校验失败 → `InvalidCertificate` / `HandshakeError` 分类返回,不参与重试(§7.2)。

***

## 9. JSON / 表单 / 上传

* JSON:请求 `ZMJSON → ZmHttpServer::ToDrogonJson → setBody + application/json`;响应按 `Content-Type` 判解析,复用 `FromDrogonJson`,`ZmHttpResult::Json()` 返回 ZMJSON。

* 表单:`application/x-www-form-urlencoded`,`PostFormCoro` 内联编码(UTF-8 百分号编码)。

* 上传:**手拼 multipart**(捆绑版无客户端 Multipart API——`MultiPart.h` 仅服务端 `MultiPartParser`/`HttpFile`,`HttpRequest` 无 `setBody(MultiPartPtr)`):随机 `boundary`(**与 body 字节做无碰撞校验**,碰撞则换 boundary)+ 各 part 的 `Content-Disposition` 头组装进 `std::string`,`setBody()` + `Content-Type: multipart/form-data; boundary=...`。拼装与读文件在客户端自持工作池执行(读盘离核);仅支持小于 `maxBodyBytes` 的常规文件;超大文件走 §10 流式下载的反向场景,本期不实现流式上传出站。

***

## 10. 流式下载(本期交付,trantor 自研通道)

### 10.1 为什么自研

drogon 1.9.13 `HttpClient` 无流式读取 API(整包缓冲),大文件直接 OOM 风险;任务为**边收边落盘 + Range 续传**。

### 10.2 通道与实现

```
ZmHttpDownloadChannel(自建 HttpClient-DL:trantor::EventLoopThread,Init 时启动)
 ├ trantor::Resolver(dlLoop,c-ares):域名 → InetAddress(**TcpClient 只收 IP**,异步,禁同步 getaddrinfo)
 ├ TcpClient(dlLoop, addr):enableSSL(TLSPolicyPtr,须在 connect() 前) + setConnectionCallback
 │   + setMessageCallback(读回调→直写盘) + setSSLErrorCallback/setConnectionErrorCallback
 ├ 请求:手写 HTTP/1.1 GET(含 Host/UA/Range/普通头;显式 Connection: close、Accept-Encoding: identity)
 └ 下载编排:dlLoop 线程直写盘(无队列/无写线程;单块=一次顺序写 syscall)
```

* **读回调直写盘**(方案 A):状态行 + 头解析确认 `200/206` 后打开 `destPath + ".part"` 文件句柄;读回调缓冲累积至 `downloadChunkBytes` 粒度**一次顺序写**,**完成判定前 `FlushPending()` 收尾落盘残余字节**(实测缺陷:仅按收包计数会滞留未落盘——2026-09-02 修复)。**这是对"事件循环绝不磁盘读写"的定向豁免**——该 loop 是专属通道,与铁律初衷(公用循环禁被盘卡死)不冲突,风险由两条时间线兜底:**单次写盘耗时 >** **`downloadWriteMaxMs`(默认 200ms)或停滞 >** **`downloadStallAbortMs`(无数据/无写进展)** → 立即 abort 止损(防网络盘/对端停发卡死通道 loop)。

* DNS:connect 前经 `trantor::Resolver::newResolver(dlLoop)` 异步 resolve(解析超时走 Resolver 自身 timeout);解析失败/超时 → `BadServerAddress` 终结。**实测坑(spike 2026-09-02):Resolver 回传的 `InetAddress` 端口恒为 0**,必须在 resolve 回调里按 URL scheme/显式端口回填(`setPortNetEndian(htons(port))`)再 connect。

* 请求构造:手写 HTTP/1.1 GET,行 = `GET <path>?<query> HTTP/1.1`,头 = `Host`、`User-Agent`、`Range`(续传时)、`Connection: close`(否则 keep-alive 下"连接关闭即完成"永不自发)、`Accept-Encoding: identity`(防服务器返回压缩体)、普通头注入;对齐服务器已实现的 Range 语义。

* 响应解析:极简自研(仅所需字段):状态行(状态码)、`Content-Length`/`Content-Range`、`Transfer-Encoding`、`Location`;`100 Continue` 等临时状态行跳过。非法响应 → 错误终结不落盘;非 2xx/非 206 → 错误终结不落盘。

* 停滞与写超时守护见上;**abort 语义 = 停止并保留** **`.part`/`.meta`** **供下次续传**(仅 §10.3 "回退 0 重下" 时截断)。

* **进度回调:本期未暴露**(API 无进度参数;结果含最终 `status/written`,需要进度上报时后续在 `ZmDownloadResult` 或 Options 上扩展)。

* 落盘策略:先写 `destPath + ".part"`,全部完成后改名覆盖目标——**Windows rename 覆盖语义**:`MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`(`.part` 与目标同目录保证同卷;失败先删目标重试一次,再挂报错);与服务器上传临时文件后 rename 同一约定。

* 元数据 `.part.meta`(ZMJSON:url/etag/lastModified/offset):**续传时读取已存在的 meta,不主动创建/周期更新**;完成时删除;失败/中止保留(供续传)——**仅作起点提示,正确性始终以服务端 `Content-Range` 校验为准**(§10.3)。

### 10.3 Range 断点续传(分支表)

| 分支               | 条件                                                                     | 处置                                                                                                    |
| ---------------- | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 可续传              | 响应 `206 + Content-Range` 且首字节 = 请求 offset                              | 从 offset 续写                                                                                           |
| 服务器忽略 Range      | 响应 `200`(未发 Range 或不支持)                                                | `.part` **截断从头重下**                                                                                    |
| Content-Range 不符 | `206` 但首字节 ≠ 请求 offset                                                 | **自动截断从头重下一次**;仍不符(服务端异常)→ 错误终结 `BadResponse`(防数据错位——吸取服务器端 SendFileStreamCoro 未按 offset 定位导致续传错位的教训;**侧游标严格按"本通道已写入偏移"推进,绝不信任磁盘既有大小**) |
| 实体已变             | 携带 `If-Match: <meta.etag>` / `If-Unmodified-Since`,`412` 或 ETag/LM 不匹配 | 从头重下(旧 `.part` 作废截断)                                                                                  |
| 无 meta/无 .part   | 首次下载                                                                   | 起点 0,普通拉取                                                                                             |

* 起点确定顺序:调用侧显式传 `Range` 起点 → 读 `.part.meta` → 默认 0。

* 写游标与 `written` 计数一致(单 loop 直写驱动),保证多段续传无重叠/无空洞。

### 10.4 HTTP 无长度响应

`Transfer-Encoding: chunked` 或连接关闭式定界:按读取流自然结束(连接关闭)作为完成信号;无 `Content-Range` 时以 `Content-Length`/实际写入字节为 `total`。

***

## 11. 日志与统计

* **出站访问日志**(单行,对齐服务器访问日志风格):
  `[time][ZmHttpClient][out] METHOD target → status elapsedMs bytes retries [insecure?]`(敏感头值脱敏;全局 `validateCert=false` 应急时输出 `insecure` 标记)。

* **全局统计**(`DumpStats`,首版):`{requests, ok, badResponse, timeout, networkErr, bytesIn}`(请求数/成功/错误分类计数/响应体入字节)。**per-target 明细(重试率/平均耗时/收发字节)留后续版本**;流式下载量未纳入统计。

***

## 12. 边界与止损

* 不支持(本期):HTTP/2 客户端、代理(CONNECT)、出站 WebSocket、Unix socket、Cookie 会话池(默认 `enableCookies` 关闭)。

* 不内置 SSRF 防护(业务层宿主做目标白名单/内网封禁)。

* `maxBodyBytes` **业务护栏**(防"业务误读超大响应"误判,超限按 `BadResponse` 分类返回)——drogon 是"整包缓冲完才交付",**护栏不提供内存保护**,超限响应仍会先整包缓冲;预检手段 = 请求前 `Content-Length`(可被欺骗),大文件一律走流式通道。

* 同步形态 `SendSync`:仅限业务线程/客户端自持工作池;**所有已登记 loop(客户端各 lane + 服务器各 loop)** 线程调用一律拒绝(实现:loop 登记表 + `isInLoopThread` 检查;drogon 自身断言只保护客户端自身 loop,防不住"服务器 loop 线程被卡死")。**等待超时兜底**:`Close()`/异常下状态机可能永不回调 → `fut.wait_for(...)` 超时返回 `Timeout` + ERROR 日志,业务线程绝不久挂。上限 = `尝试数 ×(单次超时 + 退避上限)+ 10s`(尝试数 = `1 + 重试预算`,受 `maxTotalAttempts` 截断;`timeoutSec=-1` 不超时请求按 60s/次计)——**上限必须覆盖真实最坏耗时**,否则同步形态会比协程/异步更早报超时,同一请求三种形态结论不一致(默认配置下约 62s;不超时请求默认 4 次尝试下约 262s;要更短就调小 `timeoutSec`/`retryCount`)。

***

## 13. 验收清单

> **状态(2026-09-03)**:P0-P6 全部落地,本清单所列行为均已通过冒烟/压测验证(见 `docs\issues\2026-09-02-drogon-httpclient-issues.md` §1.10/§1.11);唯一留白 = mTLS 双向(服务端未配置客户端证书,待双方配置后补测)。

* [ ] `Init/Close` 幂等;未 Init 发送报错;`Close` 后终态。

* [ ] 协程/回调/同步三形态对同一接口行为一致(结果字段完整);SendSync 在已登记 loop 线程被拒、业务线程正常。

* [ ] 生命周期与服务器解耦:服务器未 Open/已 Close 时客户端仍可用(独立 loop 自持)。

* [ ] 连接池:并发压测下同 target 连接数 ≤ `maxConnPerHost`;不同 target 互不串扰;`normalLoopThreads>1` 时同 target 恒同 loop。

* [ ] 重试:模拟 `Timeout`/`NetworkFailure`/500,验证退避间隔、`retries` 计数、`Retry-After` 优先、非幂等默认不重试;重定向+重试合计 ≤ `maxTotalAttempts`。

* [ ] 重定向:302 跟随、跨域剥头、`maxRedirects` 截断。

* [ ] TLS:自签信任(trustCA,普通 lane 与下载通道**双路径**)、mTLS 双向、全局 `validateCert=false` 应急 + 日志 `insecure` 标记、`InvalidCertificate` 分类;`ZmHttpRequestOptions` 无 validateCert 字段(已删)。

* [ ] 流式下载:2GB+ 文件下载 SHA256 全对;**中断后续传**逐分支验证——(a) 206 正常续段无重叠/空洞;(b) 服务器忽略 Range 返回 200 → 截断从头;(c) `If-Match` 下 412 → 从头;(d) abort 后 `.part`/`.meta` 保留可续;2048 阈值大小文件双路径;`MoveFileExW` 覆盖已存在目标成功;单次写盘超 `downloadWriteMaxMs` 时 abort(慢盘模拟)。

* [ ] JSON 往返、表单编码、**手拼 multipart** 上传正确性(含 boundary 撞串换边界、多 part、二进制文件)。

* [ ] `resumeLoop`:服务器 handler 协程发起,`opts.resumeLoop=发起 loop` 后确认协程终段恢复于该 loop(客户端 lane 线程无残留)。

* [ ] 出站访问日志单行格式与脱敏(`insecure` 标记);`DumpStats` 计数准确。

* [ ] 铁律检查:**公面/普通 lane loop 零磁盘 IO**;下载通道 loop 直写盘但写超即 abort;大下载不抢占服务器三面事件循环与普通出站请求(独立双 lane);下载通道 DNS 无同步 getaddrinfo(代码审查)。

***

# 第二期设计(2026-09-07 增补)

> 状态:已定稿(2026-09-07 用户评审:TLS CA 注入**本期不做**,其余采纳)· 依据:docs/issues/2026-09-07-drogon-httpclient-code-review.md
> 未列出的行为沿用第一期;与第一期冲突处,**以第二期为准**。

## 14. 差异总览

| 维度 | 第一期 | 第二期 | 动机 |
| --- | --- | --- | --- |
| 续传事实源 | `.part.meta{url/etag/lm/offset}` | **`.part` 大小即 offset**;meta 仅 `{etag,lastModified}` 且一次写 | offset 与磁盘状态双源必不一致(评审 BUG-1/4) |
| 变更检测 | `If-Match` + 412 分支 | **`If-Range`**(不匹配返 200 全量) | 标准续传语义,分支表收敛 |
| 200 回退路径 | 断开重连重发 | **原连接续读**(响应体即全量,就地截断) | 消灭"重启被断开回调击杀"类缺陷(评审 BUG-2) |
| 写盘模型 | dlLoop 直写 + 单写 >200ms 即 abort | **每会话写线程 + 有界队列(默认 64MB)** | 容忍慢盘突发;内存有界;loop 零磁盘,撤销铁律豁免 |
| 响应解析 | 极简自研,宽松解析 | **fail-closed**(严格校验,歧义即报错) | chunked 自追加 UB、size 行误判 0 的土壤 |
| 生命周期 | 仅 `await_ready` 查一次 IsReady | **首跳异步化** + 状态机全程状态检查 | 消 Close 竞态(UAF/回退 app loop/协程永挂) |
| 失败连接 | 仅"下一跳规避" | 连接层失败**即移除替换**;Timeout 仅规避 | 死连接 busy=0 反被 least-busy 优先选中 |
| 显式 Range 起点 | §10.3 承诺,未实现 | **不提供**(起点恒 = `.part` 大小) | 减少错位面,协议只有一条合法续传路径 |
| TLS CA 注入普通 lane | trustCA 不生效 → 全局 `validateCert=false` 应急 | **本期不做**,维持 §8 现状(2026-09-07 决策) | — |
| 状态机(ZmSendMachine) | 回调状态机 + 薄 awaiter | 保留,补根因最小复现(§17) | 三形态复用价值成立;纪律升级为结构保证 |

## 15. 流式下载通道重设计

### 15.1 续传协议(.part 即游标 + 一次写 sidecar + If-Range)

**事实源唯一化**:`.part` 文件的现有大小就是"已落盘偏移"(N),不再持久化 offset。变更检测用 HTTP 标准头 `If-Range`(强 ETag 优先,无 ETag 用 Last-Modified;两者皆无则退化为仅 Content-Range 起点校验并记日志)。

meta 侧车保留文件名 `.part.meta`,Schema 瘦身:

```json
{"etag": "...", "lastModified": "..."}
```

**写入时机只有一次**:会话解析到响应头(或重连后的新响应头)时,经写线程(§15.2)落盘;完成时随 `.part` 一起改名收尾并删除 meta;错误/中止保留。**没有周期更新、没有 offset 字段**——评审 BUG-1(meta 无人写)与 BUG-4(offset 与磁盘不一致)结构性不存在。

**请求构造**(续传 N>0 时):

```
GET <path> HTTP/1.1
Host: <BracketHost(host)>[:port]
Range: bytes=<N>-
If-Range: <meta.etag>
Connection: close
Accept-Encoding: identity
```

**分支表**:

| 条件 | 处置 |
| --- | --- |
| 无 `.part`(N=0,全新下载) | 普通 GET;响应头解析后经写线程写 meta |
| `206` 且 Content-Range 首字节 == N | 唯一合法续传路径,续写 |
| `200`(If-Range 不匹配 / 对端忽略 Range) | **原连接续读**(响应体即全量):写线程按序 `TRUNCATE(0)` 后,后续 body 从 0 落盘;重写 meta;游标归 0。**不重连** |
| `3xx` 带 `Location` | **跟随**:解析 Location(绝对 / 协议相对 / 根相对 / 相对含 `.`·`..`)→ 换目标重连,续传语义保留(Range/If-Range 照发,由对端以 200/206 收敛);跨目标(scheme/host/port 任一变化)剥除 `Authorization`/`Cookie`/`Proxy-Authorization`,与普通通道同名单。上限 `maxRedirects`;`opts.followRedirect=false`、Location 缺失或超限 → `BadResponse` |
| `304` | 无响应体(本通道不发条件请求),`BadResponse` 终结 |
| `206` 首字节 ≠ N,或 `416` | 服务端异常:断开 → 就地截断 → **重连一次**(epoch 守卫,§15.4)发普通 GET;仍异常 → `BadResponse` 终结 |

原第一期 412 分支删除(If-Range 语义下"资源已变"表现为 200 全量;412 属服务器实现异常路径,归入末行处置)。

**响应体编码**:请求侧已声明 `Accept-Encoding: identity`;响应带 `Content-Encoding` 且非 `identity` 时**当场 `BadResponse`**——原样落盘会得到"下载成功但文件是压缩字节"的静默损坏。

### 15.2 写线程与背压(指令队列)

**每会话一个写线程**(与服务器上传侧每连接写线程同一模式),dlLoop **零磁盘操作**,撤销第一期"事件循环磁盘豁免"。

```
dlLoop(读/解析/编排)                写线程(唯一盘上执行者)
  OnRecv ─ retrieve → 节点入队 ──→  按 action 顺序执行:
  指令队列 deque<Action>            WRITE(chunk) / TRUNCATE(0)
  字节上限 downloadQueueMaxBytes    WRITE_META(json) / FINISH / ABORT
  (默认 64MB,入队超限 → ABORT)     FINISH:排空 → 关句柄 → MoveFileExW 覆盖
                                    → 删 .part.meta → Done(ok)
                                    ABORT:关句柄(保留 .part/.meta)→ Done(err)
```

- **读侧入队**:`MsgBuffer` 按 `downloadChunkBytes` 粒度切节点(一次内存拷贝),`retrieve` 后即返回,loop 无阻塞。
- **背压闭环**:写线程慢 → 队列积压 → 触顶(64MB)→ 会话 ABORT(保留 `.part`/`.meta` 可续传)。替代第一期"单次写 >200ms 即杀整个下载"——网络盘/机械盘合法抖动不再误杀,内存上界明确。
- **写线程卡死兜底**:对端网络共享类阻塞 `WriteFile` 可能久挂,ABORT 时由 dlLoop 对句柄 `CancelIoEx` 解除阻塞,写线程走失败路径退出,保证 Done 必达。**被取消的写不算磁盘故障**:写线程识别 `ERROR_OPERATION_ABORTED` 后按通道真实原因回执(`Fail` 回填的原因或"通道关闭"),不让"写盘失败"顶掉真实原因。**回填原因必须早于 `CancelIoEx`**(且与写线程读它走同一把队列锁):被唤醒的写线程会立刻取该原因,晚一步就只剩兜底文案。
- **停滞守护**:`downloadStallAbortMs` 内读侧无收包 **且写侧无进展**(队列已空,或写线程已同样久未取走指令)→ ABORT。写侧判据必须能独立成立:写线程挂在网络盘上时队列恒非空,只看"队列空"会让会话永挂。
- **完成时序**:完成判定(收满/终结 chunk/连接关闭)在 dlLoop,入队 `FINISH` 后由写线程确认**队列排空 + 句柄关闭 + 改名成功**才回调——"残余字节未落盘"类缺陷在结构上不可能复发。
- **Done 回调线程 = 写线程**(协程侧经 `resumeLoop` 亲和,与普通 lane 同约定)。
- **文件打开**:`StartDownload` 把"stat `.part` 定 N → 读 sidecar → `CreateFileW` + 定位/截断 → 起写线程"整段提交**客户端工作池**(`ZmHttpClient::SubmitBlockingTask`)执行——调用方通常是服务器事件循环线程,磁盘操作不得落在其上;受理结果同步返回,会话级失败经 `done` 回执。重连场景的新文件句柄由新连接的会话流程同样处理。
- 并发度预期:同时下载数通常个位数,每会话一线程可接受;不做共享写线程池。

### 15.3 响应解析器 fail-closed

- chunk size 行**必须全为 hex** 且整行消费,否则 `BadResponse` 终结;单个 chunk 声明长度 > `downloadQueueMaxBytes` → `BadResponse`(防内存无界);
- 数据块之后的两个字节**必须真是 CRLF**(不是只校验"长度存在"),否则 `BadResponse`——否则残留字节会被当作下一个 size 行解析;
- 头部行数/总长上限(256KB 保留);重复 `Content-Length`/`Transfer-Encoding` 冲突 → `BadResponse`;
- `Content-Range` 解析失败按"无"处理,但 **206 无有效 Content-Range → `BadResponse`**(206 必须可验证首字节);
- 状态行版本/格式畸形 → `BadResponse`;`1xx` 临时响应跳过保留。

### 15.4 生命周期与并发

- **连接代数(epoch)守卫**:每次(重)连接 `epoch_++`;OnConnected/OnRecv/OnDisconnect/OnConnectError/OnSslError 捕获注册时的 epoch,与当前不符即忽略——重连窗口内旧连接回调不再击杀会话(评审 BUG-2 的结构解,与 §15.1"200 原连接续读"双保险)。
- **受理与停止的原子性**:`StartDownload` 的磁盘段在工作池执行,完成后**在同一把 `s_dlMtx` 锁域内**复查 `s_dlRunning` 并投递首跳——复查与投递必须同锁域,出锁后 `Shutdown` 可能已把 loop 连同线程一起销毁;`Shutdown` 收尾时再对残留登记补叫停一轮。两侧合力保证"已受理的会话必然被叫停",不存在"逃过 Shutdown 快照、写线程永远等不到 `writerStop_`"的会话。
- **回调持弱引用**:会话的连接回调与解析回调一律持 `weak_ptr`(锁不上即丢弃),会话存活由登记表 `s_sessions` 锚定到 `Deliver` 摘除为止——强引用会与 `client_`/`resolver_` 成员构成**引用环**,令会话连同写线程、文件句柄永久泄漏。
- 通道沿用单 `EventLoopThread`;`Shutdown` 顺序:停收新任务 → 快照在飞会话 + 对在途写句柄 `CancelIoEx` → 等登记表排空(≤3s)→ quit + join dlLoop → 残留登记补叫停后清表。
- 会话对象由 dlLoop 与写线程双方 shared_ptr 持有,终结路径(正常/错误/通道关闭)都收敛到"写线程 Done 必达一次"。

## 16. 普通通道修订(修缺陷,不改架构)

1. **路径编码**:`req->setPathEncode(false)`(drogon urlEncode 不保留 `%`,双重编码);代价是 ParseUrl 收紧——path 仅允许 pchar/`/`/`?`/`=`/`&` 白名单,裸空格/控制字符 → `BadServerAddress`。
2. **生命周期竞态**:`NextAttempt`/`GetPoolClient` 入口查 `IsReady()`,未就绪 → `NetworkFailure` 终结(不再新建池/回退 app loop);**首跳异步化**(Start 把首跳 `queueInLoop` 到 lane loop)→ `await_suspend` 栈上不存在同步 Finish,薄桥"resume 最后一条语句"纪律升级为结构保证;`DownloadAwaiter::await_suspend` 处理 `StartDownload()==false` 同步收尾。
3. **失败连接处置**:`NetworkFailure`(无响应)→ 该 client 从池中**移除**(锁内),池空则新建替换;`Timeout` 仅下一跳规避不移除(慢≠坏)。
4. **Init 失败回滚**:lane pool 启动超时 → 删池、注销 loop、置回 Uninit,杜绝"假 Initialized"砖化。
5. **multipart**:filename 转义——剔除控制字符、quoted-string 转义 `"` 与 `\`;**非 ASCII 以 UTF-8 原样入 quoted-string,不采用 RFC5987 `filename*`**(决议 2026-09-08:multipart 接收端是 drogon `MultiPartParser`,不识别 `filename*`,追加该参数反使自家服务器丢文件名;与服务端 `MakeContentDisposition` 用 RFC5987 不矛盾——下载的接收端是浏览器,生态不同);boundary 对**组装后完整 body** 查碰撞。
6. **IPv6**:下载通道 Host 头与普通 lane 统一 `BracketHost`。
7. **RegisterLoop 接线义务**:`ZmHttpServer::Open()` 自动把三面 loop 注册进客户端登记表(两模块双向写明协议义务);SendSync 的 loop 拒绝面自此全覆盖。
8. **统计**:新增 `status4xx` 计数;`ok` 语义文档明确为"传输成功"。
9. **头值白名单**:头名沿用严格白名单(裸空格/控制字符 → 拒绝);**头值只拒 CR/LF/NUL 等控制字符**,空格与制表符合法——原实现把头名判据套在头值上,`Accept: text/html, */*`、`Content-Type: application/json; charset=utf-8` 一类合法值会让整个下载通道失败(普通通道走 drogon 无此限制,同一份头配置两边行为不一致)。
10. **Init 失败回滚注销 loop**:lane pool 启动超时除删池外,须把已登记的 loop 逐一出表(否则 `IsLoopThread` 解引用悬空指针)。
11. **multipart 字段名转义**:`name="..."` 与 `filename="..."` 共用同一 quoted-string 转义(剔控制字符、转义 `"` 与 `\`);boundary 碰撞校验改在**转义后**的字节上做。
12. **下载收尾不做"删目标重试"**:`MoveFileExW` 失败即回执错误并保留 `.part`。原退路先删用户既有文件、二跳再失败即双输。
13. **下载错误分类保真**:被 `CancelIoEx` 解除的写按通道真实原因回执(见 §15.2)。
14. **User-Agent 归属**:UA 由 `Options.userAgent` 统一决定,业务经 `headers`/`commonHeaders` 传入的 User-Agent **不生效**(drogon 发送时以 client 级 UA 覆盖同名头;UA 是 client 级状态,同一 client 无法按请求区分)。
15. **中文路径**:`.part`/`.part.meta` 的读写路径一律 CP_UTF8 转宽后打开(窄串直接进 `fstream` 在中文路径下静默失败,续传会丢掉 If-Range 校验值)。
16. **阻塞任务出口**:新增 `ZmHttpClient::SubmitBlockingTask(task)`——磁盘 IO 等阻塞任务提交到客户端自持工作池(工作池为进程级终态,已受理任务必然执行);事件循环线程不得直接做磁盘操作。

## 17. 状态机根因验证计划(实施前必做,半天)

"协程帧内复杂值对象 + 跨线程 resume → 0xC0000005"的现行归因,与"跨线程 resume 堆上帧本身不撕裂内存"的语义认知冲突,疑似真实根因是**帧已销毁后 resume / 二次 resume** 的生命周期 bug。最小复现工程(独立小工程,三用例):

- C1:帧内 `std::map`/`std::string`,drogon 回调线程正常跨线程 resume(控制组);
- C2:同样帧,人为制造 resume-after-destroy(预期崩,验证根因形态);
- C3:同样帧,二次 resume(预期崩)。

**决策门**:C1 稳定不崩 → 归因改写,纪律从"帧内禁复杂对象"放宽为"禁止 resume-after-destroy/二次 resume"(可工具强化);C1 也崩 → 维持第一期工具链结论,状态机与纪律不动。无论哪个分支,`ZmSendMachine` 分层保留(三形态复用),差异只在纪律的性质与可检测性。

**实施记录(2026-09-08,决策门已出)**:复现工程 `AI测试用目录\coro_frame_repro`(纯 C++20 协程,Task 语义对齐 drogon:急切启动 + final 挂起帧由持有者析构)。结果:

- **C1 控制组:20000 轮 × 5 次(1/4 worker 各验证)全绿,ok=20000/20000,exit=0——帧内持 map/string 的跨线程 resume 稳定不崩**;
- C2(resume-after-destroy)→ 0xC0000374 堆损坏;C3(double-resume)→ 0x80000003——预期崩,根因形态吻合。

**结论:第一期"MSVC 工具链必崩"的归因不成立,真实根因是生命周期误用(resume-after-destroy / 二次 resume / destroy 与 resume 竞态)。纪律改写为:帧内可以持复杂值对象;禁止对已销毁/已完成帧 resume;禁止 destroy 与 resume 并发。** `ZmSendMachine` 分层按本节约定保留(三形态复用),§16.2 的"首跳异步化"仍实施(消灭 await_suspend 栈上同步 Finish 的结构隐患)。(注:复现中途一版 vector\<Task\> 持有骨架曾 3/3 崩,系该骨架自身持有方式存疑,已废弃,不作为工具链结论依据。)

## 18. Options / API 变更

| 项 | 变更 | 说明 |
| --- | --- | --- |
| `Options.downloadWriteMaxMs` | **删除** | 由队列上限替代(慢盘不再按单写耗时杀) |
| `Options.downloadQueueMaxBytes` | **新增**,默认 64MB | 写侧背压硬上限,触顶 ABORT(保留续传) |
| `Options.downloadStallAbortMs` | 语义收窄 | 仅读侧停滞(无收包且队列空) |
| `DownloadCoro` 显式 Range 起点 | **不提供** | 起点恒 = `.part` 大小,减少错位面 |
| `ZmDownloadResult` | 增 `resumedFrom`(N>0 时) | 可观测性 |
| `.part.meta` Schema | `{url/etag/lastModified/offset}` → `{etag,lastModified}` | 一次写,无 offset |
| TLS 相关 Options | 不变 | 二期不做 CA 注入(§14) |
| 其余 API(SendCoro/Async/Sync/Upload 等) | 不变 | — |

## 19. 第二期验收清单

> 回归载体:`ZiMoTest\ZmHttpClientTest`(本地桩服务器 + 真实客户端,42 项断言;覆盖范围与未实测项见 `docs/issues/2026-09-13-drogon-httpclient-review-fixes.md` §五)。运行注意:`drogon::Task` 为懒启动(入口用 `drogon::AsyncTask`),主线程 EventLoop 在静态初始化期已由 drogon 建立,须复用而非新建。

- [ ] **续传真实分支(不预置任何 sidecar)**:(a) 中断 → 重下走 206,无重叠/空洞;(b) If-Range 不匹配(服务端资源已变)→ 200 原连接续读 + 就地截断,成品正确;(c) 服务器忽略 Range → 同 (b);
- [ ] 206 首字节不符 / 416 → 重连一次(epoch 守卫生效,旧连接回调不击杀会话),仍异常 → `BadResponse`;
- [ ] sidecar 一致性:手工截短 `.part` → 下次续传起点 = 截短后大小;手工删 sidecar → 退化普通下载;
- [ ] 慢盘/大队列:人为写阻塞 → 队列触顶 ABORT 保留 `.part/.meta` 可续;单次写耗时不触发误杀;
- [ ] 解析器:非 hex size 行 / 206 无 Content-Range / 头冲突 / chunk 数据块后非 CRLF / `Content-Encoding: gzip` → `BadResponse`,不落盘;
- [ ] 下载重定向:301/302 换目标跟随、跨目标剥敏感头、`maxRedirects` 截断、`followRedirect=false` 不跟随、`3xx` 无 Location → `BadResponse`;
- [ ] 头值含空格可正常下载(`Accept: text/html, */*`、`charset=utf-8`),头名含空格仍拒绝;
- [ ] 中文 `destPath`:`.part.meta` 可读回、If-Range 正常发出;无 ETag/LM 的资源发 WARN 日志且有响应;
- [ ] 429:命中重试,`Retry-After` 整秒与 HTTP-date 两种格式均被尊重(与 cap 取 min);
- [ ] `SendSync` 默认配置下不早退:同请求三形态结论一致(等待上限按尝试数计);
- [ ] URL:`%20`/中文转义逐字节到服务端(echo 验证);裸空格 → `BadServerAddress`;
- [ ] Close 竞态:Close 与在飞协程发送/下载并发 → 无崩溃,协程有终态;Close 后新请求快速失败;**Close 期间受理的下载必被叫停**(写线程全部退出、无会话泄漏、Close 不阻塞);
- [ ] 失败连接:死连接被移除,后续请求不再命中;Timeout 不移除;
- [ ] Init 失败回滚:lane 启动超时 → `IsReady()==false`,可重新 Init;
- [ ] multipart:含引号/CRLF 文件名上传,服务端解析正确;
- [ ] IPv6 目标:普通 lane 与下载通道 Host 头格式正确;
- [ ] RegisterLoop:服务器 handler 线程调 SendSync 被拒(接入后回归);
- [ ] 根因复现:§17 三用例出结论,纪律文档随结论更新;
- [ ] 既有回归:2GB+ SHA256、重试矩阵、重定向跨域剥头、multipart 上传、`MoveFileExW` 覆盖、chunked 下载逐字节一致。

