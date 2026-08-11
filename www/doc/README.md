# ZiMo 服务门户 — 前端

## 技术栈

| 层级 | 技术 |
|------|------|
| 框架 | Vue 3 (CDN, Options API) |
| 样式 | 纯 CSS（深色主题，CSS Grid + Flexbox） |
| 路由 | 门户为 history 模式 SPA（服务器 `/portal/*` fallback）；各独立页直接跳转 |
| 通信 | `auth.js` 公共 REST 客户端（`fetch` + 凭据携带），RESTful API（端口 39441，路径 `/zimo/api`） |
| 会话 | `zm_session` cookie（HttpOnly 由服务器种置），`auth.js` 统一会话检查/心跳/失效跳转 |
| 音频播放 | WebCodecs `AudioDecoder` 解码 Opus 帧流（fetch 流式拉取） |
| 密码强度 | zxcvbn.js（注册/重置/强制改密页） |
| 其他 | qrcode.min.js（分享链接二维码）、marked.min.js（富文本渲染）、ink.js（Canvas 粒子背景） |

## 项目结构

```
www/
├── html/
│   ├── index.html         着陆页（会话检查 → 用户面板 + 在线状态 + 心跳）
│   ├── login.html         登录页
│   ├── register.html      注册页（zxcvbn 密码强度 + 救援码）
│   ├── reset.html         找回密码页（救援码验证）
│   ├── force-reset.html   强制重置密码页（管理员重置后首次登录进入）
│   ├── portal.html        门户 SPA（主页/文件中心/服务器音频传输/用户管理）
│   └── 404.html           自定义 404 页面（兜底展示）
├── js/
│   ├── auth.js            ★ 公共 REST 客户端 + 会话管理（所有页面共用）
│   ├── index.js           着陆页逻辑（三态分流：已登录/会话失效/网络异常）
│   ├── login.js / register.js / reset.js / force-reset.js
│   ├── portal.js          门户壳（导航/路由/懒加载 + 主页/文件中心/音频/用户管理模块）
│   ├── filehub.js         文件中心模块（双空间/分享/zip/上传下载）
│   ├── ink.js             着陆页 Canvas 粒子背景（"墨聚成印"）
│   ├── vue.global.prod.js Vue 3 运行时（CDN 缓存副本）
│   ├── zxcvbn.js          密码强度评估
│   ├── marked.min.js / qrcode.min.js
├── css/
│   ├── style.css          深色主题全局样式
│   └── filehub.css        文件中心模块样式
├── resource/favicon.ico
└── doc/
    └── README.md          本文件
```

## 页面说明

- **着陆页（/）** — 会话检查三态分流：
  - 已登录 → 用户面板（昵称/账号/最后登录 IP 与时间/活动时间）+ 在线状态徽标 + 心跳保活（断网/恢复 toast 提示）+ "进入门户"按钮 + 墨聚成印粒子动画
  - 会话失效 → toast 提示后 1 秒跳转 `/login`
  - 网络异常 → 错误视图 + 重试按钮
- **登录页（/login）** — 账号 + 密码，失败提示具体原因（含锁定剩余时间）；成功后跳转 `/portal`（若 `force_change` 则跳 `/force-reset`）
- **注册页（/register）** — 账号/密码（zxcvbn 强度条）/昵称/救援码，密码与救援码二次确认
- **找回密码页（/reset）** — 账号 + 救援码验证后设置新密码
- **强制重置密码页（/force-reset）** — 管理员重置后的临时密码会话进入，设置新密码 + 新救援码
- **门户（/portal）** — SPA，顶部模块导航（来自 `/portal/info` 授权模块列表，前端未授权隐藏）：
  - **主页（home）** — 用户信息 + 功能模块概览
  - **文件中心（filehub）** — 公共/个人双空间切换、面包屑导航、文件列表（文件夹优先 + 排序）、搜索、多选/删除/重命名/移动/复制、上传（进度条）、下载（Range 断点续传）、zip 打包下载、分享（生成链接 + 二维码 + 复制 + 取消，分享面板含分享列表）
  - **服务器音频传输（serverAudioStream）** — 实时收听服务器系统声音：连接状态机（idle/connecting/listening/reconnecting），WebCodecs 解码播放，音量控制
  - **用户管理（userManager）** — 用户列表（关键字/角色/状态筛选 + 分页）、停用/启用/删除/恢复、重置密码（临时密码弹窗）、改昵称/角色/授权模块（授权弹窗，提升 admin 自动授予用户管理）
- **404（/404）** — 未匹配路由的兜底展示页

## 数据获取

★ 所有业务数据统一通过 RESTful API（端口 39441，根路径 `/zimo/api`）获取；跨域 + 凭据由 `auth.js` 统一处理（fetch `credentials: 'include'`，跨端口共享 `zm_session` cookie）。

```javascript
// auth.js 公共客户端（window.ZmAuth）
const A = window.ZmAuth;
await A.api.login(account, password);       // POST /auth/login → 服务器种 cookie
await A.api.portalInfo();                   // GET  /portal/info → {user, modules[]}
const list = await A.api.filehubList('public', 0, 'name', 'asc');
//    GET /portal/filehub/list?space=public&dir_id=0&sort=name&order=asc
```

### 认证 API（/auth/*）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/auth/login` | 登录，签发会话并种 `zm_session` cookie |
| POST | `/auth/register` | 注册（账号/密码/昵称/救援码） |
| POST | `/auth/reset` | 找回密码（救援码验证） |
| POST | `/auth/complete-change` | 完成强制改密 |
| POST | `/auth/logout` | 登出 |
| GET | `/auth/me` | 当前会话用户信息 |
| GET | `/auth/heartbeat` | 会话心跳（续期） |

### 门户 API（/portal/*）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/portal/info` | 门户初始化：用户 + 授权模块列表（`[{code,name,url}]`） |
| GET | `/portal/userManager` | 用户列表（keyword/role/status + 分页） |
| GET | `/portal/userManager/{id}` | 用户详情（含授权模块） |
| POST | `/portal/userManager/{id}/{action}` | action: disable/enable/delete/restore/reset-password/nickname/modules/role |

### 文件中心 API（/portal/filehub/*）

| 方法 | 路径 | Query/body 参数 | 说明 |
|------|------|----------------|------|
| GET | `/list` | `space, dir_id, sort, order` | 列目录（含面包屑链） |
| GET | `/search` | `space, keyword` | 递归模糊搜索 |
| GET | `/download` | `space, file_id`（支持 Range） | 下载文件 |
| GET | `/shares` | — | 我的分享列表 |
| POST | `/upload` | `space, dir_id, name`（body=二进制流，`X-File-Size` 头声明大小供完整性校验） | 上传文件 |
| POST | `/mkdir` | `{parent_id, name}` | 新建目录 |
| POST | `/rename` | `{type, id, new_name}` | 重命名 |
| POST | `/move` | `{ids, target_dir_id}` | 移动 |
| POST | `/copy` | `{ids, target_dir_id, target_space}` | 复制（可跨空间） |
| POST | `/delete` | `{ids}` | 删除 |
| POST | `/zip` | `space` + body `{ids}` | zip 打包下载（流式，前端 120s 超时保护） |
| POST | `/share` | `{type, id}` | 创建分享（返回 token/url） |
| POST | `/unshare` | `{share_id}` | 取消分享 |

分享链接：`http://<host>/share/<token>`（HTTP 80 302 → RESTful 端口；公共分享免登录，个人分享需登录）。

### 音频 API（/portal/serverAudioStream/*）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/stream` | 音频流（二进制帧：len(4B)+seq(4B)+Opus 20ms 帧，48kHz 立体声 64kbps） |
| GET | `/status` | 采集状态快照 |

## 设计理念

- **统一会话管理** — 所有页面共用 `auth.js`（会话检查/心跳/失效跳转/401 统一处理），登录态由服务器 cookie 承载，前端不存 token
- **模块化门户** — 模块清单来自服务器授权（`/portal/info`），后端 403 兜底，前端按模块隔离脚本
- **零构建** — 从 CDN 加载 Vue 3，无需 Node.js/npm 构建工具
- **专业运维面板风格** — 深色背景、柔和阴影、清晰信息层级
- **RESTful 优先** — 语义化 HTTP 方法 + Query 参数，JSON body
- **文件中心体验** — 双空间切换、面包屑、批量操作、zip 打包、分享二维码、Range 断点续传
- **密码安全** — zxcvbn 强度提示，密码/救援码二次确认，服务端 PBKDF2 散列
