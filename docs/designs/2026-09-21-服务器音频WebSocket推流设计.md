# 服务器音频 WebSocket 推流设计

> 状态:设计定稿(待实施) · 日期:2026-09-21
> 修订:v1.7 —— 分片 100ms → 40ms(封装每片 5 帧 → 2 帧;一簇一片的粒度直接决定"读数地板",见 §5.3),起播补发与缓冲窗口随之调成 5 片 / 25 片(仍是 0.2s / 1 秒);新增"余量卡住才回收"(原先余量被推高后不会自己落回,表现为延迟稳定在几百毫秒下不来);看门狗自愈落点由硬编码 0.4s 改为落在目标余量上;前端加 ?debug=1 诊断读数
> 修订:v1.6 —— 落后量口径修正:改由客户端在上报里带**播放头片号**,服务端在**收到上报那一刻**算好并存入连接状态 —— 原先在 `/status` 里拿"此刻的直播边缘"去减一个心跳间隔(15 秒)前的快照,量到的是上报间隔:界面显示"落后 15 秒"而耳朵只落后 0.3 秒;同时"从未上报"返回 -1 显示为"未上报",不再显示成"整条流的时长"。
> 修订:v1.5 —— 按实施与自测结果回写:去掉"积压过深整体重建"(规则④)—— 冻结返回/追不上的表现就是"缓冲里未播的时长变大",由规则② 一次前跳收回即可,不需要额外规则,也不需要拿"片的绝对时刻"去比播放头(那会在起播时误判);补"先发帧再断开"的顺序要求(两件事必须排进同一个循环任务,否则客户端只看得到断开、看不到原因);补 `EnsureReady` 的并发启动语义与 `AUDIO_BUSY`(暂时不可用)与 `AUDIO_NO_DEVICE`(确实没有设备)的区分;界面补一个明确的暂停/继续入口;前端 API 基址支持构建期覆盖(测试环境换端口)。
> 修订:v1.4 —— **恢复一律落在直播边缘**:断网恢复也不再"从原片号续拉"(那条分支整体去掉),`sync` 因此不再需要片号、`resync` 帧整体删除;起播缓冲窗口由 60 秒缩到 1 秒(服务端不再保留可供回头的历史);重连退避上限由 8 秒收紧到 2 秒并监听 `online` 事件(这是"断网恢复 ≤5s"的前提;旧上限会让"网络回来了客户端还在睡"吃掉整个预算)。
> 修订:v1.3 —— 暂停语义定案:**延迟优先**,暂停恢复一律回到直播边缘、不听暂停期间的内容;"无缝续听"整体去掉,协议随之去掉 resume 帧(恢复即 `sync{seq:0}`,走与重连同一套重定基);界面文案、验收指标与时间口径说明同步。
> 修订:v1.2 —— 补稳态定时器(删除轮询循环后,看门狗/前跳/状态收敛/裁剪的驱动者);暂停与恢复的语义与界面文案对齐(>2s 的暂停恢复后回直播边缘),并给暂停加 60s 保持上限;重连补发加 30 片上界,落后超过即直接回边缘;新增"积压过深不追加、直接重建"的返回路径;落后判定改用 buffered 实测值;"重定基"收敛为服务端三处共用的一个过程;同键连接替换与 onClose 归属校验;引导失败即断开。
> 修订:v1.1 —— 缓冲模型改为"全量追加 + 落后前跳 + 周期裁剪";补发深度与延迟目标对齐;binary 帧头补起始片号;推送路径改为"锁内只登记入队、发送在各连接自己的事件循环";心跳兼作鉴权复检并放宽到 120s;明确内存上界与后台边界;验收指标按可测口径重写。
> 对象:实施者、前后端联调、后续维护者
> 决策:HTTP 拉流(/init、/segment 与听众续约)整体弃用,WebSocket 推流为唯一传输核心;服务端保留 /ws(推流)与 /status(状态观察,非媒体面)。
> 前置文档:《2026-09-20-服务器音频模块设计》《2026-09-21-服务器音频播放卡死(复盘)》

***

## 1. 概述

### 1.1 背景(为什么换)

移动端实测确认,拉取式传输存在四类结构性问题,加固手段只能缓解、不能根除:

| # | 问题 | 本质 |
| --- | --- | --- |
| 1 | 静默假死:显示"正在播放"却长期无声 | 数据只有被请求才来,轮询循环的每次挂起都等价于断音;而自愈判定与循环同宿一体,故障时一起停摆 |
| 2 | "断连"风暴:一次抖动 → 续约过期 → 停采 → 重握手 | 隐式心跳(拉片即续约)不可靠:服务端没有任何"听众还在"的直接信号 |
| 3 | 延迟读数锯齿、常态 470~620ms | 轮询空转的往返耗时 + 批次量化:每次都要等一个往返,拿到的是一批若干片 |
| 4 | 每听众 ~10 次/秒请求、每请求一次鉴权 | 拉取式传输的固有代价 |

WebSocket 推流把这四类从结构上消除:**连接级探活**(断连即时可知)、**生产即推**(无轮询 RTT)、**一次建连长期复用**(鉴权改按周期复检),并保留音频缓冲与自愈决策的既有成果。

### 1.2 设计目标(能力核心)

1. **推播(Push)**:采集每 40ms 产一片,立即向所有在线连接推送,稳态端到端延迟 ≤400ms(局域网手机;口径见 §6.2);
2. **连接驱动生命周期**:听众存活 = 连接存活;onClose 即离场,无续约窗口、无扫描;
3. **断线续接**:重连一律从直播边缘续接,不补断网期间的内容(与暂停恢复走同一条路径);
4. **暂停保留**:暂停 = 连接与设备保留、服务端停推;恢复一律回到直播边缘(不听暂停期间的内容)——"无缝续听"整体去掉,延迟优先(§5.2);
5. **按需采集**:首个听众建连才启采,全部离场(或暂停超时)自动停采释放设备;
6. **播放器韧性**:停滞哨兵、自适应余量、追平直播、假死看门狗、周期裁剪——全部与传输解耦,继续生效;
7. **内存有界**:客户端靠周期裁剪封顶、服务端靠 1 秒起播缓冲与心跳超时封顶。

### 1.3 非目标与边界

* 不新增传输加密方案(wss 沿用现有 39441 端口与证书);
* 不改变音频内容与封装(A_OPUS / 40ms 分片);
* 不修改采集器 `audio_capturer` 与封装器 `webm_muxer`;
* **不为后台/锁屏提供"储备缓冲"**。服务端只能推"已经采到的",客户端手里的余量不可能超过"与直播边缘的距离",因此后台存活能力不超过现有水平(见 §7)。

> 推流下客户端唯一能控制的量是**播放头落后直播边缘多少** —— 这决定了 §2.1 的整个缓冲模型。

***

## 2. 总体架构

```
┌──────────────────────────── 服务端 ────────────────────────────┐
│  capture 线程 OnFrame(每 40ms 一片)                            │
│     │  webm_muxer 成簇                                          │
│     ▼                                                           │
│  m_segments 起播缓冲(1s;只服务新连接的起播补发)                 │
│     │                                                           │
│     └─ 锁内:登记待推片(更新每连接的 pushedUptoSeq)              │
│        锁外:loop->queueInLoop(…)  ── 每条连接在“自己所属的”       │
│                                    事件循环上按序写 socket       │
│  WS 连接注册表(键 uid:client)⇄ 听众表(存活与连接绑定)          │
│  事件循环定时器:每 30s 鉴权复检(drogon::async_run 驱动协程)     │
│  /status(HTTP 观察,非媒体面)                                   │
└────────────────────────────────────────────────────────────────┘
                                  │ wss://host:39441/zimo/api/serverAudioStream/ws
                                  ▼
┌──────────────────────────── 前端 ──────────────────────────────┐
│  接收循环(ws 事件驱动,串行 append):                            │
│    meta → 校验 streamId(变 → 重建媒体源)                       │
│    init 帧 → append                                             │
│    分片批 → 逐片 append(永不丢片;落后由"前跳"收敛)            │
│  稳态定时器(每秒):假死看门狗 / 余量衰减 / 落后前跳 / 状态收敛     │
│                    每 30 秒一次:裁剪旧数据(内存封顶)            │
│  韧性全部复用:停滞哨兵 / 自适应余量 / 延迟 EWMA / 媒体会话        │
└────────────────────────────────────────────────────────────────┘
```

**替换边界**:只替换"字节从哪来"(拉 → 推)。媒体元素、MSE、缓冲管理、播放自愈全部原样复用。

### 2.1 缓冲模型:全量追加 + 落后前跳 + 周期裁剪

推流与拉流最大的差别:**数据按直播边缘推来,客户端不可能"拉多一点"攒出富余**。所以"缓冲档位"不能再实现成"拉到多少片",而要重新定义:

| 规则 | 内容 |
| --- | --- |
| ① 全量追加 | 收到的片一律按序 append,**永不丢弃新片**。丢片会在时间轴上留空洞,而媒体元素不会自己跳过空洞:播放头走到空洞处会**永久停住**,状态却仍显示"正在播放" |
| ② 落后前跳 | 以"落后量 = 直播边缘 − 播放头"为**唯一控制量**。落后超过 `余量 + 1.5s` 时,把播放头前跳到 `边缘 − 余量`,落点必在已缓冲区间内(不产生空洞);跳过的音频不补听,这就是"追平直播" |
| ③ 周期裁剪 | 每 30s 把"播放头 − 10s"之前的旧数据从 SourceBuffer 移除,内存与收听时长解耦 |
| ④ 落后收敛 | 页面被冻结后返回、本机追不上等情形,表现都是**"缓冲里未播的时长变大"**——规则② 一次前跳就把它收回到余量以内,**不需要额外的"积压检测/重建"规则**。判据只用 `buffered` 的实测值,不拿"片的绝对时刻"去比播放头(起播时播放头还没定位,那样比必然误判) |

| ⑤ 余量卡住才回收 | 余量被 ② 之外的事件(起播落点、一次卡顿、投递成串)推高之后**不会自己落回**(推流下"追加速度 = 播放速度")。所以要有一条回收:按 **2 秒窗口内的最小余量**判定(瞬时值会随分片到达锯齿,单看某一次必然误判),持续 `LEAD_SETTLE_MS`(3 秒)高于 `目标 + LEAD_SETTLE_MARGIN`(0.35s)才回收一次 —— 正常锯齿与抖动都不触发 |

**读数地板**:前跳的落点会被浏览器吸附到 WebM 簇的起点(seek 只能落在簇边界),而一簇就是一片 —— 所以"实测余量"的下限 ≈ **目标 + 一个分片**,再加一次分片到达的锯齿 ≈ 目标 + 两个分片。这就是分片时长直接决定读数下限的原因(100ms 分片时实测读数 400ms 上下,改成 40ms 后 293ms)。

规则①②的关键推论:**推流下"余量"与"落后量"是同一个量** —— 想要更多余量,就是主动维持更大的落后量(规则②的前跳落点即余量的设定点)。追加的推进速度 = 播放头的推进速度 = 1 倍速,所以落后量一旦定下来**不会自己变化**;抖动或卡顿造成的落后**不会自动收回**,只能靠规则② 主动跳。**规则② 不是补救手段,而是稳态控制手段** —— 这是与拉流最本质的区别(拉流时"拉到多少"本身就能调节落后量)。

***

## 3. 传输协议(定稿)

### 3.1 端点与连接

```
wss://{host}:39441/zimo/api/serverAudioStream/ws
```

同一 HTTPS 监听器(同端口同证书),由 `ZmHttpServer::RegisterWebSocket` 注册。客户端 cookie 随升级请求携带;`Origin` 与页面同 host、仅端口不同(WebSocket 升级无预检,不受跨域限制)。

### 3.2 鉴权(升级快判 + 引导完整判 + 周期复检)

1. **升级快判(同步,onAuth)**:只判 `zm_session` 是否存在且形态合法,不查库(该回调同步,无法 await)。不通过 → 拒绝,业务侧不会再收到 onOpen/onMessage;
2. **引导完整鉴权(异步,BootstrapWs)**:会话(`AuthAndTouch`)+ 账号状态(`CheckCtxSync`)+ 权限(`HasPermission`)。不通过 → 发 `{"type":"auth_failed","code","message"}` 后 `shutdown(kViolation)`;
3. **周期复检(见 §4.7)**:每 30 秒对在线连接重跑一次同样的完整鉴权,不通过即断开。

> 第 3 条是必须的:推流下客户端不再周期性发请求,若只在建连时鉴权,权限被收回、账号被停用、异地登出之后,客户端能一直听到连接自然断开。

### 3.3 帧格式

**服务端 → 客户端**

| 帧 | 类型 | 载荷 |
| --- | --- | --- |
| 元数据 | text | `{"type":"meta","streamId":"…","segmentMs":100,"sampleRate":48000,"channels":2,"bitrate":64000,"baseSeq":4410}` |
| 初始化段 | binary(类型 0) | WebM 初始化段字节(162 字节) |
| 分片批 | binary(类型 1) | `[片数 u32 LE][长度 u32 LE][片]×N`(N 可 0) |
| 心跳 | text | `{"type":"ping"}`(每 15s) |
| 控制 | text | `{"type":"error","code":"AUDIO_NO_DEVICE","message":"…"}` / `{"type":"auth_failed","code","message"}` |

**binary 帧统一头**:`[类型 u8][起始片号 u64 LE]` + 上表载荷。

* 类型 0(初始化段):起始片号 = 紧随其后的第一片的片号(即 meta 里的 `baseSeq`);
* 类型 1(分片批):起始片号 = 本批首片的片号。

去掉"靠片数自增推断片号"的隐含约定 —— 客户端要据此把片落到时间轴上(补发/跳号时尤其必要),**片号必须是显式的**。

片序与时间轴的对应沿用旧口径:**第 N 片覆盖采集时间 `[(N−1)×segmentMs, N×segmentMs)`**,因此"直播边缘时刻 = 已收到的最新片号 × segmentMs"。**换流一律由新连接引导时的 `meta` 承担**(streamId 不同即重建媒体源)。

**客户端 → 服务端**

| 帧 | 载荷 | 触发 |
| --- | --- | --- |
| 同步/重定基 | `{"type":"sync","client":"…"}` | ws open 后;重连、暂停恢复、重建后一律重定基到直播边缘 |
| 暂停 | `{"type":"pause"}` | 用户暂停(连接保留、服务端停推、仍应答 ping) |
| 心跳应答 | `{"type":"pong","seq":N,"pos":M}` | 收到 ping。N = 已收到的最新片号;M = **播放头所在片号**(未起播为 0) |

* **落后量必须在上报的那一刻算好**(服务端用"收到 pong 时的直播边缘 − M"存下来)。心跳每 15 秒才一次,而直播边缘每秒都在前进:若在 `/status` 里拿"此刻边缘"去减一个十几秒前的快照,量到的是**上报间隔**——界面会显示"落后 15 秒",耳朵实际只落后 0.3 秒;
* M 报的是**播放头**(听到的位置),不是"收到的最新片":后者是网络差(约 0.2 秒),前者才是"听到的比实况晚多久"。客户端未起播时报 0,服务端返回 `lagMs = -1`,界面显示"未上报",不显示成一个假的大数;
* `client` 沿用旧口径:1~32 位 `[A-Za-z0-9_-]`,服务端按 `uid:client` 建键;
* **sync 不带片号**:客户端任何时候都只要求"从当前直播边缘起"(首次建连、断网重连、暂停恢复、重建全走这一条),服务端也不做历史补发 —— 两端都不存在"回头要旧片"的路径;
* **停止 = 关闭连接**(不再有 stop 帧):连接关闭即听众离场。

### 3.4 起播补发

任何一次重定基(首次建连、断网重连、暂停恢复、重建)都只做一件事:**从直播边缘回退 `kBackfillSegs`(5 片 ≈ 200ms)起发**。即 `meta{baseSeq}` 之后紧跟 init 段与这一个补发批(≤ `kBackfillSegs` 片),随后转实时推送。

> **为什么只补 200ms、不补历史**:① 补发按片花时间(每片一次 append,手机上约 5~20ms),断网 60 秒后要补一千多片、6~12 秒才追得上,而这些内容恢复后又会立刻被规则② 跳到直播边缘丢弃 —— 越补越慢、也没有任何听觉收益;② 要的是"点击播放/恢复即近乎实时",不是"补听错过的那段"。所以服务端不保留可供回头的历史(§4.9 的缓冲窗口只有 1 秒),客户端也不会去要。
>
> 这 200ms 是必需的:媒体元素手里没有几百毫秒数据就不会进入播放态(数据太少时它停在 HAVE_METADATA:界面显示"正在播放"却不出声)。它不构成"补听",之后播放头一直贴在边缘上。

### 3.5 时序示例

```
客户端                                  服务端
  │  open + sync{client}                 │
  │─────────────────────────────────────▶│ onAuth 快判 → 接受
  │                                      │ 引导(async_run):完整鉴权 + EnsureReady
  │◀──────────────────── meta{baseSeq}   │ 锁内取流描述/init/补发起点
  │◀──────────────────── binary 0(init) │
  │◀──────────────────── 批(5 片起播补发)│
  │◀──────────────────── 批(1 片,实时)… │ OnFrame 每片推(锁外经各连接的事件循环)
  │───────────────────────── pong{seq}   │ 心跳应答(兼作复检挂点与落后量来源)
  │        …断网 / 恢复…                  │
  │  open + sync{client}                 │ 一律重定基到直播边缘(客户端重建媒体源)
  │◀──────────────────── meta + 批(5 片) │
  │        …用户暂停 / 恢复…               │
  │───────────────────────── pause        │ 停止推送(连接保留、仍回 pong,最长 60s)
  │  (重建媒体源) + sync{client}           │ 恢复:一律回到直播边缘
  │─────────────────────────────────────▶│
```

***

## 4. 服务端设计(module_server_audio_stream.h/.cpp)

### 4.1 路由与钩子

```cpp
WsCallbacks cb;
cb.onAuth    = [this](const HttpRequestPtr& req) { return HasSessionCookie(req); };
cb.onOpen    = [this](const WebSocketConnectionPtr& c, const HttpRequestPtr& req) { BootstrapWs(c, req); };
cb.onMessage = [this](const WebSocketConnectionPtr& c, string&& m, const WebSocketMessageType& t) { OnWsMessage(c, std::move(m), t); };
cb.onClose   = [this](const WebSocketConnectionPtr& c) { OnWsClose(c); };
m_rest->RegisterWebSocket(kRoutePrefix + "/ws", cb);
```

* `onOpen` 运行在**连接所属的事件循环**上:在此用 `trantor::EventLoop::getEventLoopOfCurrentThread()` 记下该连接的循环(§4.4 的推送要用);
* `BootstrapWs` 内不做阻塞动作:整条引导(完整鉴权 → `EnsureReady` → 取流描述)经 `ZmHttpServer::RunOnPool` 离核,协程由 `drogon::async_run` 驱动(onOpen 是普通回调,不能 co_await;`async_run` 正是"从普通函数启动协程"的桥);
* 引导完成前到达的消息(通常就是 sync)缓存在连接状态里(单元素 pending),引导完成后统一交给 §4.5 的重定基过程;
* 引导失败(鉴权不过 / `EnsureReady` 失败 / 采集未就绪)→ 发对应 `error` 或 `auth_failed` 帧后 `shutdown(kEndpointGone)`,**不留悬挂连接**(采集没跑就没有 OnTick,悬挂连接既不会被心跳清理,也不会被停采判定覆盖);
* `EnsureReady` 必须区分**三种在跑状态**:①已就绪(直接返回);②**正在启动中**(另一个连接刚把设备拉起来、首片还没到 —— 要**等它就绪**,不能当成"设备正在释放",否则多端同时首次收听时后来者会被踢掉);③正在收尾(等它退完再重启,这才是"刚听完又点开始"的时序);
* 失败原因要分两类下发:**`AUDIO_BUSY` = 暂时不可用**(设备正在释放 / 启动超时 / 采集未就绪)→ 客户端退避重试;**`AUDIO_NO_DEVICE` = 服务端确实没有可用设备** → 客户端如实提示并停下。混成一个码会让"稍等就能听"被报成"没有设备"。

### 4.2 连接注册表与上下文

```cpp
struct WsClient
{
    drogon::WebSocketConnectionPtr conn;
    trantor::EventLoop*            loop = nullptr;  ///< 连接所属事件循环(发送只能排到这里)
    int64_t uid = 0;  std::string account, nickname, ip, cookie;  int level = 0;
    std::string client;                             ///< 客户端标识(区分同账号多端)
    bool     paused = false;                        ///< 暂停:保留连接但不推送
    int64_t  pausedAtMs = 0;                        ///< 暂停起点(超 kPauseHoldSec 断开)
    uint64_t pushedUptoSeq = 0;                     ///< 已排入发送队列的最新片号(避免重复推)
    uint64_t lastSeq = 0;                           ///< pong 报告的最新片号(算落后量)
    int64_t  lastPongMs = 0;                        ///< 心跳兜底判定
    int64_t  sinceMs = 0;
};
std::unordered_map<std::string, WsClient> m_wsClients;   ///< 键 uid:client,由 m_mutex 保护
```

* 连接的键存在 drogon 的连接上下文里(`conn->setContext<std::string>(key)`),onClose 据此定位条目;
* **同键替换**:登记时若该键已存在,先把旧连接 `shutdown(kEndpointGone,"replaced")` 再覆盖 —— 半开的旧连接(网络掉线但心跳未到期)可能还占着同一个键;
* **onClose 的归属校验**:删除前必须确认 `it->second.conn == 传入的 conn`,否则会把"新连接"的条目删掉(旧连接的 onClose 迟到时必然发生)。

采集侧状态(`m_segments` / `m_startSeq` / `m_liveSeq` / `m_nextSeq` / `m_muxer` / `m_running` / `m_readyCv` / `m_lastActivityMs`)与 `EnsureReady` 全部保留;旧续约表 `m_listeners` 及其辅助全部删除。`cookie` 仅供周期复检复用,**不写日志**。

### 4.3 采集生命周期

| 事件 | 行为 |
| --- | --- |
| 首片就绪 | 采集线程 `OnFrame` 置 `m_ready` 并唤醒 `EnsureReady` 的等待者 |
| `OnFrame`(采集线程) | 成簇入起播缓冲 + 登记待推片(§4.4) |
| 收到 sync | 刷新 `m_lastActivityMs`(有听众在要数据) |
| 全部连接关闭 | 沿用 `OnTick` 判定(`m_wsClients.empty()` 且距 `m_lastActivityMs` ≥ `kIdleGraceMs`)→ `RequestStop()` 停采释放设备 |
| 暂停中的连接 | 仍算"有听众"(不停采),但不参与推送;**超过 `kPauseHoldSec(60s)` 未恢复即断开**(否则一个暂停的连接会把音频设备长期占住) |
| 采集失败/设备失效(`OnCaptureDone`) | 对全部连接发 `error{AUDIO_DEVICE_LOST}` 后 `shutdown`;下一次 sync 重新 `EnsureReady` |
| `Shutdown()` | 对所有连接 `shutdown(kEndpointGone)` 并清空注册表 |

### 4.4 实时推送(线程与顺序)

三条硬约束:

1. **采集线程的回调里不得阻塞**(`audio_capturer.h`:帧回调在采集线程上、回调内不得阻塞)——在音频回调里做网络写会直接把音频抖出去;
2. **不得持 `m_mutex` 发送** —— `m_mutex` 同时被事件循环线程(onMessage/onClose//status)与采集线程使用,锁内发送会把事件循环一起卡住;
3. **同一连接的发送必须串行且有序** —— 实时推送在采集线程、建连补发在工作池线程,两条线程各自 `send` 会乱序到达;乱序片会让 MSE 追加失败或出现空洞。

做法:**登记与入队在锁内完成,真正的 socket 写入由各连接自己所属的事件循环串行执行**。

```cpp
void ZmServerAudioStreamModule::OnFrame(const uint8_t* data, size_t len, uint64_t ptsMs)
{
    std::vector<std::pair<trantor::EventLoop*, WebSocketConnectionPtr>> pushes;   // 锁外入队用
    std::shared_ptr<const std::vector<uint8_t>> frame;                            // 帧体一次组装、多连接共享
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_muxer || !m_running)
            return;
        std::vector<uint8_t> seg = m_muxer->PushFrame(data, len, ptsMs);
        if (seg.empty())
            return;

        const uint64_t seq = m_nextSeq;
        m_segments.emplace_back(seq, std::move(seg));
        while (m_segments.size() > kRingSegments)   // 约 1 秒起播缓冲
            m_segments.pop_front();
        m_startSeq = m_segments.front().first;
        m_liveSeq  = seq;
        ++m_nextSeq;

        frame = std::make_shared<const std::vector<uint8_t>>(BuildBatch(seq, m_segments.back().second));
        for (auto& [key, wc] : m_wsClients)
        {
            if (wc.paused || seq <= wc.pushedUptoSeq)   // 已由补发覆盖的不重复推
                continue;
            wc.pushedUptoSeq = seq;
            pushes.emplace_back(wc.loop, wc.conn);
        }
    }
    // 锁外:queueInLoop 只把任务排进目标循环(非阻塞、FIFO),写 socket 由该循环完成
    for (auto& [loop, conn] : pushes)
        loop->queueInLoop([conn, frame] { conn->send(*frame, drogon::WebSocketMessageType::Binary); });
}
```

* 采集线程每片只做"取锁 → 登记 → 入队",不等待任何 I/O;
* 同一连接的所有帧(补发 + 实时)都排进它自己的循环,天然 FIFO ⇒ 顺序闭合;
* 发送前不检查 `connected()`:连接已关闭时 send 由 drogon 侧丢弃,onClose 会同时把该连接移出注册表。

### 4.5 重定基(引导收尾 / 暂停恢复 / 存续期重建 三处共用)

三处的需求完全一样:**给某个连接选定一个发起点,把该点之后已有的片补发出去,并让实时推送从补发点之后接上**。因此收敛成一个锁内过程 + 锁外入队:

```cpp
/// @brief 为该连接选定发起点并完成补发登记(调用方在锁内;返回需要排入的帧序列)
/// @param out  输出:待排入该连接事件循环的帧(meta / init / 分片批)
/// @return 是否成功(采集未跑则为 false,调用方发 error 帧)
bool ZmServerAudioStreamModule::RebaseLocked(WsClient& wc,
                                             std::vector<std::vector<uint8_t>>& out);
```

* 发起点恒取 `max(m_startSeq, m_liveSeq − kBackfillSegs + 1)`,补发到 `m_liveSeq`(一批,≤ `kBackfillSegs` 片);
* **同一临界区内**写 `wc.pushedUptoSeq = m_liveSeq`、`wc.paused = false`,并把 `meta`(仅在流描述变化或首次时)与 init 段排在补发批之前 —— 这样实时推送只会推补发之后的新片,不重不漏,也不乱序(§4.4 约束 3);
* 入队(`wc.loop->queueInLoop`)就在临界区内完成:它只是往目标循环的任务队列里放一个闭包(非阻塞、无 I/O),这样"补发先于实时"的顺序才被真正锁住。

触发点:
* **引导收尾**:完整鉴权通过 + `EnsureReady` 成功 + 处理 pending 的 sync;
* **暂停恢复 / 存续期重建**(客户端发 `sync`):锁内置 `paused = false` 并 `RebaseLocked`;流未变时不重发 meta,流已变(采集重启过)则 `meta` 带新 streamId,客户端据此重建媒体源。

**"先发帧再断开"必须排进同一个循环任务**:告诉客户端原因的控制帧(auth_failed / error)与 `shutdown()` 要一起 `queueInLoop`。若先排控制帧、再当场 `shutdown()`,关闭帧会**先**发出去,客户端只看得到"连接被关",看不到原因 —— 这正是界面停在"连接中"、日志里却什么线索都没有的那类问题。

### 4.6 心跳与存活

* 心跳由采集线程 `OnTick`(200ms 周期)驱动:每 ~15s 向所有连接发 `{"type":"ping"}`;
* 客户端回 `{"type":"pong","seq":N}`;`now − lastPongMs > kHeartbeatTimeoutMs(120s)` → `shutdown(kEndpointGone,"heartbeat timeout")`,由 onClose 清理注册表;
* **阈值取 120s 而不是 30s**:页面被系统冻结时无法应答(那正是手机切后台的常态),阈值太短会把还活着的听众踢掉;120s 同时给出单连接积压的上界(64kbps × 120s ≈ 1MB);
* **暂停中的连接同样要回 pong**(客户端在暂停态照常应答),否则暂停超过阈值会被踢;暂停本身另有 60s 上限(§4.3),两者独立;
* drogon 侧默认每 30s 还会发**协议级** ping(浏览器网络栈直接回 pong,不经过页面 JS),用于穿越 NAT/代理的空闲超时:不需要额外配置,也不作为业务判据;
* 停止语义:关闭连接即离场(onClose 即时移除,无扫描延迟)。

### 4.7 周期鉴权复检

* 注册方式与既有巡检一致(`drogon::app().registerBeginningAdvice` + `loop->runEvery(kRecheckSec, …)`),回调里用 `drogon::async_run(RecheckAll())` 驱动协程(定时器回调是普通函数,不能 co_await);
* `RecheckAll()`:锁内取快照(每连接的 `cookie` / `ip` / `uid` / `conn`)→ **释放锁** → 逐个 `co_await` 完整鉴权(会话 + 账号状态 + 权限)→ 不通过者发 `{"type":"auth_failed",…}` 后 `shutdown(kViolation)`;
* 鉴权实现与引导共用一份(把 `Authorize` 抽成"按 cookie + ip"的形式);
* 成本:每连接每 30 秒一次(旧方案是 10 次/秒),缓存命中时零查库;
* 副作用:复检同时续期会话滑动窗口 —— 与旧方案"拉片即续期"的语义一致。

### 4.8 /status(唯一保留的 HTTP 面)

字段不变(`capturing` / `streamId` / `listenerCount` / `listeners` / `sampleRate` / `channels` / `bitrate` / `segmentDurationMs` / `bufferFillSec`),数据源由 `m_listeners` 换成 `m_wsClients`。`listeners[].lagMs` = 该听众**上报那一刻**算好的落后量(见 §3.3),`: -1` 表示尚未上报;`paused` 表示该连接处于暂停。明细仍按 `level ≥ 2` 下发。本接口不计入活跃度。

### 4.9 常量表

| 常量 | 值 | 用途 |
| --- | --- | --- |
| `kSegmentMs` | 40 | 片长(取自封装器:每片帧数 × 帧长) |
| `kRingSegments` | 25 | 起播缓冲窗口(≈1 秒):只服务新连接的起播补发,不保留可供回头的历史 |
| `kReadyWaitMs` / `kReleaseWaitMs` | 1000 / 3000 | 首片等待 / 设备释放等待 |
| `kIdleGraceMs` | 15000 | 无人时的停采宽限 |
| `kPingIntervalMs` | 15000 | 心跳问询间隔 |
| `kHeartbeatTimeoutMs` | 120000 | 无应答断开阈值 |
| `kPauseHoldSec` | 60 | 暂停保持上限(超时断开、释放设备) |
| `kRecheckSec` | 30 | 鉴权复检周期 |
| `kBackfillSegs` | 5 | 起播补发片数(≈200ms;媒体元素进入播放态的最低需求) |
| `kDetailLevel` | 2 | /status 明细可见等级 |

### 4.10 代码变更清单

| 处理 | 内容 |
| --- | --- |
| **删除** | `HandleInit` / `HandleSegment` / `kMaxCount` / `kDefaultKeepaliveS` / `kMaxKeepaliveS` / `kPollHintMs` / `TouchListenerLocked` / `SweepListenersLocked` / `Listener.keepaliveS` 及续约参数 / `kExposeHeaders` 与 CORS 头暴露(只服务 /init 与 /segment) |
| **新增** | `BootstrapWs` / `OnWsMessage` / `OnWsClose` / `RebaseLocked` / `OnFrame` 的推送(锁内登记 + 锁外入队)/ `BuildBatch` / `RecheckAll` 与定时器 / `HasSessionCookie` / 暂停超时断开 / 同键连接替换 |
| **保留** | 采集器、封装器、`EnsureReady`、起播缓冲(窗口由 600 片改为 25 片)、`OnTick` 停采判定(判据换 `m_wsClients`)、`OnCaptureDone`、`Authorize`(抽出按 cookie + ip 的形式)、`/status` |
| **外部** | `service_portal` 装配与 `Shutdown()` 不变;ZiMoPublic 零改动;会话/权限模块零改动 |

***

## 5. 前端设计

### 5.1 audio.js:WS 传输层(全量重写)

```js
/// 建连并返回 ws 封装;帧已按类型分发,player 只面向回调
export function openAudioWs({ client, onMeta, onInit, onBatch, onError, onClose })
//  open: new WebSocket(`wss://${host}:39441/zimo/api/serverAudioStream/ws`)
//  text 帧 → JSON.parse:
//    meta → onMeta;ping → 回 pong{seq:已收到的最新片号};error/auth_failed → onError
//  binary 帧 → 先读 [类型 u8][起始片号 u64 LE]:
//    类型 0 → onInit(起始片号, 载荷);类型 1 → onBatch(起始片号, splitBatch(载荷))
//  close/error → onClose(重连决策交给 player,传输层不自动重连)
export function wsSend(obj)        // {type:'sync'|'pause'|'pong', …}
//  保留:splitBatch(批格式不变)、newClientId、goLogin(auth_failed 的 NEED_LOGIN 分支)
//  移除:initStream / pullSegments / pingSegment / segmentUrl / fetchTimeout / readError
```

### 5.2 player.js:事件化重写(缓冲管理全保留)

**驱动方式**:轮询循环删除后,原先挂在循环里的控制逻辑改由**独立的稳态定时器**驱动(否则看门狗、前跳、状态收敛、裁剪都没有人调用 —— 这是最容易漏的一处)。

| 生命周期 | 处理 |
| --- | --- |
| `start()` | 清态 → `_armElement()` → `openAudioWs` → onopen 发 `sync{client}` → 启动稳态定时器 |
| `onMeta` | 记录 `streamId/segmentMs/baseSeq/format`;与上次记录的 `streamId` 不同 → `_releaseSource()` + `_armElement()` + `streamGone` 提示;建 SourceBuffer;游标 `_cursor = _liveSeq = baseSeq − 1` |
| `onInit` | 等 SourceBuffer 就绪后 `_append(init)` |
| `onBatch` | 逐片 `_append`(等待重试,不丢片)→ `_cursor` 前进 → `_liveSeq = max(_liveSeq, 批尾片号)` → `_lastFrameAt = now` → 首批后 `_positionPlayback()`,之后由稳态定时器接管起播与收敛 |
| `onClose` / `onError` | 未暂停:退避(上限 2 秒;并监听 `online` 事件立即重试)→ 重开 ws → **重建媒体源** → `sync{client}`;`error` 帧按码复用现有 `onNotice` 通道(no-device / forbidden / session) |
| 暂停态的 `onClose` | **不自动重连**(服务端 60s 暂停上限到点会主动断开):只置"需重建"标记,等用户点恢复时再连 |
| 稳态定时器 `_tick`(每秒) | `_watchPlayhead()`(假死看门狗 + 余量衰减)、`_catchUp()`(落后前跳)、`_syncLiveState()`;每 30 次额外跑一次 `_trimBehind()` |
| `pause()` | 置 `_paused` → `wsSend({type:'pause'})`(停止追加,**仍照常回 pong**) |
| `resume()` | 置 `_paused=false` → **重建媒体源 + `wsSend({type:'sync'})`**:一律回到直播边缘,不听暂停期间的内容(延迟优先) |
| `stop()` | `close()` → `_releaseElement()`(现有) |
| 传输层回调代际 | **只处理当前 ws 对象的回调**:重连/重建后,旧连接在途的帧一律忽略(旧回调不写任何状态),避免用上一代的片覆盖新游标 |

> 重连退避上限收紧到 2 秒、并监听 `online` 事件立即重试:这是"断网恢复 ≤5s"的前提 —— 旧实现 8 秒的退避上限会让"网络已经回来了、客户端还在睡"吃掉整个预算。

**删除**:`_pump` 轮询循环、`_pullOnce`、`_handshake`、`_needSec`、`keepalive` 概念、`X-Next-Poll-Ms` 节流、`_batches`、`JUMP_AFTER_PAUSE_SEC`(恢复即回到边缘,不再需要落后判定)。

> `_liveSeq` 取"**已收到**的最新片号",不是"已追加"的游标。二者稳态一致,唯有客户端追不上时出现差;延迟读数必须按前者算 —— 否则界面显示的是"余量"(0.3s)而不是"与直播边缘的距离"(可能几十秒),读数会说谎。

### 5.3 稳态控制:全量追加 + 落后前跳 + 周期裁剪

```js
/// 规则① 追加:收到的每一片都进缓冲(缓冲忙则等待重试,不丢片)
async _appendBatch(startSeq, segs) {
  for (let i = 0; i < segs.length; i++) {
    await this._append(segs[i])            // 复用现有 _appendOne
    this._cursor = startSeq + i
  }
  this._lastFrameAt = Date.now()
}

/// 规则② 落后前跳:唯一控制量用 buffered 的实测值(ground truth)
_catchUp() {
  const a = this.audio
  if (this._stopped || this._paused) return
  const n = a.buffered.length
  if (!n) return
  // 末段区间就是“最新数据”:全量追加下,它的末尾即直播边缘在媒体时间轴上的位置
  const start = a.buffered.start(n - 1)
  const end = a.buffered.end(n - 1)
  const target = this._baseTargetSec()
  if (end - a.currentTime <= target + CATCHUP_SLACK_SEC) return
  const want = Math.max(start, end - target)          // 落点落在末段区间内,不留空洞
  if (want <= a.currentTime + 0.05) return
  try { a.currentTime = want } catch { return }
  this._lastCt = -1
  this.onNotice('segmentGone', '')                    // 复用现有提示:跳过一段
}

/// 规则③ 周期裁剪:移除“播放头 − 10s”之前的旧数据,内存与收听时长解耦
async _trimBehind() {
  const a = this.audio, sb = this._sb
  if (!sb || !a.buffered || !a.buffered.length) return
  const cutEnd = a.currentTime - KEEP_BEHIND_SEC
  if (cutEnd <= a.buffered.start(0) + 0.01) return
  try {
    await this._remove({ start: a.buffered.start(0), end: cutEnd })
  } catch {
    /* 与追加交错:下次再裁 */
  }
}

/// 规则④ 落后收敛:不需要单独的"积压检测" —— 冻结返回、本机追不上都表现为
/// "缓冲里未播的时长变大",上面那条 _catchUp 一次前跳就把它收回来了
```

实现要点:

* **控制量只用 `audio.buffered` 实测值**;"序号 × 片长"只用于**显示**与协议字段。别拿"片的绝对时刻"去比播放头:起播前播放头还没定位(是 0),任何跑过十几秒的流都会被误判成"落后几十秒";
* **追加失败即重建**:`_appendOne` 的"缓冲持续繁忙"超过上限、或媒体源异常时,**停止后续追加**并走 `_forceReconnect()`;不允许"跳过这一片继续追加"(那会造出多段空洞);
* 帧解析失败**不能静默**:WS 的解帧包在 try/catch 里,若"每一帧都解析失败"会让整条链路无声瘫掉(界面停在"连接中"),至少要留一次告警日志;
* 内存上界 = `10s(身后) + 落后量 + 已缓冲(边缘)`,与收听时长无关;
* 稳态定时器在后台会被浏览器节流(最小间隔可能到 1 分钟),这只让控制变迟钝,不影响正确性;回前台时 `checkStale()` 会补一次判定。

### 5.4 韧性部件(全部保留,语义微调)

| 部件 | 调整 |
| --- | --- |
| 停滞哨兵 `_tickStall` | 判据由"距上次成功拉片"改"距最近一帧(meta/批/ping)":>10s 无帧且非暂停 → `close()` 触发重连;含"建连后迟迟没有 meta"的判定(`CONNECT_STALL_MS`) |
| `checkStale()`(回前台) | 同样按帧滞留判定 → 主动重连 |
| 自适应缓冲余量 `_leadSec` | 触发源由"拉片 RTT"改"帧到达间隔的抖动":`jitterMs = EWMA(|帧间隔 − segmentMs|,α=0.2)`,`余量 = max(_leadSec, jitterMs×2 + 0.15s, 2 片)`;翻倍/衰减/上限不变(0.3s 起,封顶 2s,45s 无卡顿后减半) |
| 假死看门狗 `_watchPlayhead` | 不变(改由稳态定时器驱动) |
| 追平直播 `_catchUp` | 由"补救"升为**稳态控制**(§5.3 规则②),阈值不变(余量 + 1.5s) |
| 周期裁剪 `_trimBehind` | 新增:每 30s 裁掉"播放头 − 10s"之前的旧数据 |
| 余量回收(卡住才回) | 新增:窗口最小余量持续 3 秒高于 目标+0.35s 才前跳一次;看门狗自愈的落点由硬编码 0.4s 改为落在当前目标上 |
| 延迟 EWMA / MediaSession / 音量 | 不变 |

### 5.5 index.vue

* 诊断读数:URL 带 `?debug=1` 时在参数行下方显示一行 —— `余量 / 帧抖动 / 目标 / 实测余量 / 触发额度 / 落后 / 超出紧急线 / 窗口最小余量 / 稳态定时器拍数 / 已执行前跳次数`。默认不显示;它是排"延迟下不来"这类问题的唯一有效入口(定位到"地板由分片粒度决定"就是靠它)。

* `onVisibility` 回前台追加 `player.checkStale()`;
* 能力检测把"浏览器支持 WebSocket"并入 `isPlaybackSupported`;
* **暂停文案校正**:现有"保留连接 · 60 秒内可无缝续听"与新语义不符(暂停恢复一律回到直播边缘),改为"点击继续,从服务器当前声音接着听"(不承诺续听旧内容);
* **补一个明确的暂停/继续入口**:圆盘只管"开始/停止"(推流下停止 = 关闭连接即离场),暂停另给一个次级按钮 —— 否则暂停只能从锁屏媒体面板触发,而"暂停后再继续"是明确的需求;
* 使用提示里关于后台的那句改为"切到别的模块继续听;锁屏/切后台受系统限制可能中断,回前台自动接回直播边缘"(不再宣称锁屏可继续听)。

### 5.6 后台与锁屏(如实标注)

切后台/锁屏时:页面可能被系统冻结 → 收不到帧、回不了 pong → 超过 §4.6 的阈值后连接由服务端断开 → 回前台走重连(重新 sync,必要时重建媒体源)。冻结时间较短时页面没被断开,积压的帧会按序追加进来(播放头随之前进),再由规则② 一次前跳到直播边缘 —— 听觉上是"接着放、再无跳回现在"。

**推流不改变这条边界**:服务端只能推"已采到的",客户端手里的余量不可能超过"与直播边缘的距离",因此无法为后台预留缓冲(§1.3)。回前台后的表现:数秒内回到直播边缘(可能伴随一次"跳过一段"的提示)。

***

## 6. 交付与验收

### 6.1 交付物与顺序

| 序 | 内容 | 文件 |
| --- | --- | --- |
| 1 | 服务端 WS 端点(§4 全量:登记/入队/重定基/心跳/复检/协议) | `module_server_audio_stream.h/.cpp` |
| 2 | 前端 WS 传输层 + API 基址收敛成可覆盖常量(测试环境换端口用) | `frontend/src/api/audio.js`、`frontend/src/api/base.js`(连带 `client.js` / `filehub.js` / `stores/session.js` 改为引用) |
| 3 | 前端播放器事件化重写(§5.2~5.4) | `frontend/src/modules/server-audio-stream/player.js` |
| 4 | 删除 HTTP 拉流:服务端 `/init` `/segment` 与续约扫描;前端轮询/拉片/keepalive 代码 | 同 1~3 |
| 5 | 界面:暂停/继续入口 + 文案校正(§5.5) | `frontend/src/modules/server-audio-stream/index.vue`、`audio-stream.css` |
| 6 | 构建部署(前端 `npm run release` + 服务重编译重启) | — |
| 7 | 真机验收(§6.2) | — |
| 8 | 更新交接文档 | `docs/designs/2026-09-21-服务器音频后端实现交接.md`、`2026-09-20-服务器音频前端交接.md` |

> 不设"HTTP 并存"灰度:唯一客户端是本仓库前端,整体切换 + 真机验收即可;旧代码一并删除以保持仓库整洁。

### 6.2 验收标准(手机 + PC)

**延迟口径写死**:`延迟读数 = 已收到的最新片号 × segmentMs − 已播位置`。与旧读数相差固定的一个片长(旧口径按片的**起点**算),复测须按新口径换算后再比较。

| 场景 | 通过标准 |
| --- | --- |
| 稳态延迟 | 局域网手机端到端 ≤400ms(按上述口径),读数平滑无 ±100ms 锯齿 |
| 断网 10s(飞行模式)再恢复 | ≤5s 恢复"正在播放",全程无"谎报正在播放";恢复后落后 ≤0.5s(一律从直播边缘续接,断网期间的内容不补) |
| 杀服务 / 停采 | ≤10s 自动重连回到直播边缘,无手动操作 |
| 切后台 5 分钟回前台 | 回到直播边缘(允许一次"跳过一段"提示),不要求"无缝" |
| 暂停后立即恢复 | 从直播边缘继续(不听暂停期间的内容),≤3s 出声 |
| 暂停 30s 后恢复 | 同样从直播边缘继续,≤3s 出声(60s 内免重新协商音频设备) |
| 暂停后离开 60s | 服务端断开并释放音频设备(设备不被长期占住);此后恢复 = 走一次重连,≤5s 出声 |
| 多端并行 | 手机 + PC 互不影响;`/status listenerCount` 正确 |
| 断连风控 | 主动关闭/停止:服务端听众表即时清空;网络异常(无关闭帧):≤120s 由心跳超时清空 |
| 权限撤销 / 停用 | ≤30s 内在线连接被断开并提示 |
| 页面刷新 / 换流 | 新 streamId 触发重建,无旧缓冲残留 |
| 内存上界 | 连续收听 1 小时,页面内存与 SourceBuffer 不单调增长(周期裁剪生效) |
| 返回原站降级 | 浏览器不支持 WebSocket 时能力检测提示(与 `isPlaybackSupported` 合并判定) |

> 两项时间指标的口径:从"客观条件就绪"到"真的出声" —— 断网一项从网络恢复计时,暂停一项从点击继续计时,都含重连(如需)、鉴权、补发 5 片与起播定位。实测预期 1~2 秒,数值是验收上界(超了判不通过),不是目标值;与"稳态端到端延迟"(你说的"差多远")是两个不同的量。

***

## 7. 风险与边界

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| OEM 后台限制(杀连接/冻结页面) | 后台断流 | 与传输无关(旧 HTTP 同样存在);回前台 `checkStale` 重连 + 落后前跳回到边缘;不作"储备缓冲"承诺 |
| 冻结返回时的千片积压 | 短暂落后 | 按序追加 + 规则② 一次前跳到边缘(§5.3 规则④):不重建、不静默 |
| 冻结页面期间的发送积压 | 服务端内存 | 心跳阈值 120s 给出上界(64kbps ≈ 1MB/连接),超时即断开 |
| 无业务心跳 → 半开假活 | 连接泄漏 | 服务端 ping + 120s 超时,客户端 10s 收帧哨兵,双向兜底 |
| 首包竞态(onMessage 早于引导完成) | 丢 sync | 单元素 pending 缓冲,引导完成后交重定基过程 |
| 发送跨线程乱序 | 追加失败 / 空洞 | 同一连接的发送全部排进它自己的事件循环(§4.4),补发与 `pushedUptoSeq` 同临界区(§4.5) |
| 同键连接替换 | 误删新连接 / 计数错乱 | 登记时踢旧连接;onClose 按连接上下文取键并校验 `conn` 一致(§4.2) |
| 采集线程被发送拖慢 | 音频抖动 | 锁内只登记入队,不做 socket 写;帧体多连接共享 |
| 客户端追不上(移动弱网) | 落后累积 | 规则② 前跳收敛;真到"播不动"由假死看门狗兜底重建;不做"逐片跳过"(会造多段空洞) |
| 暂停连接长期占设备 | 设备与功耗 | `kPauseHoldSec = 60` 超时断开(§4.3) |
| 多标签 / 多端 | 连接语义 | `uid:client` 键天然区分,`/status` 如实计数 |
| 服务重启瞬间 | 短暂断音 | 重连后直接回直播边缘;起播补发的 5 片保证立刻有料可播 |
| 代理 / 网关环境 | WS 被缓冲 / 中断 | 移动直连为主;drogon 默认协议级 ping 穿越空闲超时;验收外延场景如实记录 |

***

## 8. 备选方案(附录,均维持"已放弃"结论)

| 方案 | 结论 | 理由 |
| --- | --- | --- |
| HTTP 长轮询改良 | 放弃 | 仍是万能轮询:静默半开、续约失效这两个结构性问题不消除 |
| SSE | 放弃 | 单向,续拉/暂停/恢复另需通道,二进制 base64 徒增开销 |
| WebRTC 数据/媒体通道 | 放弃 | **与"手机后台/锁屏可听"的硬约束直接冲突**:WebRTC 与 WebAudio/WebCodecs 两条低延迟通道在移动系统上切后台即被挂起,只有交给媒体元素播放的分片流能在后台继续;此外需自建信令 + ICE/STUN/TURN 与 SRTP/DTLS,只换来 ~100ms |
| HTTP 拉流 | 放弃(保留为灾备知识) | 其缓冲自愈、追平、看门狗等成果在本方案中原样保留;拉取式传输本身弃用 |
