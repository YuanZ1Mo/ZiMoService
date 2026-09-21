<script setup>
// 服务器音频模块页(/portal/server-audio-stream)
// 页面只做视图与交互:连接/缓冲/后台策略/错误收敛都在 player.js
// keep-alive 常驻:切走不销毁、播放继续;onDeactivated 只停特效动画并转入后台策略
import { ref, computed, watch, nextTick, onMounted, onBeforeUnmount, onActivated, onDeactivated, inject } from 'vue'
import { AudioPlayer, isPlaybackSupported } from './player'
import { audioApi } from '../../api/audio'
import './audio-stream.css'

const toast = inject('toast')
// 门户壳会向模块页透传 :home(本模块用不到,声明以避开落在根元素上的 fallthrough 属性)
defineProps({ home: { type: Object, default: null } })

/* ---------- 状态字典:文案与需求 §6.4 / 交接 §6.4 逐字对应 ---------- */
const ICONS = {
  play: '<svg width="46" height="46" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M8.6 5.4a1.2 1.2 0 0 1 1.82-1.03l8.1 5.6a1.2 1.2 0 0 1 0 1.98l-8.1 5.6A1.2 1.2 0 0 1 8.6 16.6Z"/></svg>',
  pause: '<svg width="44" height="44" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="6.6" y="5" width="3.9" height="14" rx="1.6"/><rect x="13.5" y="5" width="3.9" height="14" rx="1.6"/></svg>',
  spinner: '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 3.4a8.6 8.6 0 0 1 8.6 8.6"/><path d="M12 20.6A8.6 8.6 0 0 1 3.4 12" opacity=".45"/><path d="M3.4 12A8.6 8.6 0 0 1 12 3.4" opacity=".28"/></svg>',
  refresh: '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" aria-hidden="true"><path d="M20.6 12a8.6 8.6 0 1 1-2.5-6.05"/><path d="M20.6 4.2v5.4h-5.4"/></svg>',
  warn: '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 4.6 2.9 19.6a1.1 1.1 0 0 0 .95 1.65h16.3a1.1 1.1 0 0 0 .95-1.65Z"/><path d="M12 10.2v4.3"/><circle cx="12" cy="17.6" r="1.05" fill="currentColor" stroke="none"/></svg>',
  info: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11.2v5"/><circle cx="12" cy="7.8" r="1.05" fill="currentColor" stroke="none"/></svg>',
  wave: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M3 12h2.6l1.8-4.6L10 17l2.6-6.4L14.6 14h6.4"/></svg>',
  spinSm: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M12 3.6a8.4 8.4 0 0 1 8.4 8.4"/><path d="M20.4 12A8.4 8.4 0 0 1 12 20.4" opacity=".5"/></svg>',
  warnSm: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 4.6 2.9 19.6a1.1 1.1 0 0 0 .95 1.65h16.3a1.1 1.1 0 0 0 .95-1.65Z"/><path d="M12 10.2v4.3"/><circle cx="12" cy="17.6" r="1.05" fill="currentColor" stroke="none"/></svg>',
  pauseSm: '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="7" y="5.6" width="3.6" height="12.8" rx="1.5"/><rect x="13.4" y="5.6" width="3.6" height="12.8" rx="1.5"/></svg>'
}
const STATES = {
  idle: { text: '聆听服务器', extra: '', ico: 'play', sico: 'info', cls: 'idle', tone: '', chip: '空闲', label: '开始收听' },
  connecting: { text: '连接中…', extra: '等待首片就绪', ico: 'spinner', sico: 'spinSm', cls: 'connecting', tone: '', chip: '准备中', label: '取消' },
  playing: { text: '正在播放', extra: '', ico: 'pause', sico: 'wave', cls: 'playing', tone: 'ok', chip: '采集中', label: '暂停' },
  paused: { text: '已暂停', extra: '点击继续,从服务器当前声音接着听', ico: 'play', sico: 'pauseSm', cls: 'paused', tone: '', chip: '采集中', label: '继续播放' },
  reconnecting: { text: '连接中断,重连中…', extra: '', ico: 'refresh', sico: 'warnSm', cls: 'reconnecting', tone: 'warn', chip: '重连中', label: '停止' },
  stalled: { text: '播放卡顿,正在恢复', extra: '', ico: 'refresh', sico: 'warnSm', cls: 'reconnecting', tone: 'warn', chip: '恢复中', label: '停止' },
  'no-device': { text: '服务器无可用音频设备', extra: '请检查默认播放设备后重试', ico: 'warn', sico: 'warnSm', cls: 'nodevice', tone: 'err', chip: '设备不可用', label: '重试' }
}

/* ---------- 视图状态 ---------- */
const supported = isPlaybackSupported()
const audioEl = ref(null)
const canvasEl = ref(null)
const stageEl = ref(null)
const discEl = ref(null)

const state = ref('idle')
const stateDetail = ref('')      // reconnecting 的原因
const hint = ref('')             // 参数行/舞台下方的一次性提示(410 重排、换流等)
const format = ref({ sampleRate: 0, channels: 0, bitrate: 0 })
const segmentMs = ref(100)
const delayMs = ref(null)
/// 诊断读数:URL 带 ?debug=1 时显示(默认不显示,不打扰普通使用)
const debugOn = new URLSearchParams(location.search).has('debug')
const debugText = ref('')
/// 音量记忆:默认 100,用户调过就记住(只存本地)
const VOLUME_KEY = 'zimo-audio-volume'
function loadVolume() {
  try {
    const raw = localStorage.getItem(VOLUME_KEY)
    if (raw === null || raw === '') return 100   // 从未存过:用默认值
    const v = Number(raw)
    return Number.isFinite(v) && v >= 0 && v <= 100 ? v : 100
  } catch {
    return 100   // 隐私模式/禁用存储:退回默认
  }
}
const volume = ref(loadVolume())

const status = ref(null)         // /status 结果(capturing / listenerCount / listeners / bufferFillSec)
const ownClient = ref('')        // 本次收听使用的客户端标识(用于把"本机"标出来)
let lastStatusAt = 0

const cur = computed(() => STATES[state.value] || STATES.idle)
const discIcon = computed(() => ICONS[cur.value.ico] || ICONS.info)
const statusIcon = computed(() => ICONS[cur.value.sico] || ICONS.info)
const showDetail = computed(() => stateDetail.value || cur.value.extra)
const isActive = computed(() => state.value !== 'idle' && state.value !== 'no-device')

/* 参数行:格式参数与延迟(连接后才有真值,连接前用 /status 的快照填充) */
const fmt = computed(() => {
  const f = format.value
  const sr = f.sampleRate || (status.value && status.value.sampleRate) || 0
  const ch = f.channels || (status.value && status.value.channels) || 0
  const br = f.bitrate || (status.value && status.value.bitrate) || 0
  return {
    sampleRate: sr ? `${Math.round(sr / 1000)}kHz` : '— kHz',
    channels: ch === 2 ? '立体声' : ch === 1 ? '单声道' : '— 声道',
    bitrate: br ? `${Math.round(br / 1000)}kbps` : '— kbps',
    segmentMs: segmentMs.value || (status.value && status.value.segmentDurationMs) || 0
  }
})
const delayText = computed(() => {
  if (!isActive.value || delayMs.value == null) return '—'
  return String(delayMs.value)
})

/* 收听信息:人数人人可见;明细仅在服务端下发 listeners 字段时渲染(不自己判权限) */
const listeners = computed(() => (status.value && Array.isArray(status.value.listeners) ? status.value.listeners : null))
const listenerCount = computed(() => {
  if (!isActive.value) return 0
  return (status.value && Number(status.value.listenerCount)) || 0
})
const listenHint = computed(() => {
  if (!isActive.value) return '按需采集:无人收听时采集线程自行停止并释放设备'
  if (!listeners.value) return '明细(账号/设备/IP)仅开发者与管理员可见'
  return ''
})
/// 落后量文案:-1 = 该听众尚未上报游标(刚建连或页面被冻结),不是"落后很久"
function fmtLag(ms) {
  const n = Number(ms)
  if (!Number.isFinite(n) || n < 0) return '未上报'
  const s = n / 1000
  return s >= 10 ? `${s.toFixed(0)}s` : `${s.toFixed(1)}s`
}
function fmtSince(ms) {
  const min = Math.max(0, Math.round((Date.now() - (Number(ms) || 0)) / 60000))
  return min >= 1 ? `已听 ${min} 分` : '刚加入'
}

/* ---------- 播放器 ---------- */
let player = null

function onNotice(kind) {
  if (kind === 'session') toast('会话已失效,正在跳转登录…', 'warn')
  else if (kind === 'forbidden') toast('权限不足,收听已停止', 'err')
  else if (kind === 'noDevice') toast('服务器无可用音频设备', 'warn')
  else if (kind === 'blocked') toast('浏览器阻止了自动播放,请点击开始继续', 'warn')
  else if (kind === 'streamGone') flashHint('流已更换,已重新连接并回到直播边缘')
  else if (kind === 'segmentGone') flashHint('已跳过过旧分片,回到直播边缘')
}
let hintTimer = null
function flashHint(msg) {
  hint.value = msg
  if (hintTimer) clearTimeout(hintTimer)
  hintTimer = setTimeout(() => { hint.value = '' }, 5000)
}

function onDisc() {
  if (!player) return
  switch (state.value) {
    case 'playing':
      player.stop()
      break
    case 'paused':
      player.resume()
      break
    case 'connecting':
    case 'reconnecting':
      player.stop()
      break
    default:
      // idle / no-device:重试开始(点击即用户手势,起播据此放行)
      player.start()
  }
}

/* ---------- /status:5s 轮询(仅播放期间);页面激活时补一次快照 ---------- */
async function refreshStatus() {
  if (Date.now() - lastStatusAt < 800) return
  lastStatusAt = Date.now()
  try {
    status.value = await audioApi.status()
    if (player) player.setListenerCount(listenerCount.value)
  } catch {
    /* 观察类请求失败不打断播放 */
  }
}

/* ---------- 特效:按播放时间驱动的律动(不接音频分析) ---------- */
const reduceMotion = typeof window !== 'undefined' && window.matchMedia
  ? window.matchMedia('(prefers-reduced-motion: reduce)').matches
  : false
let raf = null
let lastTs = 0
let t = 0
let rot = 0

const PAL = {
  idle: ['#38BDF8', '#22D3EE', '#7DD3FC'],
  connecting: ['#38BDF8', '#7DD3FC', '#22D3EE'],
  playing: ['#22D3EE', '#38BDF8', '#F472B6'],
  paused: ['#38BDF8', '#22D3EE', '#7DD3FC'],
  reconnecting: ['#FBBF24', '#F59E0B', '#FCD34D'],
  'no-device': ['#F87171', '#FBBF24', '#FCA5A5']
}
const LEVEL = { idle: 0.07, connecting: 0.2, playing: 0.62, paused: 0.1, reconnecting: 0.24, 'no-device': 0.04 }
const SPIN = { idle: 0.05, connecting: 0.5, playing: 0.34, paused: 0.05, reconnecting: 0.28, 'no-device': 0.02 }

function rgba(hex, a) {
  const n = parseInt(hex.slice(1), 16)
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`
}

let fxGeo = { w: 0, h: 0, cx: 0, cy: 0, r: 90 }
function measureFx() {
  const cv = canvasEl.value
  const disc = discEl.value
  if (!cv || !disc) return
  const cr = cv.getBoundingClientRect()
  const dr = disc.getBoundingClientRect()
  if (!cr.width || !cr.height) return
  const dpr = Math.min(window.devicePixelRatio || 1, 2)
  cv.width = Math.round(cr.width * dpr)
  cv.height = Math.round(cr.height * dpr)
  const ctx = cv.getContext('2d')
  if (ctx) ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  fxGeo = {
    w: cr.width,
    h: cr.height,
    cx: dr.left - cr.left + dr.width / 2,
    cy: dr.top - cr.top + dr.height / 2,
    r: dr.width / 2 + 4
  }
}

// 状态/提示文案变化会推动圆盘位置(整行文字的增减),重新量一次,别让背后的圆环偏掉
watch([state, stateDetail, hint], () => {
  nextTick(measureFx)
})

function drawFx(dt) {
  const cv = canvasEl.value
  if (!cv || !fxGeo.w) return
  const ctx = cv.getContext('2d')
  if (!ctx) return
  const key = state.value
  const pal = PAL[key] || PAL.idle
  const lv = LEVEL[key] || 0.08
  const spin = SPIN[key] || 0.1
  const { w, h, cx, cy, r: R } = fxGeo
  rot += dt * spin
  t += dt
  ctx.clearRect(0, 0, w, h)

  // 盘后柔光
  const g = ctx.createRadialGradient(cx, cy, R * 0.4, cx, cy, Math.max(R * 2.5, 180))
  g.addColorStop(0, rgba(pal[0], 0.16 + 0.3 * lv))
  g.addColorStop(0.55, rgba(pal[1], 0.05 + 0.1 * lv))
  g.addColorStop(1, 'rgba(0,0,0,0)')
  ctx.fillStyle = g
  ctx.beginPath()
  ctx.arc(cx, cy, Math.max(R * 2.5, 180), 0, Math.PI * 2)
  ctx.fill()

  // 放射律动条
  const N = 68
  const maxAmp = Math.min(R * 0.5, 86)
  ctx.lineCap = 'round'
  for (let i = 0; i < N; i++) {
    const a = (i / N) * Math.PI * 2 + rot
    const wv = Math.sin(t * 1.35 + i * 0.42) * 0.5 + 0.5
    const wv2 = Math.sin(t * 0.62 + i * 0.19 + 1.3) * 0.5 + 0.5
    const amp = lv * (0.3 + 0.7 * wv * wv2) * maxAmp
    if (amp < 0.6) continue
    const r0 = R + 10
    const r1 = r0 + amp
    ctx.strokeStyle = rgba(pal[i % 3], 0.18 + 0.62 * wv * Math.min(lv * 1.8, 1))
    ctx.lineWidth = i % 4 === 0 ? 3.4 : 2.2
    ctx.beginPath()
    ctx.moveTo(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0)
    ctx.lineTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
    ctx.stroke()
  }

  // 基环 + 呼吸环
  ctx.strokeStyle = rgba(pal[0], 0.16 + 0.18 * lv)
  ctx.lineWidth = 1.2
  ctx.beginPath()
  ctx.arc(cx, cy, R + 6, 0, Math.PI * 2)
  ctx.stroke()
  const breath = 0.5 + 0.5 * Math.sin(t * 1.1)
  ctx.strokeStyle = rgba(pal[2], 0.06 + 0.2 * breath * Math.min(lv * 2.4, 1))
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.arc(cx, cy, R + 26 + breath * 10, 0, Math.PI * 2)
  ctx.stroke()

  // 外圈虚线环(缓慢旋转)
  ctx.save()
  ctx.setLineDash([3, 9])
  ctx.strokeStyle = rgba(pal[1], 0.2 + 0.12 * lv)
  ctx.lineWidth = 1.1
  ctx.beginPath()
  ctx.arc(cx, cy, R + 52, rot * 0.6, rot * 0.6 + Math.PI * 2)
  ctx.stroke()
  ctx.restore()
}

function loop(ts) {
  const dt = lastTs ? Math.min((ts - lastTs) / 1000, 0.05) : 0.016
  lastTs = ts
  drawFx(dt)
  raf = requestAnimationFrame(loop)
}

function startFx() {
  if (raf != null || reduceMotion) return
  if (!isActive.value) return
  lastTs = 0
  raf = requestAnimationFrame(loop)
}
function stopFx() {
  if (raf != null) {
    cancelAnimationFrame(raf)
    raf = null
  }
}
/// 状态变化:活动态跑律动,空闲/无设备只画一帧静止画面(不常驻产帧)
function syncFx() {
  measureFx()
  if (reduceMotion || !isActive.value) {
    stopFx()
    drawFx(0.016)
  } else {
    startFx()
  }
}

/* ---------- 生命周期 ---------- */
let statusTimer = null
let delayTimer = null
let resizeObs = null

function onVisibility() {
  if (!player) return
  if (document.hidden) player.enterBackground()
  else {
    // 回前台:先确认收帧链路没有在后台期间烂掉,再切回前台档位(防滞留假死)
    player.checkStale()
    player.enterForeground()
    refreshStatus()
    setTimeout(syncFx, 60)
  }
}

onMounted(() => {
  if (!supported) return
  player = new AudioPlayer({
    audio: audioEl.value,
    onState: (key, detail) => {
      state.value = key
      stateDetail.value = detail || ''
      ownClient.value = player ? player.client : ''
      syncFx()
      if (key !== 'playing') delayMs.value = null
      if (key === 'idle' || key === 'no-device') status.value = null
      refreshStatus()
    },
    onInfo: () => {
      format.value = { ...player.format }
      segmentMs.value = player.segmentMs
    },
    onNotice
  })
  player.setVolume(volume.value)

  // 延迟每秒刷新一次(EWMA 平滑:界面读数随分片批次存在 ±100ms 量化锯齿,平滑后更接近感知值)
  delayTimer = setInterval(() => {
    const raw = player ? player.delayMs : null
    if (raw == null) delayMs.value = null
    else if (delayMs.value == null) delayMs.value = raw
    else delayMs.value = Math.round(delayMs.value * 0.5 + raw * 0.5)
    // 诊断行(?debug=1):目标 = max(余量, 抖动折算, 起播片数)——谁最大谁在决定延迟
    if (debugOn && player && player.active) {
      const d = player.debugInfo
      debugText.value =
        `余量 ${d.reserve}s · 抖动 ${d.jitter}ms · 目标 ${d.target}s · 实测余量 ${d.lead}s · 额度 ${d.slack}s · 落后 ${d.lag}ms · 超紧急线 ${d.over}ms · 窗口最小余量 ${d.min}s · tick ${d.ticks} · 已跳 ${d.jumps}`
    }
  }, 1000)
  // 收听信息 5s 轮询:仅播放期间(未播放不必轮询)
  statusTimer = setInterval(() => {
    if (player && player.active) refreshStatus()
  }, 5000)

  document.addEventListener('visibilitychange', onVisibility)
  if (typeof ResizeObserver !== 'undefined' && stageEl.value) {
    resizeObs = new ResizeObserver(() => {
      measureFx()
      if (!isActive.value) drawFx(0.016)
    })
    resizeObs.observe(stageEl.value)
  }
  syncFx()
  refreshStatus()   // 页面打开时补一次快照(格式参数与人数),之后按需轮询
})

onActivated(() => {
  // 从别的模块切回来:恢复前台缓冲策略,重算画布并恢复律动(播放从未停止)
  if (player) player.enterForeground()
  setTimeout(syncFx, 60)
  refreshStatus()
})

onDeactivated(() => {
  // 切走不销毁页面:播放继续(连接与服务端设备保留不变,只是前端不再刷新特效)
  stopFx()
  if (player) player.enterBackground()
})

onBeforeUnmount(() => {
  document.removeEventListener('visibilitychange', onVisibility)
  if (delayTimer) clearInterval(delayTimer)
  if (statusTimer) clearInterval(statusTimer)
  if (hintTimer) clearTimeout(hintTimer)
  if (resizeObs) resizeObs.disconnect()
  stopFx()
  if (player) player.stop()
})

function onVolume(e) {
  volume.value = Number(e.target.value)
  if (player) player.setVolume(volume.value)
  try {
    localStorage.setItem(VOLUME_KEY, String(volume.value))
  } catch {
    /* 存储不可用:不记忆 */
  }
}
</script>

<template>
  <div>
    <div class="page-head sa-head">
      <div>
        <h1>服务器音频</h1>
        <div class="sub">收听服务器正在播放的声音</div>
      </div>
    </div>

    <!-- 能力检测未过:不显示开始按钮(判定用能力检测,不认浏览器名) -->
    <section v-if="!supported" class="card sa-unsupported">
      <span class="art">
        <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><rect x="3" y="4" width="18" height="16" rx="3"/><path d="M3 9h18"/><circle cx="12" cy="15" r="3.1"/><path d="M6.2 6.6h.01M9 6.6h.01"/></svg>
      </span>
      <b>不支持的浏览器</b>
      <p>当前浏览器不支持实时收听,请在安卓 Chrome(或同内核浏览器)中打开。</p>
      <span class="badge badge-dim">需支持 MediaSource 与 WebM/Opus</span>
    </section>

    <div v-else class="sa-shell">
      <!-- ============ 深色舞台:特效区 + 圆盘主按钮 + 状态行 ============ -->
      <section ref="stageEl" class="sa-stage">
        <canvas ref="canvasEl" class="sa-canvas" aria-hidden="true"></canvas>

        <div class="sa-stage-top">
          <span class="sa-chip" :class="cur.tone === 'ok' ? 'ok' : (cur.tone === 'warn' || cur.tone === 'err') ? 'warn' : 'idle'">
            <i class="dot"></i>{{ cur.chip }}
          </span>
          <span v-if="status && status.streamId" class="sa-chip" title="流标识:采集重启/服务重启后会变化,客户端据此丢弃旧缓冲">
            <span class="mono">流 {{ status.streamId }}</span>
          </span>
        </div>

        <div class="sa-disc-wrap">
          <button ref="discEl" type="button" class="sa-disc" :class="'is-' + cur.cls"
                  :aria-label="cur.label" @click="onDisc">
            <span class="sa-disc-ico" v-html="discIcon"></span>
          </button>
        </div>

        <div class="sa-status" :class="cur.tone ? 'is-' + cur.tone : ''" role="status" aria-live="polite" aria-atomic="true">
          <span class="sa-status-ico" v-html="statusIcon"></span>
          <span class="sa-status-text">{{ cur.text }}</span>
          <span v-if="showDetail" class="sa-status-extra">{{ showDetail }}</span>
        </div>
        <div class="sa-stage-hint">{{ hint }}</div>
      </section>

      <!-- ============ 信息卡:参数行 / 收听信息 / 音量 ============ -->
      <section class="card sa-info">
        <div class="sa-params">
          <span class="sa-pv">{{ fmt.sampleRate }}</span><span class="sep">·</span>
          <span class="sa-pv">{{ fmt.bitrate }}</span><span class="sep">·</span>
          <span class="sa-pv">{{ fmt.channels }}</span>
          <span class="sa-delay" :style="{ opacity: delayText === '—' ? 0.5 : 1 }">
            延迟 <b>{{ delayText }}</b> ms
          </span>
        </div>
        <div v-if="debugOn" class="sa-params-cap">{{ debugText }}</div>

        <div class="sa-divider"></div>

        <div class="sa-listen-head">
          <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 14v-2a8 8 0 0 1 16 0v2"/><rect x="2.6" y="13.4" width="4.2" height="6.4" rx="1.6"/><rect x="17.2" y="13.4" width="4.2" height="6.4" rx="1.6"/></svg>
          <span v-if="listenerCount">当前 <span class="cnt">{{ listenerCount }}</span> 个会话在收听</span>
          <span v-else>未在收听</span>
          <span class="hint">{{ listenHint }}</span>
        </div>
        <div v-if="listenerCount && listeners && listeners.length" class="sa-listen-list">
          <div v-for="(l, i) in listeners" :key="(l.uid || 0) + ':' + (l.client || i)" class="sa-lrow">
            <span class="dot" :class="{ stale: !l.paused && (l.lagMs || 0) > 3000 }"></span>
            <span class="who">
              <span class="nm">{{ l.nickname || l.account || ('用户 ' + l.uid) }}</span>
              <span v-if="ownClient && l.client === ownClient" class="you">本机</span>
            </span>
            <span class="cl">client {{ l.client }}</span>
            <span class="ip">{{ l.ip }}</span>
            <span class="lag" :class="{ warn: !l.paused && (l.lagMs || 0) > 3000 }">{{ l.paused ? '已暂停' : ((l.lagMs < 0) ? '未上报' : '落后 ' + fmtLag(l.lagMs)) }} · {{ fmtSince(l.sinceMs) }}</span>
          </div>
        </div>
        <div v-else-if="!listenerCount" class="sa-empty">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 8.4v4.2l3 1.8"/></svg>
          <span>无人在听时采集会自动停止并释放音频设备(按需采集)</span>
        </div>

        <div class="sa-vol">
          <span class="sa-vol-ico">
            <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11.5 5.2 7 8.8H3.6v6.4H7l4.5 3.6z"/><path d="M15.4 9.3a4 4 0 0 1 0 5.4"/><path d="M18.3 6.5a8 8 0 0 1 0 11"/></svg>
          </span>
          <input class="sa-range" type="range" min="0" max="100" :value="volume" aria-label="音量"
                 :style="{ '--v': volume + '%' }" @input="onVolume">
          <span class="sa-vol-val num">{{ volume }}</span>
        </div>
        <div class="sa-params-cap">页面音量是相对系统音量的软调节(默认 100)</div>
      </section>

      <section class="card sa-tips">
        <span class="ti">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
               stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <rect x="3.6" y="3.6" width="7" height="7" rx="1.8"/><rect x="13.4" y="3.6" width="7" height="7" rx="1.8"/>
            <rect x="3.6" y="13.4" width="7" height="7" rx="1.8"/><rect x="13.4" y="13.4" width="7" height="7" rx="1.8"/>
          </svg>
        </span>
        <span>切到别的模块可继续收听</span>
      </section>
    </div>

    <!-- 媒体元素:声音必须走它(MSE 喂分片);不接 WebAudio -->
    <audio ref="audioEl" preload="auto" style="display:none"></audio>
  </div>
</template>

<style scoped>
/* 页头与舞台同宽居中(壳层 portal.css 的 .page-head 只管排版,这里补宽度约束) */
.sa-head{max-width:720px;margin:0 auto var(--sp-5)}
.sa-head .sub{color:var(--color-text-2);margin-top:2px}
</style>