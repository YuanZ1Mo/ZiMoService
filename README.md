# ZiMoService

ZiMo 客户端生态的核心 Windows 服务，基于 libevent 事件循环提供 HTTP、RESTful API、JSON-RPC、用户系统、文件中心、消息广播、远程音频等能力。

## 功能

- **用户系统** — 注册/登录/找回密码/强制改密（`/auth/*`），会话管理（轮换签发、滑动/绝对双上限、每账号上限、LRU 淘汰），账号+IP 阶梯锁定，注册/重置滑动窗口限流，PBKDF2-HMAC-SHA256（600k 迭代）密码散列
- **门户 SPA** — `/portal`（history fallback），模块化前端（主页/文件中心/服务器音频传输/用户管理），模块按用户授权动态展示，未授权模块前端隐藏 + 后端 403 兜底
- **用户管理** — 用户列表/详情/停用/删除/恢复/重置密码（临时密码 + 强制改密）/改昵称/改角色/授权模块，操作等级约束 + 审计日志
- **文件中心** — 公共/个人双空间，数据库驱动（路径全部由 DB id 组装，防穿越），上传/下载（Range 断点续传）/重命名/移动/复制/删除/zip 打包下载，分享链接（`/share/<token>` 免登录），后台一致性校验线程
- **远程音频** — WASAPI loopback 采集系统声音 + Opus 编码（48kHz 立体声 64kbps，20ms 帧），RESTful 流式推送，门户页 WebCodecs 实时收听
- **消息广播** — 端口 39640，TCP 一对多推送，基于 ZmBroadcastServer/ZmBroadcastClient
- **请求调度** — 每台 HTTP 服务器独立持有 ZmReqLoopPool，per-request 事件循环线程（ZmReqLoop）承载业务，deadline 超时兜底
- **外呼客户端池** — 全门户通用 HTTP/S 外呼（ZmHttpClientPool 预创建 4/上限 80），带 TLS/gzip/cookie/重试/代理/SSE 的现代客户端
- **异步 DNS** — 基于 libevent `evdns_getaddrinfo`，事件驱动
- **Windows 服务生命周期** — 安装/卸载/调试，会话和电源事件感知

## 架构

```
service_main.cpp                     # 入口：install | uninstall | debug
  └─ ServiceCenter                   # Windows 服务控制器
       ├─ ServicePortal              # 业务层（RESTful 回调入口，ZmReqLoop 线程执行）
       │    ├─ DbInitModule            # 数据库初始化（4 个 SQLite 库统一打开/建表/补列/种子）
       │    ├─ UserModule              # 用户系统（鉴权/会话/锁定/限流/审计，供各模块注入）
       │    ├─ FileHubModule           # 文件中心（双空间/分享/zip，注入 UserModule）
       │    ├─ ServerAudioStreamModule # 远程音频（WASAPI 采集 + Opus 编码 + 订阅分发）
       │    ├─ PortalModule            # 门户 SPA + 用户管理（注入 UserModule）
       │    ├─ DeepSeekModule          # DeepSeek 余额查询（保留，当前未挂路由）
       │    └─ ZmHttpClientPool        # 通用外呼请求池（预创建 4 / 上限 80，注入各业务模块）
       └─ NetDock                    # 网络层编排者
            ├─ HttpServerManager     # 通用 HTTP 服务器 (端口 80，静态文件 + 页面路由)
            │     └─ ZmHttpRouter        # 路由中间件链（Express 风格）
            ├─ HttpRestfulManager   # ★ HTTP RESTful 前端 (端口 39441，路径 /zimo/api)
            │     └─ ZmRESTfulServer → ZmReqLoopPool → ZmReqLoop 业务线程
            ├─ HttpJsonRpcManager    # HTTP JSON-RPC 前端 (端口 39440，路径 /zimo/jrpc，兼容保留)
            │     └─ ZmJsonRpcServer → ZmReqLoopPool → ZmReqLoopJrpc 业务线程
            └─ BroadcastManager      # 消息广播服务端 (端口 39640)
```

**RESTful 请求路由（★ 业务入口）**：
```
HTTP RESTful 请求 → ZmRESTfulServer (Worker 线程)
  → HttpRestfulManager::OnRESTfulCBAsync → ZmReqLoopPool::Acquire(排队上限 = 剩余预算)
  → task->BindLoop(loop) → PostToLoop(REQ_LOOP_SIG_START)
  → ServicePortal::RestfulRequestCB (ZmReqLoop 线程执行，事件驱动)
     ├── 剥掉根 URI 前缀（/zimo/api/xxx → /xxx）
     ├── 模块分发链（命中即返回）：UserModule(/auth/*) → FileHubModule(/portal/filehub/*,/share/*)
     │     → ServerAudioStreamModule(/portal/serverAudioStream/*) → PortalModule(/portal/*) → GET /ping
  → ZmReqLoopRest::Response* (TryReply 门 + task 直通) → HTTP 响应
```

## 线程模型

系统包含 **三条 HTTP 事件循环线程**，每台 HTTP 服务器另有 **独立 HTTP 线程池** 与 **ZmReqLoopPool 请求池**：

| 线程/池 | 所属组件 | 说明 |
|---------|---------|------|
| **HTTP 80 事件循环** | `HttpServerManager` → `ZmEvBaseRunLoop` | 仅处理 HTTP 80 请求接收与响应发送 |
| **HTTP 39440 事件循环** | `HttpJsonRpcManager` → `ZmEvBaseRunLoop` | 仅处理 JRPC HTTP 请求接收与响应发送 |
| **HTTP 39441 事件循环** | `HttpRestfulManager` → `m_evLoopHttpServer` | 仅处理 RESTful HTTP 请求接收与响应发送 |
| **HTTP 线程池** | `ZmHttpServer::m_pool` | 每个 HTTP 服务器独立的线程池，执行请求处理（doer） |
| **ZmReqLoopPool（RESTful）** | `HttpRestfulManager` → `ZmReqLoopPool` | per-request 事件循环线程池（预创建 hardware_concurrency，上限 4×，业务预算 5000ms），执行业务回调 |
| **ZmReqLoopPool（JRPC）** | `HttpJsonRpcManager` → `ZmReqLoopPool` | 同上，工厂产出 `ZmReqLoopJrpc` 承载 per-request 回复函数 |
| **ZmHttpClientPool** | `ServicePortal` | 外呼客户端池（预创建 4 / 上限 80），每实例独立事件循环线程 |
| **每日清理线程** | `UserModule` | 每日 03:00 清理过期会话/锁定/限流/审计日志（保留期 7~90 天） |
| **一致性校验线程** | `FileHubModule` | 启动即校验 + 每日 03:00 窗口，文件系统与 DB 漂移以文件系统为准修复 |

用户系统关键线程纪律：库访问以各自 mutex 串行化；PBKDF2 计算在锁外（先锁内取盐，锁外计算，锁内比较）。

## 网络端口

| 服务 | 端口 | 说明 |
|------|------|------|
| 通用 HTTP | 80 | 着陆页 + 登录/注册/找回密码 + 门户 SPA + 静态文件 + 分享转发 |
| HTTP RESTful | 39441 | ★ 业务 API 入口，路径 `/zimo/api` |
| HTTP JSON-RPC | 39440 | JSON-RPC 2.0 兼容保留（路径 `/zimo/jrpc`，仅 `ping`） |
| 消息广播 | 39640 | TCP 一对多消息推送 |

## 前端页面

浏览器访问 `http://localhost`：

| 路径 | 说明 |
|------|------|
| `/` | 着陆页 — 检测会话后跳转 `/login` 或 `/portal` |
| `/login` | 登录页 |
| `/register` | 注册页（zxcvbn 密码强度提示） |
| `/reset` | 找回密码页（救援码验证） |
| `/force-reset` | 强制重置密码页（管理员重置后首次登录跳转） |
| `/portal` 与 `/portal/*` | 门户 SPA（history fallback）— 主页/文件中心/服务器音频传输/用户管理 |
| `/share/{token}` | 文件分享链接 — 302 转发到 RESTful 端口（免登录可达） |
| `/404` | 自定义 404 页面 |
| `*` | 兜底路由 — 文件不存在则展示 404 页面 |

```
www/                      ← 静态资源（exe 同级，全路径规范化防穿越）
├── html/                 # 页面（index/login/register/reset/force-reset/portal/404）
├── css/                  # style.css（主题）/ filehub.css（文件中心）
├── js/                   # auth.js（公共 REST 客户端）/ portal.js / filehub.js 等
├── resource/favicon.ico
└── doc/                  ← 不可通过 HTTP 访问（getAbout 内部读取）

db/                       ← 数据目录（exe 同级，与 www 分离，同步 www 不清库）
├── user/user.db          ← 用户库（users/sessions/login_locks/modules/user_modules）
├── rate/rate.db          ← 通用限流库（register_rate_limits/reset_rate_limits，后续模块可复用）
├── audit/audit.db        ← 业务日志库（user_manage_logs/filehub_logs）
└── filehub/filehub.db    ← 文件中心元数据库（dirs/files/shares/share_download_logs）
modules/filehub/<space>/  ← 文件本体（0 = 公共空间，uid = 个人空间；不可通过 HTTP 直接访问）
```

前端：Vue 3 (CDN) + 纯 CSS，零构建。会话由 `zm_session` cookie 承载，所有业务数据通过 RESTful API（端口 39441，跨域已配置 CORS + 凭据）获取。

## HTTP 80 路由架构

```
HttpServerManager::SetupRouter()           # 基础路由（静态文件）
  ├── /html/* → ServeStaticFile           # 页面
  ├── /css/*  → ServeStaticFile
  └── /js/*   → ServeStaticFile

ServicePortal::RegisterHttpRoutes()        # 页面入口路由
  ├── GET /              → index.html（会话检测跳转）
  ├── GET /404           → 404.html
  ├── ANY *              → ServeStaticFile  # ★ 兜底路由（文件不存在展示 404）
  ├── UserModule        → /login /register /reset /force-reset
  ├── FileHubModule     → /share/{token}    # ★ 302 → RESTful 端口分享页
  └── PortalModule      → /portal 与 /portal/*（SPA history fallback）
```

## RESTful API 方法

所有业务 API 统一通过 RESTful（端口 39441，根路径 `/zimo/api`）访问。会话由 cookie `zm_session` 携带（同源跨端口 Cookie，服务端 CORS 已配置凭据）。

### 认证（/auth/*，免登录）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/auth/login` | 登录（账号+密码），成功签发新会话并种 cookie；失败触发账号+IP 锁定计数 |
| POST | `/auth/register` | 注册（账号/密码/昵称/救援码），IP 滑动窗口限流（20 次/60s） |
| POST | `/auth/reset` | 找回密码（救援码验证后改密） |
| POST | `/auth/complete-change` | 完成强制改密（管理员重置后，临时密码会话内调用） |
| POST | `/auth/logout` | 登出（吊销当前会话） |
| GET | `/auth/me` | 当前会话用户信息（账号/昵称/最后登录 IP/时间） |
| GET | `/auth/heartbeat` | 会话心跳（续期 + 清理过期会话） |

### 门户（/portal/*）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/portal/info` | 门户初始化：用户信息 + 授权模块列表（[{code,name,url}]，developer 全量） |
| GET | `/portal/userManager` | 用户列表（keyword/role/status 筛选 + 分页，上限 100/页） |
| GET | `/portal/userManager/{id}` | 用户详情（含当前授权模块，供授权弹窗回显） |
| POST | `/portal/userManager/{id}/{action}` | 用户操作，action ∈ `disable`/`enable`/`delete`/`restore`/`reset-password`/`nickname`/`modules`/`role` |

用户管理约束：
- 前置：登录 + 可见模块须含 `userManager`（developer 天然全量；admin 提升时自动授权）
- 等级校验：操作者等级必须高于目标当前等级（developer=3 / admin=2 / user=1）
- `reset-password`：生成 12 位随机临时密码（仅一次返回明文）→ 写 `force_change=1` → **吊销目标全部会话**
- `disable`/`delete`：吊销目标全部会话
- 所有操作写 `user_manage_logs` 审计日志

### 文件中心（/portal/filehub/*）

空间参数 `?space=`：`public`（公共，space=0）/ `personal`（个人，space=当前用户 id）。

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/portal/filehub/list` | 列目录（dir_id 指定目录，排序/面包屑链，区分公共/个人根） |
| GET | `/portal/filehub/search` | 递归模糊搜索（space 范围内） |
| GET | `/portal/filehub/download` | 下载（`space, file_id`；支持 Range 断点续传，零拷贝） |
| GET | `/portal/filehub/shares` | 我的分享列表 |
| POST | `/portal/filehub/upload` | 上传（`space, dir_id, name`；body 为二进制流，`X-File-Size` 声明校验，流式落盘） |
| POST | `/portal/filehub/mkdir` | 新建目录 |
| POST | `/portal/filehub/rename` | 重命名（type + id + new_name） |
| POST | `/portal/filehub/move` | 移动（ids + target_dir_id） |
| POST | `/portal/filehub/copy` | 复制（支持跨空间 personal→public） |
| POST | `/portal/filehub/delete` | 删除（ids，级联删除分享行） |
| POST | `/portal/filehub/zip` | zip 打包下载（`space` + body `{ids}`，目录递归，ZipWriter 流式生成） |
| POST | `/portal/filehub/share` | 创建分享（type + id，公共/个人空间均可） |
| POST | `/portal/filehub/unshare` | 取消分享（share_id） |
| GET | `/share/{token}` | 分享访问（HTTP 80 302 → RESTful；公共分享免鉴权，个人分享需登录） |

写操作纪律：鉴权/冲突检查（DB）→ 文件系统 → DB → 审计日志；DB 失败回滚文件系统。公共空间写保护：仅 uploader 本人或 developer/admin；个人空间仅本人。

### 音频（/portal/serverAudioStream/*）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/portal/serverAudioStream/stream` | 远程音频流（二进制帧：len(4B)+seq(4B)+Opus 20ms 帧；48kHz 立体声 64kbps；鉴权+serverAudioStream 模块权限，无设备返回 503） |
| GET | `/portal/serverAudioStream/status` | 采集状态快照 |

### 系统

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/ping` | 心跳检测，返回 `{"pong": true}` |

### 调用示例

```bash
# 心跳
curl http://localhost:39441/zimo/api/ping

# 登录（保存会话 cookie）
curl -c cookies.txt -X POST http://localhost:39441/zimo/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"account":"admin","password":"secret"}'

# 门户信息（授权模块列表）
curl -b cookies.txt http://localhost:39441/zimo/api/portal/info

# 列出公共空间文件
curl -b cookies.txt "http://localhost:39441/zimo/api/portal/filehub/list?space=public&dir_id=0"

# 上传文件
curl -b cookies.txt -X POST --data-binary @file.zip \
  -H "X-File-Size: $(stat -c%s file.zip)" \
  "http://localhost:39441/zimo/api/portal/filehub/upload?space=public&dir_id=0&name=file.zip"

# 下载文件（Range 断点续传）
curl -b cookies.txt -C - "http://localhost:39441/zimo/api/portal/filehub/download?space=public&file_id=123" -o file.zip

# 用户列表
curl -b cookies.txt "http://localhost:39441/zimo/api/portal/userManager?page=1&pageSize=20"
```

## JRPC 方法（兼容保留）

JRPC 前端（端口 39440，路径 `/zimo/jrpc`）仅保留 `ping`，其余方法已全部迁移/移除：

| 方法 | 说明 |
|------|------|
| `ping` | 心跳检测，返回 pong |

## 数据库

四个 SQLite 库由 `DbInitModule` 统一打开与维护（建表/补列/索引/种子，声明式列清单单一事实源），业务模块构造时注入连接引用：

| 库 | 表 | 说明 |
|----|----|------|
| `db/user/user.db` | `users` / `sessions` / `login_locks` / `modules` / `user_modules` | 用户账号（PBKDF2 盐+哈希、user_id 唯一身份 10000001~99999999）、会话（token 哈希）、账号+IP 锁定、功能模块目录与授权 |
| `db/rate/rate.db` | `register_rate_limits` / `reset_rate_limits` | 注册/找回密码滑动窗口限流（落库持久化） |
| `db/audit/audit.db` | `user_manage_logs` / `filehub_logs` | 用户管理操作日志 + 文件中心操作日志（保留 90 天） |
| `db/filehub/filehub.db` | `dirs` / `files` / `shares` / `share_download_logs` | 文件中心元数据（全库驱动，文件本体在 `modules/filehub/<space>/`） |

## 关键设计

- **用户安全** — 密码 PBKDF2-HMAC-SHA256（600k 迭代 + 随机盐）；会话 token 32B 随机 → hex64 存储 token 哈希；每次登录/注册/重置签发全新 token（会话轮换）；滑动 30 天 + 绝对 90 天双上限，每账号最多 5 个活跃会话（LRU 淘汰）；cookie `zm_session` Max-Age 90 天
- **锁定与限流** — 账号+IP 维度阶梯锁定（每 5 次失败升一档：档 1 = 15 分钟，档 2+ = 60 分钟封顶）；注册/重置 IP 滑动窗口限流（20 次/60s）；计数与锁定全部落库持久化，每日 03:00 清理
- **强制改密** — 管理员重置密码 → 12 位随机临时密码 + `force_change=1` → 吊销目标全部会话；临时密码会话登录后必须 `complete-change`（且不改会话——强制改密不吊销其他会话）
- **审计日志** — 用户管理操作（`user_manage_logs`）与文件中心操作（`filehub_logs`）全量落库，审计写入失败记 ERROR（安全操作审计缺失可追溯）
- **双协议共存** — RESTful (端口 39441) 与 JRPC (端口 39440) 各自独立前端 + 独立 ZmReqLoopPool；JRPC 仅保留 ping，业务全部走 RESTful
- **声明式 schema** — `DbInitModule` 每表一份列清单（单一事实源）：新建表 CREATE IF NOT EXISTS，存量库 PRAGMA 差集逐列 ALTER 补列（新列只需在清单追加一行）；UNIQUE/PRIMARY KEY 类约束仅建表生效，存量缺失记 ERROR 拒静默
- **事件驱动** — 业务回调与 close/超时/外部返回全部以事件（PostToLoop）送达同一 ZmReqLoop 线程，per-request 状态无跨线程竞争
- **回复直通** — 回复 helper（ZmReqLoopRest::Response* / ZmReqLoopJrpc::Response）经 TryReply 原子门 + task 直通写 HTTP 响应
- **统一鉴权前置** — `UserModule::RequireModule`（鉴权 → 角色 → 可见模块须含 moduleCode）替代各模块复制粘贴的鉴权块，未授权模块接口直调返回 403
- **零拷贝传输** — 下载使用 `evbuffer_file_segment`（mmap），上传流式落盘
- **断点续传** — 下载支持 HTTP Range 请求（206 Partial Content）
- **流式输出** — HTTP 服务器支持 `StartStreamReply`/`SendReplyChunk`/`EndStreamReply` 分块响应（音频流、zip 打包流）
- **中文路径** — 全链路 Wide API（`CreateFileW`/`FindFirstFileW`），`ZmString::UTF8_To_Unicode`/`Unicode_To_UTF8` 转换
- **请求池调度** — ZmReqLoopPool 预创建 + 扩容 + 排队（不阻塞 HTTP 事件循环），deadline 超时兜底（默认 5000ms 预算，流式自动取消 deadline）
- **Doer 池化** — ZmHttpdDoer 对象池化，即时释放减少内存占用
- **静态资源防护** — ServeStaticFile 全路径规范化（GetFullPathNameW）并校验在 www 根内，防目录穿越

## 构建

```bash
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64
```

输出到 `$(SolutionDir)$(Configuration)\`，中间文件到 `temp\`。需要 VS 2022（v143）+ Windows SDK + 同级目录 `..\ZiMoPublic\`。

第三方依赖全部为**静态链接**，产物单 exe 免 DLL 部署：

| 库 | 链接方式 | 说明 |
|----|---------|------|
| libevent | 静态库 + `/MT` | `EVENT__LIBRARY_TYPE=STATIC` + `EVENT__MSVC_STATIC_RUNTIME=ON` 自编译 |
| OpenSSL | 静态库 | `libcrypto_static.lib` / `libssl_static.lib`；需定义 `OPENSSL_STATIC` 宏 + 链接 `crypt32.lib` |
| zlib | 静态库 | `zlibstatic.lib`（1.3.1，位于 `ZiMoPublic\zlib\`） |
| libopus | 静态库 + `/MT` | 远程音频编码 |
| SQLite | 源码编译 | `ZiMoPublic\sqllite\sqlite3.c` 编入工程（amalgamation） |
| CRT | 静态 | `/MT`（`MultiThreaded`） |

## 服务管理

```bash
ZiMoService.exe install     # 安装 Windows 服务
ZiMoService.exe uninstall   # 卸载
ZiMoService.exe debug       # 前台调试运行
```

## 依赖（ZiMoPublic）

| 模块 | 说明 |
|------|------|
| `net/` | TCP、HTTP 服务端与客户端（ZmHttpClient / ZmHttpClientPool）、RESTful、DNS、请求调度（zm_net_req_loop*）、路由中间件 |
| `service/` | ZmServiceBase |
| `ssl/` | SSL 上下文管理 |
| `libopus/` | Opus 编码器（静态库 /MT，远程音频编码） |
| `net/broadcast` | 消息广播服务端与客户端 |
| `util/` | 线程、线程池、字符串工具、文件工具、`zm_util_sqlite`（SQLite 封装）、`ZipWriter`（zip 打包下载） |
| `sqllite/` | SQLite amalgamation（sqlite3.c 编译进工程） |
| `json/` | nlohmann/json 封装 |
| `spdlog/` | 日志 |
| `libevent/` | 自编译 libevent（静态库 /MT） |
| `openssl/` | 预编译 OpenSSL（静态库，OPENSSL_STATIC + crypt32.lib） |
| `zlib/` | zlib 1.3.1 静态库（HTTP 客户端 gzip/deflate 自动解压、ZipWriter 压缩） |

## 提交规范

```
feat: 新功能 / fix: 修复 / docs: 文档 / style: 代码风格
refactor: 重构 / perf: 性能优化 / test: 测试
chore: 杂项 / build: 构建 / ci: CI / revert: 回滚
```
