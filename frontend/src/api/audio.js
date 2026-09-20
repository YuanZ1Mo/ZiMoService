// 服务器音频传输层(WebSocket 推流,端点 /zimo/api/serverAudioStream/ws)
//
// 服务端持续推片,客户端只发三类控制帧:
//   sync  建连后 / 重连 / 暂停恢复 / 重建 —— 一律"从直播边缘起"(不回拉历史)
//   pause 暂停:保留连接、服务端停推(仍照常回 pong)
//   pong  心跳应答,回带已收到的最新片号(服务端据此算落后量)
// 停止 = 关闭连接(服务端以连接存活判定听众在离场),不需要 stop 帧。
//
// 帧格式(设计 §3.3):
//   text   JSON:meta / ping / error / auth_failed
//   binary [类型 u8][起始片号 u64 LE] + 载荷
//          类型 0 = WebM 初始化段;类型 1 = 分片批 [片数 u32 LE][长度 u32 LE][片]×N

import { api } from './client'
import { WS_BASE } from './base'

const WS_URL = `${WS_BASE}/serverAudioStream/ws`

/// 会话失效:与 client.js 同语义(WS 帧通知,页面自行跳登录并带回跳)
export function goLogin() {
  const target = location.pathname + location.search
  if (location.pathname === '/login') return
  location.href = `/login?redirect=${encodeURIComponent(target)}`
}

/// 客户端标识:每次开始收听生成一次,字符集 [A-Za-z0-9_-]{1,32},
/// 用于服务端区分同一账号的多端/多标签
export function newClientId() {
  const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789'
  let rand = ''
  try {
    const a = new Uint8Array(6)
    crypto.getRandomValues(a)
    for (const b of a) rand += alphabet[b % alphabet.length]
  } catch {
    rand = Math.random().toString(36).slice(2, 8)
  }
  return `c${Date.now().toString(36)}${rand}`.slice(0, 32)
}

/// 拆批:片数 N(u32 LE) + N 组[长度 L(u32 LE) + L 字节的片];N 可为 0
/// @param buf 载荷的 ArrayBuffer(不是 Uint8Array:DataView 只接受 ArrayBuffer)
export function splitBatch(buf) {
  const view = new DataView(buf)
  const n = view.getUint32(0, true)
  const segs = []
  let off = 4
  for (let i = 0; i < n; i++) {
    if (off + 4 > buf.byteLength) break
    const len = view.getUint32(off, true)
    off += 4
    if (off + len > buf.byteLength) break
    segs.push(new Uint8Array(buf, off, len))
    off += len
  }
  return segs
}

/// 解析 binary 帧头:[类型 u8][起始片号 u64 LE] + 载荷
/// 载荷切出独立 ArrayBuffer:appendBuffer 与 splitBatch 都按 ArrayBuffer 取用
/// (Uint8Array 视图传进 DataView 会直接抛异常)
function parseBinary(buf) {
  const view = new DataView(buf)
  const type = view.getUint8(0)
  // 64 位片号:按两个 32 位拼(片号远小于 2^53,不会有精度问题)
  const lo = view.getUint32(1, true)
  const hi = view.getUint32(5, true)
  return { type, startSeq: hi * 4294967296 + lo, payload: buf.slice(9) }
}

/**
 * 建连并返回连接对象;帧已按类型分发,player 只面向回调
 *
 * @param {Object}   opt
 * @param {string}   opt.client    客户端标识(sync 帧携带)
 * @param {Function} opt.onMeta    流描述(text meta)
 * @param {Function} opt.onInit    (起始片号, 初始化段字节)
 * @param {Function} opt.onBatch   (起始片号, 分片数组)
 * @param {Function} opt.onError   服务端控制错误/auth_failed(JSON 对象)
 * @param {Function} opt.onClose   连接关闭(重连决策交给 player,本层不自动重连)
 * @param {Function} opt.getPosSeq 返回"播放头对应的片号"(未起播返回 0);心跳应答带上它,
 *                                 服务端据此算出该听众"听到的位置比实况晚多少"
 * @return {{ws:WebSocket, liveSeq:number, send:Function, close:Function, closed:boolean}}
 */
export function openAudioWs({ client, onMeta, onInit, onBatch, onError, onClose, getPosSeq }) {
  const ws = new WebSocket(WS_URL)
  ws.binaryType = 'arraybuffer'

  const conn = {
    ws,
    closed: false,
    liveSeq: 0, ///< 已收到的最新片号(pong 回带用)
    send(obj) {
      if (this.closed || ws.readyState !== WebSocket.OPEN) return
      try {
        ws.send(JSON.stringify(obj))
      } catch {
        /* 连接正在关闭:丢弃该控制帧 */
      }
    },
    close() {
      if (this.closed) return
      this.closed = true
      try {
        ws.close()
      } catch {
        /* 忽略 */
      }
    }
  }

  ws.onopen = () => conn.send({ type: 'sync', client })

  ws.onmessage = (ev) => {
    if (conn.closed) return
    try {
      if (typeof ev.data === 'string') {
        const j = JSON.parse(ev.data)
        if (!j || !j.type) return
        if (j.type === 'meta') onMeta && onMeta(j)
        else if (j.type === 'ping') {
          // seq = 已收到的最新片;pos = 播放头所在片(服务端算落后量用)
          let pos = 0
          try {
            pos = getPosSeq ? Number(getPosSeq()) || 0 : 0
          } catch {
            pos = 0
          }
          conn.send({ type: 'pong', seq: conn.liveSeq, pos })
        } else if (j.type === 'error' || j.type === 'auth_failed') onError && onError(j)
        return
      }
      const { type, startSeq, payload } = parseBinary(ev.data)
      if (type === 0) {
        onInit && onInit(startSeq, payload)
      } else if (type === 1) {
        const segs = splitBatch(payload)
        if (segs.length) conn.liveSeq = Math.max(conn.liveSeq, startSeq + segs.length - 1)
        onBatch && onBatch(startSeq, segs)
      }
    } catch (e) {
      // 单帧解析失败不影响后续帧,但必须留痕:静默吞掉"每帧都失败"会让整条链路无声地瘫掉
      if (!conn.parseWarned) {
        conn.parseWarned = true
        console.warn('[audio] 帧解析失败:', e && e.message)
      }
    }
  }

  // 错误统一收敛到 onclose(浏览器在错误后必然关闭连接)
  ws.onerror = () => {}

  ws.onclose = () => {
    if (conn.closed) return
    conn.closed = true
    onClose && onClose()
  }

  return conn
}

export const audioApi = {
  /// 状态与收听信息(沿用通用封装:401 统一跳登录与错误码映射)
  status: () => api.get('/serverAudioStream/status')
}
