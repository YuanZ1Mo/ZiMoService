# ZiMo 服务管理中心 — 前端

## 技术栈

| 层级 | 技术 |
|------|------|
| 框架 | Vue 3 (CDN, Options API) |
| 样式 | 纯 CSS（深色主题，CSS Grid + Flexbox） |
| 路由 | Hash 模式前端路由 |
| 通信 | Fetch API / RESTful API（端口 39441，★ 已从 JRPC 迁移） |
| 上传/下载 | XHR / HTTP POST/GET（端口 80） |

## 项目结构

```
www/
├── html/
│   ├── index.html         着陆页（进入控制中心）
│   ├── control.html       控制中心 SPA（首页/文档/接口测试/关于）
│   ├── filehub.html       文件中心（文件管理/上传/下载）
│   └── 404.html           自定义 404 页面（兜底展示）
├── js/
│   ├── app.js             control.html 的 Vue 应用
│   └── filehub.js         filehub.html 的 Vue 应用
├── css/
│   └── style.css          深色主题样式（含文件中心样式）
├── db/filehub/            文件中心数据目录（不可通过 URL 直接访问）
└── doc/
    └── README.md          本文件
```

## 页面说明

- **着陆页（/）** — "进入控制中心"按钮
- **控制中心（/control）** — 管理面板 SPA：
  - **首页** — 实时时钟 + 服务状态卡片 + CPU/内存/GPU 负载（每秒刷新）
  - **文档** — ★ RESTful API 方法文档（含 JRPC 兼容文档），可折叠展开，支持一键复制和跳转测试
  - **接口测试** — ★ RESTful 接口测试（含 JRPC 兼容测试模块），参数编辑 + 返回体预览
  - **文件中心** — 点击跳转到文件中心页面
  - **关于** — 从 README.md 动态读取渲染
- **文件中心（/filehub）** — 文件管理页面：
  - **面包屑导航** — 首页 › 文件中心 › 目录层级
  - **文件列表** — 文件夹在前/文件在后，🔒 标识密码保护的目录
  - **搜索** — 实时过滤当前目录内容，toast 悬浮提示结果数
  - **上传** — 多文件选择 + XHR 进度条 + mmap 零拷贝存储
  - **下载** — 零拷贝 + Range 断点续传
  - **密码管理** — HMAC-SHA256，输入法自动禁用中文
  - **模态框** — 模糊背景效果，点击外部不关闭
- **404（/404）** — 未匹配路由的兜底展示页

## 数据获取

★ 所有业务数据统一通过 RESTful API 调用端口 39441 获取（跨域，服务器已配置 CORS）：

```javascript
// RESTful API（推荐）
const r = await fetch('http://localhost:39441/zimo/api/files?path=');
const json = await r.json();
console.log(json.files); // [{name, size, type, hasChild, hasPassword}, ...]

// 心跳
await fetch('http://localhost:39441/zimo/api/ping');

// 服务状态
const status = await (await fetch('http://localhost:39441/zimo/api/status')).json();

// SSE 事件流
const es = new EventSource('http://localhost:39441/zimo/api/events');
es.onmessage = (e) => console.log(JSON.parse(e.data));

// 广播消息
await fetch('http://localhost:39441/zimo/api/broadcast?topic=hello&content=world&tag=demo', {
  method: 'POST'
});

// 上传文件
const xhr = new XMLHttpRequest();
xhr.open('POST', 'http://localhost:39441/zimo/api/files/upload?path=file.zip');
xhr.upload.onprogress = (e) => { /* 进度 */ };
xhr.send(file);

// 下载文件
window.open('http://localhost:39441/zimo/api/files/download?path=file.zip');
```

### 文件中心 RESTful API 完整列表

| 方法 | 路径 | Query 参数 | 说明 |
|------|------|-----------|------|
| GET | `/files` | `path` | 列出目录下的文件和文件夹 |
| GET | `/files/search` | `keyword` | 模糊搜索文件/文件夹名 |
| POST | `/files/dirs` | `path, dirName, username?, password?` | 创建目录（可选密码保护） |
| DELETE | `/files` | `path, username?, password?` | 删除文件或空目录 |
| GET/POST | `/files/verify-password` | `path, password` | 验证目录密码 |
| PUT | `/files/password` | `path, username, oldPassword, newPassword` | 修改目录密码 |
| POST | `/files/batch-delete` | `paths (逗号分隔), username?, password?` | 批量删除文件 |
| POST | `/files/upload` | `path` (body=二进制) | 上传文件 |
| GET | `/files/download` | `path` (支持 Range) | 下载文件 |

### 兼容 JRPC（端口 39440）

旧版 JRPC 接口仍在端口 39440 可用，方法名不变（`listFiles`/`searchFiles`/`createDir` 等），推荐新功能迁移到 RESTful API。

```javascript
// JRPC（兼容保留）
const r = await fetch('http://localhost:39440/zimo/jrpc', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'listFiles', params: { path: '' } }),
});
const json = await r.json();
console.log(json.result.files);
```

## 设计理念

- **专业运维面板风格** — 深色背景、柔和阴影、清晰信息层级
- **零构建** — 从 CDN 加载 Vue 3，无需 Node.js/npm 构建工具
- **悬浮提示** — toast 自动消失，不打断操作流程
- **响应式** — 适配桌面管理场景
- **RESTful 优先** — 语义化 HTTP 方法 + Query 参数，与 JRPC 兼容共存
