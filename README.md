# ZiMoService

ZiMo 客户端生态的核心 Windows 服务：一个进程内同时承载 **前端页面 / JSON-RPC / RESTful** 三个 HTTP 面（drogon 1.9.13 + trantor），在此之上提供用户系统、权限与门户、文件中心、服务器音频、小工具等业务能力。全部依赖静态链接，产物为单 exe。

## 功能

- **用户系统** — 注册/登录/登出/强制改密/心跳（`/auth/*`）；会话签发与续期（滑动 30 天 + 绝对 90 天，每账号最多 5 条，超限淘汰最久未活动的一条）；登录失败阶梯锁定（同 IP 连续 5 次锁 15 分钟、10 次锁 60 分钟，账号维度优先、登录键维度兜底）；注册/登录多维限流（滑动窗口 60 秒 20 次，维度含 IP/账号/登录键/设备）；PBKDF2-HMAC-SHA256（60 万次迭代 + 16 字节随机盐，哈希串自带参数，便于日后演进）
- **权限与门户** — 角色 developer(3) / admin(2) / user(1)；有效权限 = 角色默认 ∪ 单独授予 − 单独拒绝；权限点分「门户模块」（进侧边栏）与「功能权限」（只守接口）两类；门户模块清单按 index 全序下发，前端据此渲染侧边栏与路由；权限结果按会话缓存，Redis 不可用时自动降级为直查库
- **文件中心** — 公共空间 + 每用户个人空间；目录与文件元数据全部落库，物理路径由条目 id 逐级拼出（不信任客户端路径）；浏览/搜索/新建目录/重命名/移动/复制/删除/回收站（保留 30 天）/上传（≤8MB 单请求，大文件 8MB 分片续传，单文件上限 20GB）/下载令牌直链/Range 断点续传/zip 打包下载/分享（提取码、有效期、下载次数上限、仅登录可见）/传输任务中心（查看/取消/重试/清空）
- **文件中心管理** — 概览统计、全量一致性校验（以文件系统为事实源，只补库、只删行，不动物理文件）、缓存区管理与清理、全量传输任务与回收站管理、审计与分享日志查询、空间配额设置
- **服务器音频** — WASAPI 环回采集系统声音 → WebM(Opus) 分片 → WebSocket 推流；首个听众建连才启采，无人 15 秒后停采并释放音频设备；新听众从直播边缘回退 5 片（约 200ms）起播；心跳问询 + 30 秒鉴权复检
- **小工具** — JSON 格式化、Markdown 编辑器等工具页；计算全部在浏览器本地完成，服务端只提供一次进入鉴权（`/devTools/check`）
- **Windows 服务** — 安装/卸载/前台调试运行；会话与电源事件感知；停止时先收业务线程与音频设备、再关网络层、最后等工作池排空

## 架构

```
service_main.cpp                     入口：install | uninstall | debug | 无参（服务模式）
  └─ ServiceCenter                   Windows 服务控制器（SCM 回调驱动生命周期）
       ├─ NetDock                    网络层宿主：全局参数/证书 + 三面监听 + 生命周期
       │    ├─ HttpFrontendManager      前端面 80/443（HTTPS 时 443 完整 + 80 仅重定向）
       │    ├─ HttpJsonRpcManager       JSON-RPC 面 39440（根路径 /zimo/jrpc）
       │    ├─ HttpRestfulManager       RESTful 面 39441（根路径 /zimo/api，业务主入口）
       │    └─ BroadcastManager         广播服务端 39640（未接入）
       └─ ServicePortal              业务层：装配模块 + 注册全部路由与横切 advice
            ├─ 用户系统    DbModule / UserModule / PasswordModule / SessionModule /
            │              PermissionModule / SecurityModule / AuditModule /
            │              AuthGateModule / AuthModule / UserAdminModule / PortalModule
            ├─ 文件中心    FileDbModule / FileStoreModule / FileAuditModule / DirLock /
            │              FileNodeModule / FileTaskModule / FileUploadModule /
            │              FileTokenModule / FilePackModule / FileShareModule /
            │              FileHubModule / FileAdminModule
            ├─ 服务器音频  ServerAudioStreamModule（采集器 + WebM 封装 + WS 分发）
            └─ 小工具      DevToolsModule
```

**生命周期三相**（由 `ServiceCenter::OnStart` / `OnStop` 驱动）：

```
OnStart（Phase1 配置）                  OnStop（Phase3 收尾）
  NetDock::Init()                        ServicePortal::Shutdown()   先停业务线程与音频设备
   ├ 全局 Init（参数/证书/CORS/JSONP）     NetDock::Close()            app().quit() + join
   ├ 三面构造与监听登记                    WorkPool().WaitIdle(10s)    在途任务排空后再析构模块
   └ 三面结构路由与门禁 advice
  ServicePortal::Init()
   ├ 建库建表种子 + 依赖注入
   └ 前端/JRPC/RESTful 路由 + CORS
  NetDock::Open()（Phase2 启动）
```

**RESTful 请求链路（★ 业务入口）**：

```
HTTP 请求（39441 /zimo/api/...）
  → ZmHttpRestfulServer：事件循环线程收包，PreRouting 门禁 advice
       ├ OPTIONS 预检 → CORS advice（白名单或同站跨端口放行，否则 403）
       ├ 免鉴权接口（注册/登录、分享公开面、下载令牌直链）放行
       └ 无 zm_session cookie → 401 AUTH_REQUIRED（不查库）
  → 业务协程 handler（drogon 原生协程，跨线程恢复安全）
       ├ co_await SessionModule::AuthAndTouch   会话校验 + 续期 + 强制改密边界
       ├ HasPermission(uid, 权限点)             接口级权限（表见「RESTful API 方法」）
       └ 业务模块编排（数据 / 存储 / 审计）
  → JsonResponse / FileResponse → 事件循环线程发送
```

门禁 advice 只做同步粗判（cookie 存在性、路径判定），异步查库一律放在 handler 内 —— drogon 的 advice 回调只支持调用线程内同步回调，跨线程调用会卡死处理链。

## 线程模型

| 线程 / 池 | 所属 | 说明 |
| --- | --- | --- |
| drogon 事件循环线程 | `ZmHttpServer` 全局 | 请求接收、响应发送、协程恢复、WebSocket 回调；线程数 = CPU 核数（`threadNum = 0` 自动） |
| 业务工作池 | `ZmHttpServer::WorkPool` | SQLite 读写、密码散列、目录扫描、设备枚举等阻塞任务；初始 8 线程、按需扩张，关停时 `WaitIdle` 排空 |
| 流式发送 / 流式落盘 | 平台 HTTP 层 | 收发状态机归连接所属事件循环，读盘/写盘离核；慢客户端由软件水位（默认 8MB）兜住内存 |
| 音频采集线程 | `ZmAudioCapturer` | 每 20ms 一帧、每 2 帧（40ms）封一片；推送只做「锁内登记 + 锁外入队」，socket 写入由各连接所属事件循环完成 |
| 每日 03:00 清理 | 用户库 / 文件库各一条 | 过期会话、锁、限流与审计日志（按表保留 7/30/90 天）；回收站保留期、分片回收、打包缓存、全量一致性校验 |
| 10 分钟巡检 | `ZmFileAdminModule` | 回收空闲打包缓存、回收没有活会话的上传任务 |
| 30 秒巡检 | `ZmFileHubModule` / 音频模块 | 收回「响应开始前断连」漏掉的下载名额；音频在线连接鉴权复检 |

并发模型要点：SQLite 单写连接（串行）+ 读连接池（公共库 `ZmSqliteDb`，WAL + busy_timeout）；目录级写锁 `ZmDirLock` 按桶散列，只覆盖「冲突裁决 + 写库」窗口，长耗时物理 IO 在锁外做。

## 网络端口

| 面 | 端口 | 根路径 / 说明 |
| --- | --- | --- |
| 前端页面 | 80 / 443 | 静态资源 + SPA 页壳 + 页面门禁；HTTPS 模式下 443 为完整面、80 仅做 80→443 重定向 |
| JSON-RPC | 39440 | `/zimo/jrpc`，JSON-RPC 2.0（HTTP 恒 200，错误见信封 `error.code`） |
| RESTful | 39441 | `/zimo/api`，业务 API 主入口 |
| 广播 | 39640 | 自定义 TCP 一对多广播，当前未接入（`ServicePortal::BroadcastMessage` 恒返回 false） |

证书放 exe 同级 `certs\server.crt` + `certs\server.key`：两个文件都存在时全站启用 HTTPS（前端 443 与 80 重定向、JRPC 与 RESTful 同步升 HTTPS），缺一即整体退回 HTTP。

页面（80/443）与 API（39441）端口不同但同 host，属「同站跨端口」：CORS 预检按同站放行，不经过跨站白名单；跨站来源需在 `ZmHttpServer::SetCorsAllowedOrigins` 显式登记（空表 = 跨站一律拒绝）。

## 前端页面

浏览器访问 `http://localhost`：

| 路径 | 说明 |
| --- | --- |
| `/` | 有会话 → 302 `/portal`；无会话 → 302 `/login` |
| `/login` | 登录页（已登录访问 → 302 `/portal`） |
| `/register` | 注册页 |
| `/reset` | 找回密码页（页面已就位，服务端接口尚未注册） |
| `/force-reset` | 强制改密页（管理员重置后首次登录跳转） |
| `/portal` 与 `/portal/*` | 门户 SPA（history fallback），未登录 → 302 `/login?redirect=<原路径>` |
| `/s/{token}` | 文件分享页（链接即凭证，免会话渲染；仅登录可见的分享由接口细判并引导登录回跳） |
| `/404` | 自定义 404 页 |
| `/assets/` `/css/` `/js/` `/svg/` `/resource/` `/favicon.ico` | 静态资源，交静态文件路由器（优先发送 .gz 孪生） |

门户侧边栏模块与页面路由由服务端下发的权限清单驱动：`home`（用户主页，index 1）→ `filehub`（文件中心，2）→ `serverAudioStream`（服务器音频，3）→ `devTools`（小工具，4），负数区 `systemManager`（系统管理，-1）排在其后。前端按「目录名 = 权限点 kebab-case」自动生成路由（`systemManager` → `/portal/system-manager`），无权限的模块既不渲染也不可达。

前端工程（`frontend/`）：Vue 3 + Vite + Pinia + Vue Router（纯 JS）。

```bash
cd frontend
npm install          # 首次
npm run dev          # 本地开发（API 仍走 39441，需服务端在跑）
npm run release      # 构建 + 部署：dist 镜像到 Release\workspace\frontend 并生成 .gz 孪生
```

## 目录结构

```
ZiMoService/
├── service_main.cpp            入口（install / uninstall / debug / 服务模式）
├── service_center.h/.cpp       服务控制器：生命周期三相 + 会话/电源事件
├── service_portal.h/.cpp       业务层：模块装配、路由与 CORS 注册
├── net_dock.h/.cpp             网络层宿主：全局参数、证书、三面配置
├── http_frontend_manager.*     前端面管理器（80/443）
├── http_jsonrpc_manager.*      JSON-RPC 面管理器（39440）
├── http_restful_manager.*      RESTful 面管理器（39441）
├── broadcast_manager.*         广播服务端管理器（未接入）
├── service_define.h            服务名、端口与根路径常量
├── modules/                    业务模块（用户系统 11 个 + 文件中心 12 个 + util）
│   └── util/                   audio_capturer（WASAPI 采集）、webm_muxer、dir_lock
├── frontend/                   门户前端工程（源码 + public + 部署脚本）
├── certs/                      本地证书（server.crt / server.key / cacert.pem）
├── resource/                   图标等资源
└── docs/                       设计与需求文档
```

## 数据库与存储

数据一律落在 exe 同级目录，与静态资源分离（同步前端不会碰到库文件）。

| 库 | 路径 | 表 |
| --- | --- | --- |
| 用户库 | `db\user\user.db` | 13 张：users、user_profile、sessions、password_reset_tokens、login_locks、rate_limits、security_events、roles、permissions、user_permissions、operation_logs、login_logs、verify_codes |
| 文件库 | `db\filehub\filehub.db` | 10 张：spaces、nodes、upload_sessions、upload_chunks、transfer_tasks、shares、share_nodes、share_logs、file_logs、write_limits |

```
db\user\user.db                        用户/会话/权限/审计；建表与种子幂等，仅首次建库
db\filehub\filehub.db                  文件中心元数据；空间行与目录随用随建
modules\filehub\space\<space>\         条目本体（0 = 公共空间，其余 = 用户 id）
modules\filehub\space_cache\<space>\   缓存区：zip\（打包产物）、chunk\<upload_id>\（分片）、tmp\
modules\filehub\space_trash\<space>\   回收站：按条目 id 存放，属用户数据，不随缓存清理删除
uploads\                               请求上传临时目录（框架默认，不属业务数据）
```

可选依赖 Redis（`127.0.0.1:6379`，启动时 TCP 探测）：可达则缓存权限码集合、门户模块清单与策略版本号；不可达时只记日志并降级为直查库 + 进程内计数，功能不受影响。

## RESTful API 方法

全部业务 API 走 RESTful 面（39441，根路径 `/zimo/api`），会话由 cookie `zm_session` 承载（HttpOnly + SameSite=Lax，HTTPS 面加 Secure）。响应约定：成功为 HTTP 200 + 裸 JSON；失败为 `{"code":"...","message":"..."}`，状态码区分大类（401 未登录 / 403 无权限 / 409 冲突 / 413 过大 / 500 内部错误），文件中心失败时另在 `error` 内带回附加字段（如冲突清单）。

### 认证（`/auth/*`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/auth/register` | 注册（账号/密码/昵称），成功即自动登录；IP 维度限流 |
| POST | `/auth/login` | 登录；失败按账号+IP 计数并推进锁定档位 |
| POST | `/auth/logout` | 登出（吊销当前会话） |
| POST | `/auth/force-reset` | 强制改密态下完成改密（仅该态会话可调） |
| POST | `/auth/heartbeat` | 会话心跳（续期 + 应答策略版本号，前端据此决定是否重拉权限） |

强制改密态的会话只允许访问 `/auth/force-reset`、`/auth/logout`、`/auth/heartbeat`，其余接口一律 403 `FORCE_CHANGE_REQUIRED`。

### 用户管理（`/admin/*`，权限点 `userManage`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/admin/users` | 用户列表（关键词/角色/状态筛选 + 分页） |
| GET | `/admin/users/columns` | 列表列元数据（可展示/可编辑/置灰由服务端下发） |
| GET | `/admin/users/perm-codes` | 可授权权限点全集（授权弹窗用） |
| GET | `/admin/users/{id}` | 用户详情（含当前授权） |
| PATCH | `/admin/users/{id}` | 修改用户属性（昵称等） |
| POST | `/admin/users/{id}/role` | 变更角色（等级压制：操作者等级须高于目标，提升最高到操作者下一级） |
| POST | `/admin/users/{id}/permissions` | 授权（按目标集合 diff：授予/拒绝/清除覆盖） |
| POST | `/admin/users/{id}/disable` | 停用（同时吊销该用户全部会话） |
| POST | `/admin/users/{id}/enable` | 启用 |
| DELETE | `/admin/users/{id}` | 删除（软删，可恢复） |
| POST | `/admin/users/{id}/restore` | 恢复已删除用户 |
| POST | `/admin/users/{id}/reset-password` | 重置密码：生成 12 位临时密码（仅本次返回明文）、置强制改密、吊销全部会话 |

每次管理操作都会联动操作审计、安全事件、权限缓存失效与策略版本递增；不可操作自己。

### 门户（`/portal/*`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/portal/modules` | 门户模块清单 `[{code,name,url,index}]`（侧边栏与前端路由驱动） |
| GET | `/portal/home` | 用户主页数据：基本信息 + 角色定位 + 模块清单 + 有效权限码全集 |

### 文件中心（`/filehub/*`，权限点 `filehub`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/filehub/spaces` | 空间列表（公共空间 + 个人空间，含用量与配额） |
| GET | `/filehub/list` | 列目录（排序、面包屑链、关键词过滤） |
| GET | `/filehub/search` | 空间内递归搜索（上限 500 条） |
| GET | `/filehub/node/{id}` | 单条目详情 |
| POST | `/filehub/stat` | 目录容量统计（大目录转后台任务） |
| POST | `/filehub/dirs` | 新建目录 |
| POST | `/filehub/dirs/ensure` | 按相对路径逐级确保目录存在 |
| POST | `/filehub/dirs/ensure_batch` | 批量 ensure |
| PATCH | `/filehub/nodes/{id}` | 重命名（目录/文件） |
| POST | `/filehub/nodes/move` | 移动（批量；冲突策略 ask/skip/rename/overwrite） |
| POST | `/filehub/nodes/copy` | 复制（批量，支持跨空间；超阈值转后台任务） |
| POST | `/filehub/nodes/delete` | 删除到回收站（批量） |
| GET | `/filehub/trash` | 回收站列表（按删除者统计占用） |
| POST | `/filehub/trash/restore` | 还原（同名冲突自动改名） |
| POST | `/filehub/trash/purge` | 彻底删除选中条目 |
| POST | `/filehub/trash/clear` | 清空回收站（转后台任务） |
| POST | `/filehub/upload/simple` | 单请求上传（≤8MB） |
| POST | `/filehub/upload/init` | 分片上传会话（声明大小/哈希，返回已收分片） |
| PUT | `/filehub/upload/chunk` | 上传分片（8MB/片；头 `X-Upload-Id`、`X-Chunk-Index`） |
| POST | `/filehub/upload/complete` | 合并分片并落位（校验大小与哈希） |
| GET | `/filehub/upload/{uploadId}` | 上传会话状态（断点续传查询） |
| DELETE | `/filehub/upload/{uploadId}` | 取消上传并清理分片 |
| POST | `/filehub/download/token` | 申请下载令牌（10 分钟有效；同一令牌并发上限 5，全局直链并发上限 100） |
| GET | `/filehub/dl/{token}/{filename}` | 令牌直链下载（免会话，支持 Range 断点续传） |
| POST | `/filehub/pack` | 打包为 zip（上限 5000 条 / 20GB，转后台任务） |
| POST | `/filehub/pack/{taskNo}/clean` | 清理指定打包产物 |
| GET | `/filehub/tasks` | 历史传输任务（分页/关键词） |
| GET | `/filehub/tasks/active` | 进行中任务 |
| GET | `/filehub/tasks/{taskNo}` | 任务详情与进度 |
| POST | `/filehub/tasks/{taskNo}/cancel` | 取消任务 |
| POST | `/filehub/tasks/{taskNo}/retry` | 重试任务（复制/打包/回收站清理/目录统计） |
| DELETE | `/filehub/tasks/{taskNo}` | 删除任务记录 |
| POST | `/filehub/tasks/clear` | 清理已结束任务 |
| POST | `/filehub/shares` | 创建分享（提取码、有效期、下载次数上限、仅登录可见、多选条目） |
| GET | `/filehub/shares` | 我的分享列表 |
| PATCH | `/filehub/shares/{id}` | 修改分享（有效期/提取码/上限等） |
| DELETE | `/filehub/shares/{id}` | 取消分享 |
| POST | `/filehub/shares/{id}/resume` | 恢复已取消的分享 |
| POST | `/filehub/shares/purge` | 彻底删除分享记录 |
| GET | `/filehub/shares/{id}/logs` | 分享访问日志 |
| GET | `/filehub/share/{token}` | 分享公开面：取分享信息（免会话） |
| POST | `/filehub/share/{token}/verify` | 校验提取码（连错 5 次冷却 10 分钟） |
| GET | `/filehub/share/{token}/list` | 分享目录浏览（凭证由 cookie 携带，有效期 2 小时） |
| POST | `/filehub/share/{token}/download` | 分享下载（单文件或打包） |

写入纪律：鉴权与冲突检查（库内）→ 物理文件操作 → 库内提交，库内失败即回滚物理侧。公共空间仅上传者本人或管理员可改，个人空间仅本人；越权尝试同样记一条失败审计。

### 文件中心管理（`/filehub/admin/*`，权限点 `filehubAdmin`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/filehub/admin/stats` | 概览统计（空间用量、条目数、今日上传/下载、任务与分享状态） |
| POST | `/filehub/admin/sync` | 启动全量一致性校验（可选 dryRun；同一时刻仅一轮，重复触发 409 `SYNC_RUNNING`） |
| GET | `/filehub/admin/sync` | 同步进度快照 |
| POST | `/filehub/admin/sync/cancel` | 取消同步（已完成部分保留） |
| GET | `/filehub/admin/cache` | 缓存区占用 |
| POST | `/filehub/admin/cache/clean` | 清理缓存区 |
| GET | `/filehub/admin/trash` | 全空间回收站 |
| POST | `/filehub/admin/trash/restore` | 还原条目 |
| POST | `/filehub/admin/trash/purge` | 彻底删除条目 |
| POST | `/filehub/admin/trash/clean` | 按保留期清理 |
| POST | `/filehub/admin/trash/clear` | 清空全部回收站（转后台任务） |
| GET | `/filehub/admin/tasks` | 全量传输任务 |
| POST | `/filehub/admin/tasks/{taskNo}/cancel` | 取消指定任务 |
| GET | `/filehub/admin/logs` | 文件中心操作审计 |
| GET | `/filehub/admin/share_logs` | 分享访问日志 |
| GET | `/filehub/admin/spaces` | 全部空间与配额 |
| PATCH | `/filehub/admin/spaces/{space}` | 设置空间配额（0 = 不限） |

### 服务器音频（`/serverAudioStream/*`，权限点 `serverAudioStream`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| WS | `/serverAudioStream/ws` | 推流端点：客户端发 `sync`/`pause`/`pong`；服务端下发文本帧（meta/init/ping/error）与二进制分片帧 |
| GET | `/serverAudioStream/status` | 采集状态与在线听众快照（明细按角色等级下发） |

### 小工具与系统

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/devTools/check` | 小工具页进入鉴权（会话 + 账号状态 + 权限点 devTools），不携带任何工具数据 |
| GET | `/ping` | 心跳探针，返回 `{"pong":true}`；平台共享路径，三个 HTTP 面均可达 |

### 调用示例

```bash
# 登录（-c 保存 zm_session cookie）
curl -c cookie.txt -X POST http://127.0.0.1:39441/zimo/api/auth/login \
     -H "Content-Type: application/json" \
     -d "{\"account\":\"admin\",\"password\":\"********\"}"

# 门户模块清单
curl -b cookie.txt http://127.0.0.1:39441/zimo/api/portal/modules

# 文件中心：列公共空间根目录
curl -b cookie.txt "http://127.0.0.1:39441/zimo/api/filehub/list?space=0&dir_id=0"
```

## JRPC 方法

JSON-RPC 面（39440，根路径 `/zimo/jrpc`）保留协议信封与调用方式，当前只注册健康检查：

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `ping` | 无 | `{"pong":true}` |

```bash
curl -X POST http://127.0.0.1:39440/zimo/jrpc \
     -H "Content-Type: application/json" \
     -d "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{},\"id\":1}"
```

## 构建

```bash
msbuild ZiMoService.sln /p:Configuration=Release /p:Platform=x64
```

- 需 VS 2022（v143）+ Windows SDK 10.0.26100.0 + 同级目录 `..\ZiMoPublic\`（公共库与全部第三方静态库）
- 产物：`Release\workspace\ZiMoService.exe`；中间文件到 `Release\temp\`
- 第三方依赖全部静态链接（/MT），免 DLL 部署

运行期目录（exe 同级）：

```
Release\workspace\
├── ZiMoService.exe
├── certs\                 server.crt / server.key / cacert.pem（有 crt + key 才启用 HTTPS）
├── frontend\              前端构建产物（index.html + assets\ + html\ + svg\，含 .gz 孪生）
├── db\user\user.db        用户库
├── db\filehub\filehub.db  文件中心库
├── modules\filehub\       文件本体、缓存区、回收站
└── uploads\               请求上传临时目录
```

## 服务管理

```bash
ZiMoService.exe install     # 安装 Windows 服务（服务名 ZM_Svc）
ZiMoService.exe uninstall   # 卸载
ZiMoService.exe debug       # 前台调试运行
ZiMoService.exe             # 无参 = 以服务身份运行（由 SCM 启动）
```

## 依赖（ZiMoPublic）

服务依赖同级公共库 `..\ZiMoPublic\`：HTTP 三面底座（`zm_net_http_server` 及前端/JRPC/RESTful 派生面）、出站 HTTP 客户端、TCP 广播、日志、JSON、文件与字符串工具、线程与事件循环、SQLite 连接与事务（`ZmSqliteDb`）、Windows 服务基类（`ZmServiceBase`）。

| 库 | 用途 |
| --- | --- |
| drogon / trantor 1.9.13 | HTTP/HTTPS 服务器与客户端、协程、WebSocket、Redis 客户端 |
| libevent 2.2.1 | 独立事件循环（TCP 广播） |
| sqlite3 | 用户库与文件库（drogon 内置静态库） |
| OpenSSL（libssl / libcrypto） | TLS、SHA-256、HMAC、随机数 |
| libopus | 服务器音频编码 |
| minizip-ng（+ bz2 / lzma / zstd） | zip 打包下载 |
| spdlog 1.17.0 / nlohmann json 3.12.0 | 日志 / JSON |

服务侧链接清单见 `ZiMoService.vcxproj` 的 `AdditionalDependencies`，全部静态库来自 `..\ZiMoPublic\`；新增依赖时两处需同步。

## 设计原则

- **分层单向依赖**：入口 → 服务控制器 → 网络层宿主 + 业务门户 → 业务模块 → 数据模块；下层不反向依赖上层，物理侧动作由装配层以钩子注入
- **数据库 SQL 单一出处**：`ZmDbModule` / `ZmFileDbModule` 分别是两个库全部 SQL 的唯一出处，业务模块只调用其方法，不自行拼 SQL
- **门禁与业务分离**：免鉴权路径、路径前缀、接口级权限点集中声明；handler 只做一次会话校验（`AuthAndTouch` + `CheckCtxSync`）与权限判定
- **事件循环不阻塞**：哈希、库读写、磁盘扫描、设备枚举等耗时操作经工作池执行；跨线程回调一律投回连接所属事件循环，状态只被单一线程读写
- **声明与定义分离**：头文件只放声明，实现放在对应的 `.cpp` 文件中
- **成员变量命名**：`m_` 前缀（结构体除外），全局变量 `g_` 前缀
- **注释规范**：按 `@brief @param @return @example` 格式，中文注释，UTF-8 编码，LF 换行
- **代码组织**：public → protected → private，函数与成员变量分开；cpp 内顺序与头文件一致
- **资源随生命周期**：停止时先停业务线程与音频设备、再关网络层、最后等工作池排空，避免在途任务访问已析构的模块

## 提交规范

提交信息格式：`<类型>: <描述>`

| 类型 | 用途 | 示例 |
| --- | --- | --- |
| `feat` | 新功能 | `feat: 增加用户注册功能` |
| `bugfix` | 修复 bug | `bugfix: 修复登录页面崩溃的问题` |
| `docs` | 文档变更 | `docs: 更新README文件` |
| `style` | 代码风格变动（不影响逻辑） | `style: 删除多余的空行` |
| `refactor` | 代码重构 | `refactor: 重构用户验证逻辑` |
| `perf` | 性能优化 | `perf: 优化图片加载速度` |
| `test` | 添加或修改测试 | `test: 增加用户模块的单元测试` |
| `chore` | 杂项（构建或辅助工具） | `chore: 更新依赖库` |
| `build` | 构建系统或外部依赖变更 | `build: 升级webpack到版本5` |
| `ci` | 持续集成配置变更 | `ci: 修改GitHub Actions配置文件` |
| `revert` | 回滚 | `revert: 回滚feat: 增加用户注册功能` |

## 头文件包含规范

```
优先级由上到下
1. 尽量使用前向声明
2. 对应的头文件（foo.cpp → foo.h）
3. 本项目其他头文件（../modules/module_db.h）
4. 公共库头文件（util_logger.h）
5. 标准库头文件（<iostream>）
```
