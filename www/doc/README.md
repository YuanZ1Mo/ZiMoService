# ZiMo 服务管理中心 — 前端

## 技术栈

| 层级 | 技术 |
|------|------|
| 框架 | Vue 3 (CDN, Options API) |
| Markdown | marked.js (CDN) |
| 样式 | 纯 CSS（深色主题，CSS Grid + Flexbox） |
| 路由 | Hash 模式前端路由 |
| 通信 | Fetch API / JSON-RPC 2.0（端口 39440） |

## 项目结构

```
www/
├── html/
│   ├── index.html       着陆页（进入控制中心入口）
│   └── control.html     控制中心 SPA（首页/文档/接口测试/关于）
├── js/
│   └── app.js           Vue 3 应用（路由/数据/JRPC 调用）
├── css/
│   └── style.css        深色主题样式
└── doc/
    └── README.md        本文件
```

## 页面说明

- **着陆页（/）** — 简洁入口页，仅包含"进入控制中心"按钮
- **控制中心（/control）** — 管理面板 SPA：
  - **首页** — 实时时钟 + 服务状态卡片 + CPU/内存/GPU 负载（每秒刷新）
  - **文档** — JRPC 方法文档，可折叠展开，支持一键复制和跳转测试
  - **接口测试** — 方法名 + 参数编辑 + 返回体预览，直接调用 JRPC 接口
  - **关于** — 从 README.md 动态读取渲染

## 数据获取

所有业务数据通过 JRPC 调用端口 39440 获取（跨域，服务器已配置 CORS）：

```javascript
// JRPC 调用示例
const r = await fetch('http://localhost:39440/ZiMo/JRPC', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ method: 'getStatus', params: {} }),
});
const json = await r.json();
console.log(json.result); // 业务数据
```

## 设计理念

- **专业运维面板风格** — 深色背景、柔和阴影、清晰信息层级
- **实时数据刷新** — 首页每秒轮询服务器时间和状态
- **零构建** — 从 CDN 加载 Vue 3，无需 Node.js/npm 构建工具
- **响应式** — 适配桌面和移动端管理场景
