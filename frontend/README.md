# ZiMo 门户前端

ZiMo 用户系统的前端单页应用（SPA），基于 **Vue 3 + Vite 8 + Pinia + Vue Router**（纯 JS，无 TS）。
构建产物为单一 `index.html` + 静态资源，由服务端（drogon）托管；页面路由全部在前端处理，服务端统一 SPA 回落 + 鉴权门禁。

设计基准与交互规范见 [docs/designs/mockups/user-system/](../docs/designs/mockups/user-system/index.html)（已评审的设计预览稿，含配色/字体/令牌/组件规范）。

## 端口与拓扑

| 面 | 端口 | 说明 |
|---|---|---|
| 页面 | 80 / 443 | SPA 页面 + 静态资源；未带有效会话访问非白名单路径 → 302 `/login` |
| RESTful API | 39441 | `/zimo/api/*`，与页面跨端口同站（cookie `SameSite=Lax` + `credentials: include`） |
| JSON-RPC | 39440 | 平台面，前端不直接使用 |

- API 基址在 [src/api/client.js](src/api/client.js) 中按 `location.hostname:39441` 动态构造，本地开发需服务端在跑（登录/注册/门户接口都走它）。
- 静态资源引用一律**根绝对路径**（`/assets/...`）。

## 目录结构

```
frontend/
├── index.html                 # SPA 唯一入口(引用 /favicon.svg)
├── vite.config.js             # base='/' , outDir=dist
├── public/                    # 原样拷贝进 dist 的静态文件(目录可自由组织)
│   ├── svg/favicon.svg        #   站点图标(index.html 引用 /svg/favicon.svg)
│   └── html/404.html          #   服务端自定义 404 页(drogon setCustom404Page 注册)
├── scripts/
│   └── deploy.mjs             # 部署脚本:dist 镜像到运行时 frontend(多删少补)
└── src/
    ├── main.js                # 入口:挂载 + 全局样式引入
    ├── App.vue                # 根组件:路由出口 + 全局 Toast(inject('toast'))
    ├── router/index.js        # 路由表 + 全局守卫(会话探测/force_change/回跳)
    ├── api/                   # fetch 封装与按域拆分的接口
    │   ├── client.js          #   credentials、401/403 统一处理、错误码映射
    │   ├── auth.js            #   /auth/*:注册/登录/登出/强制改密/心跳
    │   ├── portal.js          #   /portal/*:模块清单/主页
    │   └── admin.js           #   /admin/*:用户管理
    ├── stores/
    │   ├── session.js         # 会话上下文(三态 isLoggedIn)、force_change、心跳状态
    │   └── modules.js         # 模块清单(服务端下发)+ policyVersion 轮询应答
    ├── layouts/
    │   └── PortalLayout.vue   # 门户壳:固定顶栏(心跳胶囊/用户菜单/退出)+抽屉导航+keep-alive 工作区
    ├── pages/                 # 通用页(免会话白名单页,与门禁白名单一一对应)
    │   ├── LoginView.vue      #   /login
    │   ├── RegisterView.vue   #   /register
    │   ├── ResetView.vue      #   /reset   (预留,不可用)
    │   ├── ForceResetView.vue #   /force-reset
    │   └── NotFoundView.vue   #   /404
    ├── modules/               # 门户业务模块(懒加载 + keep-alive)
    │   ├── home/index.vue             #   用户主页      code=home       index=1
    │   └── user-manager/index.vue     #   用户管理      code=userManager index=-1
    ├── components/            # 跨页通用组件
    │   ├── ThemeSwitch.vue    #   亮/暗/跟随系统 三态循环(持久化 localStorage 'zimo-theme')
    │   ├── PasswordStrength.vue
    │   └── Modal.vue          #   模态(遮罩点击/Esc 关闭)
    └── styles/
        ├── base.css           #   设计令牌(亮暗双主题)+通用组件样式(按钮/表单/徽章/表格/Toast…)
        ├── theme.css          #   主题切换按钮与 color-scheme
        └── portal.css         #   门户壳布局(顶栏/抽屉导航/工作区/分页)
```

## 架构要点

### 模块化机制（code → 路由）

- `src/modules/<kebab>/index.vue` 的**目录名 = 权限点 code 的小写 kebab-case**（`userManager` → `user-manager`）。
- 路由用 `import.meta.glob` 按 modules 目录自动生成，挂载在 `/portal` 下：`/portal/home`、`/portal/user-manager`。
- 侧边栏渲染由服务端 `GET /portal/modules` 下发的 `{code,name,url,index}` 驱动，排序规则：正数升序在前，负数区（-1 最先）依次在后；无权限的模块不渲染、路由不可达（前端守卫 + 服务端 403 双重拦截）。
- 工作区用 `<keep-alive>` 缓存模块组件，切换不销毁运行状态（`onActivated`/`onDeactivated`）。

### 鉴权与会话

- 会话 cookie `zm_session`（HttpOnly），所有 API 请求 `credentials: include`。
- 路由守卫（[router/index.js](src/router/index.js)）：
  - 冷启动（整页刷新）先带 cookie 探测 `/portal/home` 恢复会话，再重走守卫；
  - 未登录访问受保护页 → 记 `redirect` 跳 `/login`，登录后回跳；
  - `force_change=1` 会话仅允许 `/force-reset` 与登出，其余一律 403 引导。
- API 层（[api/client.js](src/api/client.js)）统一处理：401 → 记回跳跳登录；403 `FORCE_CHANGE_REQUIRED` → 跳 `/force-reset`；`{code,message}` 错误码映射到字段级/表单级报错。
- 心跳：门户壳每 10s `POST /auth/heartbeat`；401 立即下线，网络中断仅提示不强制下线；响应携带 `policyVersion`，版本变化时按需重新拉取模块清单。

### 视觉与主题

- 设计令牌集中在 [styles/base.css](src/styles/base.css)（语义变量 `--color-*` / 圆角 / 阴影 / 动效时长），暗色主题经 `[data-theme="dark"]` 整组覆盖。
- 主题：跟随系统偏好为默认，手动切换持久化；`prefers-reduced-motion` 时全部动画降级为静态。
- 动效硬约束：**仅 `transform` / `opacity`**，短时长低频次，禁止常驻高频动画与触发布局/重绘的属性动画。
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

- **运行时目录** = exe 同级的 `Release\workspace\frontend\`（服务端按"exe 同级 frontend"约定加载），deploy 脚本会把 dist 完整镜像过去并清理旧 hash 残留。
- **gzip 孪生**：deploy 最后一步自动对文本类资源（html/css/js/json/svg…）生成 `.gz`（最高压缩、增量跳过、确定性输出），配合服务端 `gzipStatic=true` 实现静态压缩传输（实测 JS 体积省 ~65%）。
- 服务端自定义 404 页由 `public/html/404.html` 经构建带入 dist（服务启动时按 `documentRoot\html\404.html` 注册）。
- `Release\workspace\frontend` 与 `dist` 均不入 git；仓库内只保存源码与 `public/` 输入。

## 开发约定

- **新增门户模块**：① 在 `src/modules/` 新建 `<kebab>/index.vue`；② 服务端 `permissions` 表加权限点（code/index/url）。前端路由、侧边栏、懒加载自动生效，无需改路由表。
- 服务端下发优先：侧边栏 = 模块清单接口；用户管理表格列 = `GET /admin/users/columns` 列元数据（可展示/可编辑/置灰驱动列表与表单）。改列配置不动前端代码。
- 业务接口注册在所属后端模块，前端按域在 `src/api/` 增加对应文件；错误提示分三层：字段级（输入框下）→ 区块级（banner）→ 全局级（Toast）。
- 破坏性操作一律模态二次确认；临时密码等一次性信息仅本次展示并提示复制转交。
