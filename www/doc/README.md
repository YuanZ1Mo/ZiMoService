# ZiMo 服务管理中心 — 前端

## 技术栈

| 层级 | 技术 |
|------|------|
| 框架 | Vue 3 (CDN, Options API) |
| 样式 | 纯 CSS（深色主题，CSS Grid + Flexbox） |
| 路由 | Hash 模式前端路由 |
| 通信 | Fetch API / JSON-RPC 2.0（端口 39440） |
| 上传/下载 | XHR / HTTP POST/GET（端口 80） |

## 项目结构

```
www/
├── html/
│   ├── index.html         着陆页（进入控制中心）
│   ├── control.html       控制中心 SPA（首页/文档/接口测试/关于）
│   └── file_hub.html      文件中心（文件管理/上传/下载）
├── js/
│   ├── app.js             control.html 的 Vue 应用
│   └── file_hub.js        file_hub.html 的 Vue 应用
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
  - **文档** — JRPC 方法文档，可折叠展开，支持一键复制和跳转测试
  - **接口测试** — 方法名 + 参数编辑 + 返回体预览
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

## 数据获取

所有业务数据通过 JRPC 调用端口 39440 获取（跨域，服务器已配置 CORS）：

```javascript
const r = await fetch('http://localhost:39440/ZiMo/JRPC', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ method: 'listFiles', params: { path: '' } }),
});
const json = await r.json();
console.log(json.result.files); // [{name, size, type, hasChild, hasPassword}, ...]
```

文件上传/下载直接走 HTTP 80 端口：

```javascript
// 上传
const xhr = new XMLHttpRequest();
xhr.open('POST', '/file_hub/upload/目录/文件名.txt');
xhr.upload.onprogress = (e) => { /* 进度 */ };
xhr.send(file);

// 下载
window.open('/file_hub/download/目录/文件名.txt');
```

## 设计理念

- **专业运维面板风格** — 深色背景、柔和阴影、清晰信息层级
- **零构建** — 从 CDN 加载 Vue 3，无需 Node.js/npm 构建工具
- **悬浮提示** — toast 自动消失，不打断操作流程
- **响应式** — 适配桌面管理场景
