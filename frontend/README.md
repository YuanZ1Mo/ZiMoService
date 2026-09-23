# ZiMo 门户前端

ZiMoService 的门户前端单页应用（SPA），基于 **Vue 3 + Vite 8 + Pinia + Vue Router**（纯 JS，无 TS）。
承载登录/注册/强制改密等通用页、门户壳与五个门户模块（用户主页 / 文件中心 / 服务器音频 / 系统管理 / 小工具），以及免登录的文件分享页。
构建产物为单一 `index.html` + 静态资源，由服务端（drogon）托管；页面路径全部由前端路由处理，服务端统一 SPA 回落 + 页面门禁。

设计基准与交互规范见已评审的预览稿：[user-system](../docs/designs/mockups/user-system/index.html)、[filehub](../docs/designs/mockups/filehub/index.html)、[server-audio-stream](../docs/designs/mockups/server-audio-stream/index.html)（含配色/字体/令牌/组件规范）。

## 端口与拓扑

| 面 | 端口 | 说明 |
|---|---|---|
| 页面 | 80 / 443 | SPA 页面 + 静态资源；未带有效会话访问受保护页面 → 302 `/login` |
| RESTful API | 39441 | `/zimo/api/*`，与页面跨端口同站（cookie `SameSite=Lax` + `credentials: include`） |
| 实时推流 | 39441 | 服务器音频 WebSocket `/zimo/api/serverAudioStream/ws`（http → ws，https → wss） |
| JSON-RPC | 39440 | 平台面，前端不直接使用 |

- API / WS 基址在 [src/api/base.js](src/api/base.js) 中按当前协议与主机名拼出（`<protocol>//<hostname>:39441/zimo/api`，WS 侧把 http 换成 ws）；端口可用构建期变量 `VITE_API_PORT` 覆盖（测试环境副本把 RESTful 面挪到空闲端口时用）。本地开发需服务端在跑。
- 静态资源引用一律**根绝对路径**（`/assets/...`），`base` 恒为 `/`。

## 页面与路由

| 路径 | 组件 | 说明 |
|---|---|---|
| `/login` | `pages/LoginView.vue` | 登录（`meta.public`）；已登录访问 → 回 `/portal` |
| `/register` | `pages/RegisterView.vue` | 注册（含密码强度提示） |
| `/reset` | `pages/ResetView.vue` | 找回密码（页面预留，服务端接口尚未注册） |
| `/force-reset` | `pages/ForceResetView.vue` | 强制改密（`force_change` 会话唯一可达页） |
| `/404` | `pages/NotFoundView.vue` | 未匹配路径统一重定向到此 |
| `/s/:token` | `pages/ShareView.vue` | 免登录分享页：不进门户壳，提取码校验 / 只读浏览 / 下载 |
| `/portal` 及子路由 | `layouts/PortalLayout.vue` + `modules/<kebab>/index.vue` | 门户壳（重定向到 `/portal/home`），模块路由自动生成 |
| `/` | — | 重定向 `/portal` |
| `*` | — | 重定向 `/404` |

## 门户模块

侧边栏与路由都由权限点驱动，**目录名 = 权限点 code 的小写 kebab-case**：

| 目录 | 权限点 code | index | 说明 |
|---|---|---|---|
| `home` | `home` | 1 | 用户主页：欢迎横幅 + 概览统计 + 可点击进入的模块卡片 |
| `filehub` | `filehub` | 2 | 文件中心：双空间浏览、上传下载、打包、分享、回收站、任务面板 |
| `server-audio-stream` | `serverAudioStream` | 3 | 服务器音频：实时收听系统声音 + 状态与听众信息 |
| `dev-tools` | `devTools` | 4 | 小工具：4 个分组的 7 件工具，计算全在本地 |
| `system-manager` | `systemManager` | -1 | 系统管理：子标签「用户管理」(`userManage`) 与「文件中心管理」(`filehubAdmin`)，各自独立授权与显隐 |

小工具清单（深链接 `?tool=<key>`）：格式化与编辑「JSON 格式化、Markdown 编辑器」；时间与编码「时间戳转换、编解码」；生成与计算「生成器、进制与颜色」；文本处理「字符串整理」。

## 目录结构

```
frontend/
├── index.html                 # SPA 唯一入口(引用 /svg/favicon.svg;首屏同步落 data-theme 防闪白)
├── vite.config.js             # base='/' , outDir=dist
├── public/                    # 原样拷贝进 dist 的静态文件
│   ├── svg/favicon.svg        #   站点图标
│   └── html/404.html          #   服务端自定义 404 页
├── scripts/
│   └── deploy.mjs             # 部署脚本:dist 镜像到运行时 frontend + 生成 .gz 孪生
└── src/
    ├── main.js                # 入口:挂载 + 全局样式引入
    ├── App.vue                # 根组件:路由出口 + 全局 Toast(inject('toast'))
    ├── router/index.js        # 路由表 + 全局守卫(会话探测/force_change/回跳)
    ├── api/                   # fetch 封装与按域拆分的接口
    │   ├── base.js            #   API/WS 基址(VITE_API_PORT 可覆盖)
    │   ├── client.js          #   credentials、401/403 统一处理、错误码映射、超时
    │   ├── auth.js            #   /auth/*:注册/登录/登出/强制改密/心跳
    │   ├── portal.js          #   /portal/*:模块清单/主页
    │   ├── admin.js           #   /admin/*:用户管理
    │   ├── filehub.js         #   /filehub/*:浏览/条目/上传引擎/下载/打包/任务/分享/管理端
    │   ├── audio.js           #   /serverAudioStream/*:状态接口 + WS 建连与分片切分
    │   ├── devTools.js        #   /devTools/check:页面进入鉴权
    │   └── drop-entries.js    #   系统拖入的文件/文件夹展开(entry API 递归)
    ├── stores/
    │   ├── session.js         # 会话上下文(三态 isLoggedIn)、force_change
    │   ├── modules.js         # 模块清单(服务端下发)+ policyVersion 轮询应答
    │   └── filehub.js         # 传输任务面板 + 上传队列(轮询契约见下)
    ├── layouts/
    │   └── PortalLayout.vue   # 门户壳:顶栏(心跳胶囊/主题/用户菜单)+抽屉导航+keep-alive 工作区
    ├── pages/                 # 通用页(免会话白名单页,与门禁白名单一一对应)
    │   ├── LoginView.vue / RegisterView.vue / ForceResetView.vue / NotFoundView.vue
    │   ├── ResetView.vue      #   /reset   (预留,服务端接口未注册)
    │   └── ShareView.vue      #   /s/:token 免登录分享页
    ├── modules/               # 门户业务模块(懒加载 + keep-alive)
    │   ├── home/index.vue
    │   ├── filehub/           #   index.vue + FileList/DirTreeNode/FileIcon/MoveCopyDialog/
    │   │                      #   ShareDialog/TaskPanel/TrashView + filehub.css
    │   ├── server-audio-stream/  # index.vue + player.js(MSE 播放器) + audio-stream.css
    │   ├── dev-tools/         #   index.vue + toolbox.js(共用基座) + 各工具组件
    │   └── system-manager/    #   index.vue(用户管理) + FileHubAdmin.vue(文件中心管理)
    ├── components/            # 跨页通用组件
    │   ├── ThemeSwitch.vue    #   亮/暗/跟随系统 三态循环(持久化 localStorage 'zimo-theme')
    │   ├── PasswordStrength.vue
    │   ├── Modal.vue          #   模态(遮罩点击/Esc 关闭)
    │   └── ZmSelect.vue       #   自绘下拉(原生 select 的下拉层不可定制,故自绘)
    └── styles/
        ├── base.css           #   设计令牌(亮暗双主题)+通用组件样式(按钮/表单/徽章/表格/Toast…)
        ├── theme.css          #   主题切换按钮与 color-scheme
        └── portal.css         #   门户壳布局(顶栏/抽屉导航/工作区/分页)
```

## 架构要点

### 模块化机制（code → 路由）

- `src/modules/<kebab>/index.vue` 的**目录名 = 权限点 code 的小写 kebab-case**（`serverAudioStream` → `server-audio-stream`、`systemManager` → `system-manager`）。
- 路由用 `import.meta.glob('../modules/**/index.vue')` 自动生成，挂载在 `/portal` 下（`/portal/filehub`、`/portal/system-manager`…），不需要单独维护路由表。
- 侧边栏渲染由服务端 `GET /portal/modules` 下发的 `{code,name,url,index}` 驱动，排序规则：正数升序在前，负数区（-1 最先）依次在后；无权限的模块不渲染、路由不可达（前端守卫 + 服务端 403 双重拦截）。
- 工作区用 `<keep-alive>` 缓存模块组件，切换不销毁运行状态（分片上传、音频播放都继续），模块内按 `onActivated`/`onDeactivated` 挂摘监听与轮询。
- 模块页可深链：文件中心把当前位置写进查询串（`/portal/filehub?space=0&dir=12`），刷新与浏览器前进/后退都回到原处；小工具支持 `?tool=<key>`。

### 鉴权与会话

- 会话 cookie `zm_session`（HttpOnly），所有 API 请求 `credentials: include`。
- `isLoggedIn` 是三态（`null` = 尚未探测）：整页刷新/首次进入时守卫先 `probe()`（带 cookie 打 `/portal/home`）恢复上下文，再重走守卫。
- 守卫规则（[router/index.js](src/router/index.js)）：未登录访问受保护页 → 记 `redirect` 跳 `/login`，登录后回跳；`force_change=1` 会话只放行 `/force-reset`；已登录访问 `/login` `/register` `/reset` → 回门户。
- API 层（[api/client.js](src/api/client.js)）统一处理：401 → 记回跳跳登录；403 `FORCE_CHANGE_REQUIRED` → 跳强制改密；其余 `{code,message}` 映射到字段级/表单级报错。
- 心跳：门户壳每 10s `POST /auth/heartbeat`；401 立即下线，网络中断只提示不强制下线；应答里的 `policyVersion` 变化时重拉模块清单（改权限后无需手动刷新）。
- 分享页 `/s/:token` 是公开路由：链接本身即凭证，提取码通过后由服务端下发 `zm_share` 凭证 Cookie；`login_only` 的分享引导登录后回跳本页。

### 文件中心要点

- 浏览：空间树 / 列表与网格双视图 / 虚拟滚动（行数 > 300 才启用窗口裁剪）/ 排序 / 面包屑 / 搜索；目录位置写进 URL，可分享可刷新。
- 选择与手势：单击、Ctrl 加选、Shift 连选、Ctrl+A 全选、Esc 取消；行内 `⋯` 菜单与右键菜单同一套动作；行拖拽到文件夹即移动，系统文件（含文件夹）拖入即上传。
- 上传引擎（[api/filehub.js](src/api/filehub.js)）：≤8MB 走单请求 `upload/simple`；更大的走 `init → 分片(PUT，并发 3，可断点续传) → complete`，分片 8MB；≤256MB 的文件会算整文件 SHA-256 交给服务端判秒传并在合并后校验（算不出哈希就跳过，不影响上传）。
- 拖入文件夹由 [api/drop-entries.js](src/api/drop-entries.js) 用 entry API 递归展开（`readEntries` 必须循环读到空，否则静默丢文件），把层级与空目录一起交给上传队列。
- 任务面板（[stores/filehub.js](src/stores/filehub.js)）：只轮询 `GET /filehub/tasks/active`（1.5s 一次），没有进行中任务就停轮询；页面切走停、回前台立即刷一次；打包完成后自动换取直链并触发一次下载。
- 分享：「创建/管理分享」弹窗（有效期、提取码、下载次数上限、仅登录可见）；分享页 `ShareView` 复用 `FileList`，整页只读（打开 / 下载 / 打包下载 / 详情）。
- 慢接口超时口径（写在 `api/filehub.js`）：分片合并 10 分钟、同步复制 5 分钟、批量建目录 2 分钟 —— 默认 15 秒会在服务端已成功时误报"网络异常"，诱发重复提交。

### 服务器音频要点

- 播放链路：WS 收分片 → MediaSource 喂给 `<audio>` 播放 → 后台保活；实现集中在 [modules/server-audio-stream/player.js](src/modules/server-audio-stream/player.js)，不依赖 Vue，便于脱开界面调试。
- 硬约束：声音必须交给媒体元素（MSE），**禁止 WebCodecs 自解码**；**禁止任何 WebAudio 用法**（声音一旦经 WebAudio，页面被系统挂起时会一起中断，后台播放失效）。
- 稳态与自愈：`appendBuffer` 串行且追加前确认 `readyState === 'open'`；收到多少追加多少（永不丢片）；延迟靠播放头前跳控制，内存靠周期性裁剪封顶；卡顿、滞留、播放头停走都有看门狗与重建策略。
- 能力检测：安全上下文 + MSE 支持 `audio/webm; codecs="opus"` + WebSocket，任一不满足给出降级提示。

### 视觉与主题

- 设计令牌集中在 [styles/base.css](src/styles/base.css)（语义变量 `--color-*` / 圆角 / 阴影 / 动效时长），暗色主题经 `[data-theme="dark"]` 整组覆盖。
- 主题：跟随系统偏好为默认，手动切换持久化到 `localStorage['zimo-theme']`；`index.html` 里的同步脚本在首次绘制前落 `data-theme`，避免暗色下刷新闪一次白。
- `prefers-reduced-motion` 时全部动画降级为静态；动效硬约束：**仅 `transform` / `opacity`**，短时长低频次，禁止常驻高频动画与触发布局/重绘的属性动画。
- 图标一律内联 SVG（描边 2px 风格统一），不用 emoji 做结构图标；正文对比度 ≥ 4.5:1。

## 构建与部署

```bash
cd frontend
npm install          # 首次
npm run dev          # 本地开发(vite dev server;API 仍走 39441,需服务端在跑)
npm run build        # 构建到 dist/
npm run deploy       # dist 镜像到运行时 frontend(见下)
npm run release      # 一键:build + deploy
```

- **运行时目录** = exe 同级的 `Release\workspace\frontend\`（服务端按"exe 同级 frontend"约定加载），deploy 脚本按"多删少补"镜像 dist，并清掉旧 hash 残留与空目录。
- **gzip 孪生**：deploy 最后一步对文本类资源（`.html/.css/.js/.json/.svg/.xml/.txt/.md/.csv`）生成 `.gz`（level 9、无文件名与时间戳头、已最新则跳过），配合服务端 `gzipStatic=true` 直接发送 `.gz`。
- **测试环境副本**：构建时用 `VITE_API_PORT` 指向副本的 RESTful 端口（生产 39441 被占用时用），产物可放到 `dist-test/`。
- 服务端自定义 404 页由 `public/html/404.html` 经构建带入 dist（服务启动时按 `documentRoot\html\404.html` 注册）。
- `dist/`、`dist-test/` 与运行时 `Release\workspace\frontend\` 都不入库。

## 开发约定

- **新增门户模块**：① 在 `src/modules/` 新建 `<kebab>/index.vue`（目录名 = 权限点 code 的 kebab-case）；② 服务端登记权限点（code/index/url 与默认角色）；③ 需要接口时在 `src/api/` 增加对应文件。前端路由、侧边栏、懒加载自动生效，无需改路由表。
- 服务端下发优先：侧边栏 = 模块清单接口；用户管理表格列 = `GET /admin/users/columns` 列元数据（可展示/可编辑/置灰驱动列表与表单）；文件类型分类与图标在前端维护（服务端只回扩展名）。
- 业务接口注册在所属后端模块，前端按域在 `src/api/` 增加对应文件；错误提示分三层：字段级（输入框下）→ 区块级（banner）→ 全局级（Toast）。
- 大内容与长耗时有明确口径：工具类大文本走空闲渲染与超限暂停（[modules/dev-tools/toolbox.js](src/modules/dev-tools/toolbox.js)）；同步型慢接口的超时在 [api/filehub.js](src/api/filehub.js) 里显式放大，不要沿用 15 秒默认值。
- 破坏性操作一律模态二次确认；临时密码、分享链接等一次性信息仅本次展示并提示复制转交。
