# ZiMo 服务管理中心 — 前端

## 技术栈

| 层级 | 技术 |
|------|------|
| 框架 | Vue 3 (CDN, Options API) |
| Markdown | marked.js (CDN) |
| 样式 | 纯 CSS（深色主题，CSS Grid + Flexbox） |
| 路由 | Hash 模式前端路由 |
| 通信 | Fetch API / JSON |

## 项目结构

```
www/
├── index.html          SPA 外壳（4 个页面：首页/文档/接口测试/关于）
├── js/
│   └── app.js          Vue 3 应用（路由/数据/交互）
├── css/
│   └── style.css       深色主题样式
└── README.md           本文件
```

## 设计理念

- **专业运维面板风格** — 深色背景、柔和阴影、清晰信息层级
- **实时数据刷新** — 首页每秒轮询服务器时间和状态
- **零构建** — 从 CDN 加载 Vue 3，无需 Node.js/npm 构建工具
- **响应式** — 适配桌面和移动端管理场景
