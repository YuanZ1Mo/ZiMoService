// 业务 API 基址:页面端口(80/443)与 RESTful 端口同站跨端口
//
// 端口可用构建期变量 VITE_API_PORT 覆盖 —— ZiMoTest 的测试环境副本要把 RESTful 面
// 挪到空闲端口(否则与在跑的生产服务抢 39441),构建测试包时用它指向副本端口。
export const API_PORT = import.meta.env.VITE_API_PORT || '39441'

export const API_BASE = `${location.protocol}//${location.hostname}:${API_PORT}/zimo/api`

/// 同一端口的 WebSocket 基址(http → ws,https → wss)
export const WS_BASE = `${API_BASE.replace(/^http/, 'ws')}`
