# DrogonHttpClient 开发计划(v0.4)

> 关联设计:`docs/designs/2026-09-01-drogon-httpclient-design.md`(v0.2,2026-09-02 评审修订)
> 日期:2026-09-02 · 状态:待审阅
> 目标:按设计文档实现出站 HTTP/HTTPS 客户端(门面 + 连接池 + 三调用形态 + TLS + 流式下载直写盘),通过 §13 验收清单全部勾选。

***

## 0. 里程碑总览

| # | 里程碑 | 内容 | 预计(人日) | 前置 |
| - | - | - | - | - |
| P0 | 设计定稿 | v0.2 文档(已完成,待审阅) | - | - |
| P1 | 类型与骨架 | 公共类型 + 静态状态机 + vcxproj 接线,可编译 | 1 - 2 | P0 |
| P2 | 普通 lane 可用 | 连接池 + SendCoro/Async/Sync + 重试/重定向/超时 | 5 - 7 | P1 |
| P3 | TLS/JSON/表单/上传 | TLS 全局接线 + 手拼 multipart | 3 - 4 | P2 |
| P4 | 流式下载通道 | Resolver + TcpClient 直写盘通道 + 续传 | 6 - 9 | P1(可与 P2 并行) |
| P5 | 集成自测 | 全量验收 + curl 对照 | 3 - 4 | P3 + P4 |
| P6 | 收尾 | 文档勾选、代码审查、记忆归档 | 1 | P5 |

**合计约 19 - 27 人日**。建议顺序 P1→P2→P3→P4→P5→P6;P4 与 P2/P3 可并行(仅争 P1 的类型头文件)。

> **进度**:P1 ✅(2026-09-02,可编译)。**P4 spike ✅ ALL PASS(2026-09-02)**——MoveFileExW 覆盖语义 / Resolver 异步 DNS(**坑:回传端口恒 0,须回填**)/ TLSPolicy 客户端握手(enableSSL 先于 connect)·isSSLConnection 确认 / 手写 GET + Connection: close 收流(`{"pong":true}` 200)四点全部验证;spike 落点 `A:\ZiMo\temp_httpclient_spike\`(验证后即弃)。
> **P2 主体 ✅(2026-09-02,验收方式:cmd 运行 RC=0,git-bash 下 127 为环境假象)**——连接池(哈希绑 lane/least-busy/LRU-idle 逐出)/三形态/超时/重试(Retry-After 优先、预算)/重定向(300 不跟、301/302→GET、跨域剥头)/resumeLoop 回环(SwitchThreadAwaiter 上游语义)/统计日志;目视证据:T1 GET200+Json().pong / T3 301→443 跟随 finalUrl ✓ / T4a laneloop 线程拒绝 / T4b 业务线程 200 / T5 并发×10 回包 200(T4b/T5 断言经 RC=0 确认)。
> **实测坑(已固化进代码注释/设计文档)**:① EventLoopThreadPool::start() 非阻塞,getLoop() 可能返回 nullptr → newHttpClient 回退 app() loop → trantor FATAL"already an EventLoop"——Init 必须轮询等待 lane loop 就位;② libevent 库实际在 `lib\VC\x64\MT`(旧记忆的 MD 有误);③ 冒烟自缢教训:lane 线程上做同步 wait_for/长 sleep 会冻死同 lane 请求(设计 §4.3 纪律)。
>
> **📌 2026-09-02/03 里程碑完成**:P1-P5 全部落地——崩溃根治(§issues)、普通 lane 全功能、**P4 流式下载通道**(T5a 逐字节+T5b 206 续传)、**P5 集成自测 ✅ 2026-09-03**(§1.11:重试矩阵/跨域剥头回显/2GB SHA256/护栏;临时端点已摘除);P6 ✅(勾选记录在 issues 文档)。唯一留白:mTLS 双向(服务端未配 client auth)。
> **📌 2026-09-02 崩溃根治 + 二次评审加固(本轮)**:0xC0000005"第二请求即崩"已按问题文档 §一 定界→**状态机重写(ZmSendMachine 回调状态机 + ZmMachineAwaiter 薄桥)+ 池双纪律(requestsBufferSize 锁外/快照)+ onDrogon 连接窗口 20ms 重定向缓冲**;二轮评审后追加:②协程桥帧内彻底 8 字节化(`ZmMachineCtx` 堆化,值为零帧内复杂对象);④ `SendSync` 超时兜底(Close/坠飞不永挂)、`kRedirectHopDelaySec` 常量、注释纠正("线程无关"→线程安全理由、"MSVC 合并优化"机制猜测改为未确证、错别字)。**当前状态:以 P3Smoke 12 轮压测 ALL PASS ×12 验证通过(2026-09-02)**;详见 `docs\questions\2026-09-02-drogon-httpclient-issues.md`(已同步更新)。

***

## 1. 前置环境(已验证事实,直接沿用)

* drogon 1.9.13 捆绑版已含 `HttpClient/trantor::TcpClient/TLSPolicy/Resolver/EventLoopThreadPool`,**无新增第三方依赖**;链接清单不变。
* 新文件加入 `LibZiMoPublic.vcxproj`(与 `zm_net_http_server.*` 同组,约 +3 条目 × 2 配置)。
* **MSBuild 增量坑**:公共库源码改动后,旧 lib 可能残留被误链——按记忆处置:改完后 `/t:Rebuild` 或删 `ZiMoPublic\Release_LibZiMoPublic\temp\*` + 确认 `temp_build_out\LibZiMoPublic.lib` 时间戳。
* 自测对端:沿用 `A:\ZiMo\temp_serve_test\`(exe + www + certs);自签证书生成注意 git-bash 双斜杠 `-subj "//CN=localhost"`。
* 注意:本地 debug 模式不可用,自测走 `sc start ZM_Svc` 流程(改/恢复 binPath)。

***

## 2. 阶段任务分解

### P1 类型与骨架(1-2 天)

**文件**:`ZiMoPublic\net\zm_net_http_client.h/.cpp`、`zm_net_http_pool.h/.cpp`(空壳)、`zm_net_http_client_download.h/.cpp`(空壳)。

**内容**:
1. `ZmHttpClient::Options/ZmHttpRequestOptions/ZmHttpResult`(按 §5 定义,`validateCert` 仅存在于 Options)。
2. 静态状态机 `Init/Close/IsReady`;Init 创建(但 P2 前可不启 loop)并登记 loop 管理区;Close 三步序按 §4.1。
3. **loop 登记表骨架**(客户端 lane + 留服务器 loop 注册接口):供 SendSync 拒绝与 Close 自锁检查。
4. 项目约定:命名正式化、guard `ZM_NET_HTTP_CLIENT_H` 等、Vcxproj 接线。

**验收**:可编译;Init/Close 幂等;未 Init 调用 `SendCoro` 返回 `BadServerAddress`(或断言)不崩溃。

### P2 普通 lane:连接池 + 三形态 + 超时/重试/重定向(5-7 天,核心)

**内容**:
1. `HttpClient-Loop`:`trantor::EventLoopThreadPool(normalLoopThreads, "ZmHttpClient-Lane")`,`start()/wait()` 生命周期随 Init/Close。
2. 池:key 规范化(`scheme://host:port`,默认端口/IPv6 `[]`/小写 host);`newHttpClient(host, port, ssl, loop,…)` 绑定;惰性建 → least-busy(`requestsBufferSize()`)→ 补建至 `maxConnPerHost`;`maxTargets=256` 超限逐出 **idle-only**;`normalLoopThreads>1` 时 key 哈希 `%N` 固定 loop。
3. `SendCoro/GetCoro/PostJsonCoro/PostFormCoro/UploadCoro(占位)/SendAsync/SendSync`:统一走内部"单尝试"原语;`resumeLoop` 经自定 awaiter 包装 `sendRequestCoro`(回调内 `resumeLoop->queueInLoop(resume)`);SendSync 用 promise/future 桥接 + 登记表拒绝。
4. 重试:幂等方法矩阵(§7.2);`Timeout/NetworkFailure/5xx(502/503/504)` 可重试;退避 `min(base·2^n,cap)+jitter` + **Retry-After 优先**;每次尝试**新建 req**;合计预算 `maxTotalAttempts`。
5. 重定向:301/302 非 GET/HEAD→GET 丢 body、303→GET、307/308 保体、300 不跟;跨域剥 `Authorization/Cookie/自定义敏感头`;`followedRedirect/finalUrl` 记录。
6. 超时:逐请求 `timeoutSec`(0=全局,-1=不超时)。
7. 出站访问日志/统计(§11)第一版。

**验收**:连接数 ≤ `maxConnPerHost`(压测脚本,如 50 并发假服务器);同 target 恒同 loop;重试/重定向模拟(见 P5 测试端点);SendSync 在登记 loop 被拒。

### P3 TLS 接线 / JSON / 表单 / multipart 上传(3-4 天)

**内容**:
1. TLS:全局 `validateCert`;`useOldTLS=false`;mTLS `setCertPath`(普通 lane)。
2. **trustCA 双路径**(§8):
   - 下载通道:`TLSPolicy::defaultClientPolicy(hostname)+setCaPath(trustCA)`(确证 API,P4 兑现)。
   - 普通 lane:`addSSLConfigs`(SSL_CONF_cmd CAfile)**先自测验证**;不可行 → 退路:trustCA 目标专用 client 实例(池 key 加 trustCA 指纹),并记入文档。
   - `validateCert=false` → 日志 `insecure` 标记。
3. JSON 复用 `ToDrogonJson/FromDrogonJson`;`ZmHttpResult::Json()` 判 `Content-Type`(application/json / `+json`),解析失败打日志返空对象。
4. 表单百分号编码;**手拼 multipart**(boundary 无碰撞校验、多 part、二进制段),工作池拼装;`UploadCoro` 落地。

**验收**:§13 TLS/JSON/表单/multipart 条目;用 `temp_serve_test` 自签 + curl -k 对照。

### P4 流式下载通道(6-9 天,风险最高,P1 后即可并行)

**内容**:
1. `HttpClient-DL`:单 `trantor::EventLoopThread` 自启自停;`trantor::Resolver::newResolver(dlLoop)` 异步 DNS(禁同步 getaddrinfo)。
2. `TcpClient`:`enableSSL(TLSPolicyPtr)` **在 connect() 前**;`setConnectionCallback/setMessageCallback/setSSLErrorCallback/setConnectionErrorCallback`。
3. 手写 GET:`Host/UA/Range/If-Match/Connection: close/Accept-Encoding: identity`;极简状态行+头解析(状态码/`Content-Length`/`Content-Range`/`Transfer-Encoding`/`Location`;`100` 跳过;chunked 终结)。
4. **直写盘**(方案 A):`200/206` 确认后打开 `.part`,读回调按 `downloadChunkBytes` 一次顺序写;**单次写 > `downloadWriteMaxMs`(200ms)或停滞 > `downloadStallAbortMs` → abort 保留 `.part/.meta`**。
5. 续传分支表(§10.3)全实现;`.part.meta` 读写;**`MoveFileExW(MOVEFILE_REPLACE_EXISTING)` 覆盖目标**。
6. 进度回调 `(written,total)`。

**验收**:2GB 文件 SHA256 全对;§10.3 分支逐项;写超时 abort(慢盘/模拟);`Connection: close` 与 chunked 场景。

### P5 集成自测(3-4 天,P2-P4 并行收尾)

**测试端点注入**(自测服务侧临时注册,验收后摘除):
* `/httpclient/ping` 200 JSON;`/httpclient/err/{code}` 按需返回 500/503/404;`/httpclient/redirect/{n}` 相对/跨 host 链;`/httpclient/slow?ms=`(超时);`/httpclient/cht-ok/cht-200`(Content-Type/Range 变异)。
* Range:优先用自家服务已验证的 Range 端点;若发现服务器 Range 仍有缺陷,先修服务器(本计划前置)。

**对照手段**:`curl -k https://localhost:443/...` 与客户端结果对照;`zb`/`GetType` 无 — 直接用 `sc start` + 日志。

**验收**:§13 全清单;资源占用(DumpStats/内存)观测。

### P6 收尾(1 天)

* §13 清单落地为验收记录;代码审查(对照 §12 边界);`docs/designs` 归档;memory 更新(zimo-drogon-server-notes 增录 client 要点)。

***

## 3. 风险与退路

| 风险 | 等级 | 退路 |
| - | - | - |
| 普通 lane trustCA(addSSLConfigs CAfile)未实测 | 中 | 退路 A:trustCA 目标专用 client 实例(池 key 指纹);退路 B:首交付仅下载通道支持 trustCA,普通 lane 文档标注"仅系统信任库" |
| 直写盘方案遇慢盘(网络盘)→ 频繁 abort | 中 | 调大 `downloadWriteMaxMs`;或文档限定"下载通道面向本地磁盘/快盘",网络盘接业务层重试 |
| 自研 HTTP/1.1 解析(状态行/头部边缘) | 高 | 解析失败一律"错误终结 + 保留 .part 可续";不尝试容错宽松解析 |
| 服务器端 Range/206 自测端点实际缺陷 | 中 | 先修服务器 SendFileStreamCoro 偏移 bug(设计 §10.3 已引用教训) |
| drogon 客户端自动解压导致 maxBodyBytes/统计口径差异 | 低 | 护栏与统计一律以 `resp->getBody()` 解码后字节计;文档 §12 已注明 |
| 协程跨线程 resume(默认客户端 lane loop) | 中 | 业务必须显式传 `resumeLoop`(服务器 handler 场景);集成自测含 §13 resumeLoop 项 |

***

## 4. 待审阅确认项

1. `maxTotalAttempts=8`、`downloadWriteMaxMs=200ms`、`retryCapMs=3000` 默认值。
2. 普通 lane trustCA 首交付策略:先试 addSSLConfigs,失败即走"trustCA 专用 client 实例"退路(见风险表)。
3. `resumeLoop` 用途约定:服务器 handler 侧业务一律显式传发起 loop;不传视为"业务声明 loop 无关"。
4. P4 与 P2/P3 并行推进(都依赖 P1 头文件),还是严格串行——默认按并行排期。

***

> 计划按上述假设展开;若有出入,以本文件批注修订为准。
