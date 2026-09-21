// 服务器音频播放器:WS 推流 → MediaSource 喂分片 → 交给媒体元素播放 → 后台保活
//
// 设计约束(设计 §5):
//   ① 声音必须交给 <audio> 元素(MSE),禁止 WebCodecs 自解码;
//   ② 禁止任何 WebAudio 用法(AudioContext / createMediaElementSource / AnalyserNode)——
//      声音一旦经由 WebAudio,页面被系统挂起时会被一起中断,后台播放直接失效;
//   ③ appendBuffer 串行、追加前确认 mediaSource.readyState === 'open';
//   ④ 收到多少追加多少(永不丢片:漏片会在时间轴上留空洞,播放头走到空洞处会永久停住);
//   ⑤ 延迟靠"播放头前跳"控制 —— 推流下落后量(直播边缘 − 播放头)不会自己收回,
//      它是稳态控制量而不是补救手段;内存靠周期性裁剪封顶;
//   ⑥ 任何一次(重)连接、暂停恢复、追不上时的重建,都从直播边缘起播:
//      断网期间与暂停期间的内容一律不补(设计 §3.4 / §5.2)。
//
// 本类不依赖 Vue,便于脱开界面调试。

import { openAudioWs, newClientId, goLogin } from '../../api/audio'

const MIME = 'audio/webm; codecs="opus"'

// ── 稳态与自愈参数 ──
const TICK_MS = 1000            // 稳态定时器:看门狗 / 前跳 / 状态收敛都在这里驱动
const LEAD_MIN_SEC = 0.2        // 缓冲余量起点(= 2 个分片)
                                // 这是读数的下限来源:媒体元素至少要这么多数据才肯持续出声。
                                // 真机上若出现起播不出声/频繁卡顿,把它调回 0.3(代价是读数 +100ms)
const LEAD_MAX_SEC = 2          // 余量上限(卡顿后自适应上调到此为止)
const CATCHUP_SLACK_SEC = 1.5   // 落后超过"余量 + 该值"即前跳追平(紧急:立即追)
const LEAD_SETTLE_MARGIN = 0.35 // 余量的窗口最小值高于目标多少才算"卡住"(秒)
const LEAD_SETTLE_MS = 3000     // "多余"须持续多久才回收(毫秒)
                                // 推流下"追加速度 = 播放速度":余量一旦被某次事件(起播落点 /
                                // 卡顿 / 投递成串)推高就不会自己落回。没有这条回收,落后就
                                // 永久停在高位 —— 表现为"延迟稳定在几百毫秒下不来"。
const LEAD_DECAY_MS = 45000     // 连续多久不卡,就把自适应余量减半收回来
const TRIM_EVERY_TICKS = 30     // 每 30 秒裁剪一次旧数据(内存与收听时长解耦)
const KEEP_BEHIND_SEC = 10      // 裁剪时保留播放头之后(已播过)的秒数
const FRAME_STALL_MS = 10000    // 距最近一帧超过该值:判定滞留,主动重连
const CONNECT_STALL_MS = 10000  // 建连后迟迟没有 meta:如实显示"未收到音频数据"
const PLAYHEAD_STALL_MS = 3000  // 播放头连续多久不前进判定为卡死
const PLAYHEAD_FIX_MAX = 3      // 本地跳跃自愈次数上限,超过则重建
const APPEND_RETRY_MS = 20      // 缓冲忙时的重试间隔(等待重试而不丢片)
const APPEND_RETRY_MAX_MS = 400 // 缓冲忙的等待上限(超过即判定追不上,重建)
const RECONNECT_MAX_MS = 2000   // 重连退避上限(断网恢复 ≤5s 的前提)
const JITTER_EWMA_ALPHA = 0.2   // 帧到达间隔抖动的指数平滑系数
const JITTER_LEAD_FACTOR = 2    // 抖动余量 = 抖动 × 该系数 + 安全垫(秒)
const JITTER_LEAD_MARGIN_SEC = 0.15

/// 能力检测:安全上下文 + MSE 支持 WebM/Opus + 支持 WebSocket(不认浏览器名)
export function isPlaybackSupported() {
  if (typeof window === 'undefined') return false
  if (typeof MediaSource === 'undefined' || typeof WebSocket === 'undefined') return false
  if (!MediaSource.isTypeSupported || !MediaSource.isTypeSupported(MIME)) return false
  if (window.isSecureContext === false) return false
  return true
}

export class AudioPlayer {
  /**
   * @param {Object} opt
   * @param {HTMLMediaElement} opt.audio 播放元素(<audio>,或退化的无声 <video playsinline>)
   * @param {(state:string, detail:string)=>void} opt.onState 状态回调
   *   (idle/connecting/playing/paused/reconnecting/no-device)
   * @param {()=>void} opt.onInfo 流参数/延迟等展示数据变化
   * @param {(kind:string, message:string)=>void} opt.onNotice 一次性提示
   *   (session/forbidden/noDevice/blocked/streamGone/segmentGone)
   */
  constructor({ audio, onState, onInfo, onNotice }) {
    this.audio = audio
    this.onState = onState || (() => {})
    this.onInfo = onInfo || (() => {})
    this.onNotice = onNotice || (() => {})

    this.state = 'idle'
    this.stateDetail = ''
    this.profile = 'fg'          // fg / bg(仅影响是否降频显示,推流下不改变传输)
    this.client = ''             // 本端标识(服务端按 uid:client 区分多端)
    this.streamId = ''           // 当前流标识(变化即换流,需重建媒体源)
    this.segmentMs = 100
    this.format = { sampleRate: 0, channels: 0, bitrate: 0 }
    this.listenerCount = 0

    this._ws = null              // 当前连接(传输层封装)
    this._batchQueue = []        // 待追加的分片批(串行消费,避免交错)
    this._batchBusy = false
    this._ms = null              // MediaSource
    this._sb = null              // SourceBuffer
    this._objUrl = ''
    this._stopped = true
    this._paused = false
    this._gen = 0                // 代际令牌:重连/重建后旧连接与旧异步流程一并作废
    this._chain = Promise.resolve()  // append/remove 串行链
    this._backoffMs = 1000
    this._reconnectTimer = null
    this._tickTimer = null
    this._trimTicks = 0
    this._positioned = false
    this._sourceDead = false
    this._playing = false        // 媒体元素是否真的出过声(playing 事件)
    this._stalled = false        // 建连后长时间拿不到可播数据
    this._playStalled = false    // 播放头卡住并被本地自愈中(与"连接中断"区分开)
    this._playAttempt = 0        // play() 尝试代次:忽略上一代被 load 中止的 AbortError
    this._lastCt = -1            // 看门狗:上次观察到的播放位置
    this._lastCtAt = 0
    this._stallFixes = 0
    this._leadSec = LEAD_MIN_SEC // 自适应缓冲余量(卡顿后翻倍,上限 LEAD_MAX_SEC)
    this._lastStallAt = 0
    this._cursor = 0             // 已追加到的最新片号
    this._liveSeq = 0            // 已收到的最新片号(延迟口径用前者会失真,见 delayMs)
    this._connectAt = 0
    this._lastFrameAt = 0        // 最近一帧(meta/批)到达时刻:滞留哨兵依据
    this._jitterEwma = 0         // 帧到达间隔抖动(毫秒;驱动动态余量)
    this._lastFrameGapAt = 0
    /// 诊断计数(仅 ?debug=1 时看,不参与任何行为)
    this._diag = { ticks: 0, jumps: 0, over: 0 }
    this._settleSince = 0        // '多余余量'开始出现的时刻(持续够久才回收)
    this._leadWinAt = 0          // 余量窗口起点
    this._leadMin = 0            // 窗口内最小余量(避开 100ms 锯齿)
    this._onPlayingEv = () => this._handlePlaying()
    this._onSourceEnd = () => { this._sourceDead = true }
    this._onOnline = () => {
      // 网络恢复立即重试:否则退避睡眠会把"网络已回来"的时间白白吃掉
      if (this._stopped || this._paused) return
      if (this._reconnectTimer) {
        clearTimeout(this._reconnectTimer)
        this._reconnectTimer = null
      }
      this._reconnect()
    }
  }

  /* ================= 对外接口 ================= */

  /// 开始收听(必须在用户手势内首次调用:起播需要用户激活)
  start() {
    if (!this._stopped) return
    this._stopped = false
    this._paused = false
    this._backoffMs = 1000
    this.client = newClientId()
    this.streamId = ''
    this._cursor = 0
    this._liveSeq = 0
    this._liveSeq = 0
    this._lastFrameAt = 0
    this._jitterEwma = 0
    this._lastFrameGapAt = 0
    this._leadSec = LEAD_MIN_SEC
    this._stalled = false
    this._trimTicks = 0
    this._setState('connecting')
    this._armElement()            // 手势内同步完成:建 MediaSource、挂 src、play()
    this._startTimers()
    if (typeof window !== 'undefined') window.addEventListener('online', this._onOnline)
    this._connect()
  }

  /// 停止收听:关闭连接(服务端以连接存活判定离场)+ 释放媒体元素
  stop() {
    if (this._stopped) return
    this._stopped = true
    this._gen += 1
    this._stopTimers()
    this._closeWs()
    this._releaseElement()
    this.streamId = ''
    this._setState('idle')
  }

  /// 暂停:保留连接与服务端设备,但不再接收推送(仍应答心跳)
  pause() {
    if (this._stopped || this._paused) return
    this._paused = true
    try {
      this.audio.pause()
    } catch {
      /* 元素可能尚未就绪 */
    }
    this._setMediaSessionState('paused')
    if (this._ws) this._ws.send({ type: 'pause' })
    this._setState('paused')
  }

  /// 恢复播放:重建媒体源并从直播边缘续接(不听暂停期间的内容)
  resume() {
    if (this._stopped || !this._paused) return
    this._paused = false
    if (this._ws && !this._ws.closed) {
      // 连接还在:清掉旧缓冲,发 sync 让服务端从直播边缘重定基
      this._releaseSource()
      this._armElement()
      this.streamId = ''
      this._cursor = 0
      this._liveSeq = 0
      this._ws.send({ type: 'sync', client: this.client })
      this._syncLiveState()
      return
    }
    // 连接已断(暂停超过服务端保持上限):走一次完整重连
    this._reconnect()
  }

  /// 音量(0~100):作用于媒体元素音量,是相对系统音量的软调节
  setVolume(v) {
    const vol = Math.max(0, Math.min(100, Number(v) || 0)) / 100
    try {
      this.audio.volume = vol
    } catch {
      /* 忽略 */
    }
  }

  /// 切到后台/锁屏(推流下缓冲深度由落后量决定,这里只记录档位供展示)
  enterBackground() {
    this.profile = 'bg'
  }

  /// 回到前台
  enterForeground() {
    this.profile = 'fg'
  }

  /// 延迟(ms) = 已收到的最新片结束时刻 − 已播位置(两者同一时间基,均以采集起点为 0)
  /// 尚未真正起播(currentTime 仍为 0)时返回 null,避免界面显示一个假延迟
  get delayMs() {
    if (!this._liveSeq) return null
    if (!(Number(this.audio.currentTime) > 0)) return null
    const edgeMs = this._liveSeq * this.segmentMs
    const playedMs = Number(this.audio.currentTime || 0) * 1000
    const d = edgeMs - playedMs
    return d >= 0 ? Math.round(d) : 0
  }

  /// 是否处于活动态(连接中/播放中/暂停/重连)
  get active() {
    return !this._stopped
  }

  /// 诊断读数:把"延迟由哪一项决定"摊开(界面带 ?debug=1 时显示)
  /// target = max(reserve, jitter, segLead):谁最大就是谁在决定延迟
  get debugInfo() {
    const a = this.audio
    const n = a.buffered ? a.buffered.length : 0
    const lead = n ? Math.max(0, a.buffered.end(n - 1) - a.currentTime) : 0
    return {
      reserve: Number(this._leadSec.toFixed(2)),
      jitter: Math.round(this._jitterEwma),
      target: Number(this._baseTargetSec().toFixed(2)),
      lead: Number(lead.toFixed(2)),
      slack: CATCHUP_SLACK_SEC,
      lag: this.delayMs,
      ticks: this._diag.ticks,  // 稳态定时器跑了多少拍(不涨 = 定时器没在跑)
      jumps: this._diag.jumps,  // 前跳实际执行了几次
      over: this._diag.over,     // 落后超出紧急触发线多少毫秒(负数 = 还不到线)
      min: this._diag.min        // 2 秒窗口内的最小余量(判定回收用的是它)
    }
  }

  /// 播放头对应的片号(设计口径:第 N 片覆盖 [(N−1)×segmentMs, N×segmentMs))
  /// 心跳应答把它带给服务端,/status 才能显示"这位听众听到的位置比实况晚多少";
  /// 未起播返回 0(服务端显示"未上报",不显示成一个假的大数)
  _posSeq() {
    const ct = Number(this.audio.currentTime) || 0
    if (!(ct > 0)) return 0
    return Math.floor((ct * 1000) / this.segmentMs) + 1
  }

  /// 收听人数变化时刷新 MediaSession 副题(锁屏可见)
  setListenerCount(n) {
    this.listenerCount = Number(n) || 0
    this._setupMediaSession()
  }

  /// 回到前台/恢复播放时主动检查(不等定时器下个周期):停滞过久立即重建
  checkStale() {
    if (this._stopped || this._paused) return
    if (!this._lastFrameAt) return
    if (Date.now() - this._lastFrameAt > FRAME_STALL_MS) this._reconnect()
  }

  /* ================= 内部:媒体元素与 MediaSource ================= */

  _armElement() {
    // 手势内同步完成:建 MediaSource、挂 src、调用 play()。
    // play() 之后进入 await 会丢失用户激活,所以这里不等数据:元素进入"等待播放"态,
    // 数据到达即出声。
    this._releaseSource()
    const ms = new MediaSource()
    this._ms = ms
    ms.addEventListener('sourceclose', this._onSourceEnd)
    ms.addEventListener('sourceended', this._onSourceEnd)
    this._objUrl = URL.createObjectURL(ms)
    this.audio.src = this._objUrl
    this.audio.addEventListener('playing', this._onPlayingEv)
    this._sourceDead = false
    this._positioned = false
    this._ensurePlaying()
    this._setupMediaSession()
  }

  _ensurePlaying() {
    if (this._paused || this._stopped) return
    if (!this.audio.paused) return
    const attempt = this._playAttempt
    const p = this.audio.play()
    if (p && p.catch) {
      p.catch((err) => {
        // 只有"确实被浏览器拦下"才提示并置暂停:src 更换(重建)会让上一代 play() 的
        // pending promise 以 AbortError 结束,这类残留拒绝必须忽略
        if (this._stopped || attempt !== this._playAttempt) return
        if (!err || err.name !== 'NotAllowedError') return
        this._paused = true
        if (this._ws) this._ws.send({ type: 'pause' })
        this.onNotice('blocked', err.message || '')
        this._setState('paused')
      })
    }
  }

  _handlePlaying() {
    if (this._stopped) return
    this._paused = false
    this._playing = true
    this._stalled = false
    this._playStalled = false
    this._stallFixes = 0        // 真正出过声:清空看门狗的自愈计数
    this._lastCt = -1
    this._setMediaSessionState('playing')
    this._setState('playing')
    this.onInfo()
  }

  _setupMediaSession() {
    if (typeof navigator === 'undefined' || !('mediaSession' in navigator)) return
    const ms = navigator.mediaSession
    try {
      ms.metadata = new MediaMetadata({
        title: '服务器音频',
        artist: this.listenerCount > 0 ? `来自 ZiMo 服务器 · ${this.listenerCount} 个会话在收听` : '来自 ZiMo 服务器'
      })
    } catch {
      /* 元数据不可用时不影响播放 */
    }
    const set = (action, fn) => {
      try {
        ms.setActionHandler(action, fn)
      } catch {
        /* 个别动作不支持 */
      }
    }
    set('play', () => this.resume())
    set('pause', () => this.pause())
    set('stop', () => this.stop())
    // 不设 setPositionState:直播无终点,界面也没有进度条
  }

  _setMediaSessionState(s) {
    if (typeof navigator === 'undefined' || !('mediaSession' in navigator)) return
    try {
      navigator.mediaSession.playbackState = s
    } catch {
      /* 忽略 */
    }
  }

  _clearMediaSession() {
    if (typeof navigator === 'undefined' || !('mediaSession' in navigator)) return
    const ms = navigator.mediaSession
    try {
      ms.metadata = null
      ms.playbackState = 'none'
    } catch {
      /* 忽略 */
    }
    for (const a of ['play', 'pause', 'stop']) {
      try {
        ms.setActionHandler(a, null)
      } catch {
        /* 忽略 */
      }
    }
  }

  _releaseSource() {
    const a = this.audio
    this._playAttempt += 1     // 使上一代 play() 的残留拒绝失效
    this._playing = false
    this._stalled = false
    this._playStalled = false
    this._batchQueue.length = 0   // 旧代的待追加批随媒体源一起作废
    try {
      if (this._ms && this._ms.readyState === 'open') this._ms.endOfStream()
    } catch {
      /* 已结束/已关闭 */
    }
    try {
      a.pause()
    } catch {
      /* 忽略 */
    }
    try {
      a.removeAttribute('src')
    } catch {
      /* 忽略 */
    }
    if (this._objUrl) {
      try {
        URL.revokeObjectURL(this._objUrl)
      } catch {
        /* 忽略 */
      }
      this._objUrl = ''
    }
    try {
      a.load()
    } catch {
      /* 忽略 */
    }
    if (this._ms) {
      this._ms.removeEventListener('sourceclose', this._onSourceEnd)
      this._ms.removeEventListener('sourceended', this._onSourceEnd)
    }
    this._sb = null
    this._ms = null
    this._positioned = false
  }

  _releaseElement() {
    this.audio.removeEventListener('playing', this._onPlayingEv)
    if (typeof window !== 'undefined') window.removeEventListener('online', this._onOnline)
    this._releaseSource()
    this._clearMediaSession()
  }

  /* ================= 内部:串行缓冲操作 ================= */

  _chainTask(fn) {
    const t = this._chain.then(fn)
    this._chain = t.catch(() => {})
    return t
  }

  _append(chunk) {
    return this._chainTask(() => this._appendOne(chunk))
  }

  _appendOne(chunk) {
    // 缓冲忙时**等待重试**,不能丢片:漏掉一片会在时间轴上留一个空洞,
    // 播放头走到空洞处就永久停住(媒体元素不会自己跳过空洞),表现为
    // "状态显示正在播放、但一直没声音"。手机上写入更慢,更容易撞上。
    return new Promise((resolve, reject) => {
      const { _sb: sb, _ms: ms } = this
      let waited = 0
      const tryAppend = () => {
        if (this._stopped) return reject(new Error('已停止'))
        if (!sb || !ms || ms.readyState !== 'open') return reject(new Error('流已结束'))
        if (sb.updating) {
          waited += APPEND_RETRY_MS
          if (waited > APPEND_RETRY_MAX_MS) return reject(new Error('缓冲持续繁忙'))
          setTimeout(tryAppend, APPEND_RETRY_MS)
          return
        }
        const done = () => {
          sb.removeEventListener('updateend', done)
          sb.removeEventListener('error', onErr)
          resolve()
        }
        const onErr = () => {
          sb.removeEventListener('updateend', done)
          sb.removeEventListener('error', onErr)
          reject(new Error('分片写入失败'))
        }
        sb.addEventListener('updateend', done)
        sb.addEventListener('error', onErr)
        try {
          sb.appendBuffer(chunk)
        } catch {
          onErr()
        }
      }
      tryAppend()
    })
  }

  _remove({ start, end }) {
    return this._chainTask(
      () =>
        new Promise((resolve, reject) => {
          const { _sb: sb, _ms: ms } = this
          if (!sb || !ms || ms.readyState !== 'open') return reject(new Error('流已结束'))
          if (sb.updating) return reject(new Error('缓冲正忙'))
          const cleanup = () => {
            sb.removeEventListener('updateend', done)
            sb.removeEventListener('error', onErr)
          }
          const done = () => {
            cleanup()
            resolve()
          }
          const onErr = () => {
            cleanup()
            reject(new Error('缓冲裁剪失败'))
          }
          sb.addEventListener('updateend', done)
          sb.addEventListener('error', onErr)
          try {
            sb.remove(start, end)
          } catch {
            onErr()
          }
        })
    )
  }

  /// 建 SourceBuffer(必须等 sourceopen)
  _ensureSourceBuffer() {
    return new Promise((resolve, reject) => {
      const ms = this._ms
      if (!ms) return reject(new Error('媒体源未建立'))
      const attach = () => {
        try {
          if (!this._sb) this._sb = ms.addSourceBuffer(MIME)
          resolve(this._sb)
        } catch (e) {
          reject(e)
        }
      }
      if (ms.readyState === 'open') return attach()
      const onOpen = () => {
        ms.removeEventListener('sourceclose', onClose)
        attach()
      }
      const onClose = () => {
        ms.removeEventListener('sourceopen', onOpen)
        this._sourceDead = true
        reject(new Error('媒体源已关闭'))
      }
      ms.addEventListener('sourceopen', onOpen, { once: true })
      ms.addEventListener('sourceclose', onClose, { once: true })
    })
  }

  /* ================= 内部:连接与收发 ================= */

  /// 建立连接(所有回调都带代际令牌:重连后旧连接的在途帧一律作废)
  _connect() {
    this._closeWs()
    const gen = ++this._gen
    this._connectAt = Date.now()
    this._ws = openAudioWs({
      client: this.client,
      onMeta: (m) => this._onMeta(gen, m),
      onInit: (seq, p) => this._onInit(gen, seq, p),
      onBatch: (seq, segs) => this._onBatch(gen, seq, segs),
      onError: (e) => this._onErrorFrame(gen, e),
      onClose: () => this._onWsClose(gen),
      // 上报播放头:服务端 /status 的"落后 Ns"要用它算(未起播时给 0)
      getPosSeq: () => this._posSeq()
    })
  }

  _closeWs() {
    const ws = this._ws
    this._ws = null
    if (ws) ws.close()
  }

  _onMeta(gen, m) {
    if (this._stopped || gen !== this._gen) return
    this._lastFrameAt = Date.now()
    if (m.streamId && m.streamId !== this.streamId) {
      // 换流(或首次):重建媒体源,丢弃旧缓冲
      if (this.streamId) {
        this._releaseSource()
        this._armElement()
        this.onNotice('streamGone', '')
      }
      this.streamId = m.streamId
    }
    this.segmentMs = Number(m.segmentMs) || 100
    this.format = { sampleRate: m.sampleRate, channels: m.channels, bitrate: m.bitrate }
    const base = Number(m.baseSeq) || 1
    this._cursor = base - 1
    this._liveSeq = Math.max(this._liveSeq, base - 1)
    this._positioned = false
    this._connectAt = Date.now()
    this._backoffMs = 1000
    this.onInfo()
  }

  _onInit(gen, seq, payload) {
    if (this._stopped || gen !== this._gen) return
    this._lastFrameAt = Date.now()
    this._ensureSourceBuffer()
      .then((sb) => {
        if (!sb || this._stopped || gen !== this._gen) return null
        return this._append(payload)
      })
      .catch(() => this._onAppendFail(gen))
  }

  _onBatch(gen, seq, segs) {
    if (this._stopped || gen !== this._gen || !segs.length) return
    this._lastFrameAt = Date.now()

    // 延迟口径按"已收到"算:收到即更新,不等追加完成(积压时读数才不会被低估)
    this._liveSeq = Math.max(this._liveSeq, seq + segs.length - 1)

    // 帧到达间隔的抖动:大抖动说明链路不稳,动态余量随之加厚
    const now = Date.now()
    if (this._lastFrameGapAt) {
      const gap = Math.abs(now - this._lastFrameGapAt - this.segmentMs)
      this._jitterEwma = this._jitterEwma
        ? this._jitterEwma * (1 - JITTER_EWMA_ALPHA) + gap * JITTER_EWMA_ALPHA
        : gap
    }
    this._lastFrameGapAt = now

    // 批处理走自己的串行队列:两批同时到达时不会交错(游标不会倒退、追加顺序不变)。
    // 不能直接排进 _chain —— _appendBatch 内部 await 的 _append 也排在 _chain 上,会自锁。
    // 队列长度由服务端发送窗口决定(心跳超时 120 秒 ≈ 1200 片 ≈ 1MB),不需要额外上限。
    this._batchQueue.push({ gen, seq, segs })
    this._drainBatches()
  }

  /// 串行消费分片批(重入时直接返回,由正在跑的那个循环继续消费)
  async _drainBatches() {
    if (this._batchBusy) return
    this._batchBusy = true
    try {
      while (this._batchQueue.length) {
        const { gen, seq, segs } = this._batchQueue.shift()
        if (this._stopped || gen !== this._gen) continue
        await this._appendBatch(gen, seq, segs)
      }
    } finally {
      this._batchBusy = false
    }
  }

  /// 逐片追加(任何一片失败都不再继续,交由 _onAppendFail 重建)
  async _appendBatch(gen, seq, segs) {
    for (let i = 0; i < segs.length; i++) {
      if (this._stopped || gen !== this._gen) return
      try {
        await this._append(segs[i])
      } catch {
        this._onAppendFail(gen)
        return
      }
      this._cursor = seq + i
    }
    if (!this._positioned) this._positionPlayback()
    else this._ensurePlaying()
  }

  _onAppendFail(gen) {
    if (this._stopped || gen !== this._gen) return
    // 追加失败不能"跳过这一片继续":那会在时间轴上留空洞。整体重建,从边缘接上。
    this._reconnect()
  }

  _onErrorFrame(gen, e) {
    if (this._stopped || gen !== this._gen) return
    const code = (e && e.code) || ''
    const msg = (e && e.message) || ''
    if (code === 'AUTH_INVALID' || code === 'ACCOUNT_DISABLED' || code === 'FORCE_CHANGE_REQUIRED') {
      this.onNotice('session', msg)
      goLogin()
      this._abort('idle')
      return
    }
    if (code === 'PERM_DENIED') {
      this.onNotice('forbidden', msg)
      this._abort('idle')
      return
    }
    if (code === 'AUDIO_NO_DEVICE') {
      this.onNotice('noDevice', msg)
      this._abort('no-device')
      return
    }
    if (code === 'AUDIO_BUSY') {
      // 暂时不可用(设备正在释放/采集刚起):退避后重试,不要报成"没有设备"
      this._scheduleReconnect()
      return
    }
    if (code === 'AUDIO_DEVICE_LOST') {
      this.onNotice('noDevice', msg)
      this._abort('no-device')
      return
    }
    this._reconnect()
  }

  _onWsClose(gen) {
    if (this._stopped || gen !== this._gen) return
    // 暂停态不自动重连:服务端暂停保持上限到点会主动断开,等用户点继续时再连
    if (this._paused) return
    this._scheduleReconnect()
  }

  /// 退避重连(上限 2 秒;online 事件会立即打断退避)
  _scheduleReconnect() {
    if (this._stopped || this._paused || this._reconnectTimer) return
    this._setState('reconnecting', this._stalled ? '未收到音频数据' : '连接中断,正在重新连接')
    const wait = Math.min(this._backoffMs, RECONNECT_MAX_MS)
    this._backoffMs = Math.min(this._backoffMs * 2, RECONNECT_MAX_MS)
    this._reconnectTimer = setTimeout(() => {
      this._reconnectTimer = null
      this._reconnect()
    }, wait)
  }

  /// 重建播放链路:断开旧连接 → 重建媒体源 → 重新建连(服务端从直播边缘起播)
  _reconnect() {
    if (this._stopped || this._paused) return
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer)
      this._reconnectTimer = null
    }
    this._stallFixes = 0
    this._setState('reconnecting', '正在重新连接')
    this._sourceDead = true
    this.streamId = ''
    this._cursor = 0
    this._liveSeq = 0
    this._lastFrameGapAt = 0
    this._releaseSource()
    this._armElement()
    this._connect()
  }

  /* ================= 内部:稳态控制(每秒一次) ================= */

  _startTimers() {
    if (this._tickTimer) return
    this._tickTimer = setInterval(() => this._tick(), TICK_MS)
  }

  _stopTimers() {
    if (this._tickTimer) {
      clearInterval(this._tickTimer)
      this._tickTimer = null
    }
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer)
      this._reconnectTimer = null
    }
  }

  /// 稳态定时器:看门狗 / 前跳 / 状态收敛 / 周期裁剪 / 收帧哨兵
  _tick() {
    if (this._stopped) return
    this._diag.ticks += 1
    if (this._paused) return
    if (!this._playing && this._connectAt && Date.now() - this._connectAt > CONNECT_STALL_MS) {
      this._stalled = true      // 建连成功但长时间没有可播数据:如实显示原因
    }
    this._watchPlayhead()
    this._catchUp()
    this._syncLiveState()
    if (this._lastFrameAt && Date.now() - this._lastFrameAt > FRAME_STALL_MS) {
      this._reconnect()
      return
    }
    if (++this._trimTicks >= TRIM_EVERY_TICKS) {
      this._trimTicks = 0
      this._trimBehind()
    }
  }

  /// 落后前跳:控制量取 buffered 的实测值(ground truth),落点必在已缓冲区间内
  ///
  /// 推流下"追加的推进速度 = 播放头推进速度",落后量一旦变大就不会自己收回;这里
  /// 主动把播放头推到"缓冲末尾 − 余量",把延迟稳定在一个已知值上(跳过的音频不补听)。
  _catchUp() {
    const a = this.audio
    if (this._stopped || this._paused) return
    const n = a.buffered ? a.buffered.length : 0
    if (!n) return
    const start = a.buffered.start(n - 1)
    const end = a.buffered.end(n - 1)
    const target = this._baseTargetSec()
    const lead = end - a.currentTime
    // 诊断:落后超出"紧急触发线"多少毫秒(负数 = 没到线)
    this._diag.over = Math.round((lead - (target + CATCHUP_SLACK_SEC)) * 1000)

    // 片是每 100ms 到一次的,所以瞬时余量必然锯齿(约 0.1s 幅度)。判定"余量偏高"要看
    // 窗口内的最小值,否则每次都撞在锯齿低点、永远攒不满持续时间。
    const now = Date.now()
    if (!this._leadWinAt || now - this._leadWinAt > 2000) {
      this._leadWinAt = now
      this._leadMin = lead
    } else {
      this._leadMin = Math.min(this._leadMin, lead)
    }
    this._diag.min = Number(this._leadMin.toFixed(2))

    let urgent = false
    if (lead > target + CATCHUP_SLACK_SEC) {
      urgent = true   // 落后过多(抖动 / 卡顿之后):立即追平(瞬时值即可)
    } else if (this._leadMin > target + LEAD_SETTLE_MARGIN) {
      // 只是"多余的余量"(量的是窗口最小值):持续够久才回收一次
      if (!this._settleSince) this._settleSince = now
      if (now - this._settleSince < LEAD_SETTLE_MS) return
    } else {
      this._settleSince = 0
      return
    }

    const want = Math.max(start, end - target)
    if (want <= a.currentTime + 0.05) return
    try {
      a.currentTime = want
    } catch {
      return
    }
    this._diag.jumps += 1
    this._settleSince = 0
    this._lastCt = -1
    if (urgent)
      this.onNotice('segmentGone', '')   // 只有紧急追平才提示;例行回收不打扰
  }

  /// 播放头停滞看门狗:元素"在播"但时间不前进时自愈
  ///
  /// 空洞、系统挂起、缓冲区异常都会造成"状态显示正在播放、其实没有声音"这种假死;
  /// 第一次只重新推起播放(原地续播,不产生跳音),连续第二次才跳到缓冲尾部,多次无效则重建。
  _watchPlayhead() {
    const a = this.audio
    if (this._stopped || this._paused || !this._playing) {
      this._lastCt = -1
      return
    }

    // 余量收回:一段时间没再卡,就把自适应加上去的余量减半收回来,
    // 否则卡过一次之后延迟会永久停在高位
    if (this._leadSec > LEAD_MIN_SEC && Date.now() - (this._lastStallAt || 0) > LEAD_DECAY_MS) {
      this._leadSec = Math.max(this._leadSec / 2, LEAD_MIN_SEC)
      this._lastStallAt = Date.now()
    }

    const ct = a.currentTime
    if (this._lastCt < 0 || Math.abs(ct - this._lastCt) > 0.02) {
      this._lastCt = ct
      this._lastCtAt = Date.now()
      return
    }
    if (Date.now() - (this._lastCtAt || 0) < PLAYHEAD_STALL_MS) return

    this._lastCt = -1
    this._lastCtAt = Date.now()
    this._lastStallAt = Date.now()
    this._stallFixes += 1
    const hasBuf = a.buffered && a.buffered.length
    const end = hasBuf ? a.buffered.end(a.buffered.length - 1) : 0
    if (this._stallFixes <= PLAYHEAD_FIX_MAX && end > 0) {
      // 卡过一次说明当前余量不够:翻倍(封顶),别在同一个坑里反复卡
      this._leadSec = Math.min(this._leadSec * 2, LEAD_MAX_SEC)
      if (this._stallFixes > 1) {
        try {
          a.currentTime = Math.max(a.buffered.start(0), end - this._baseTargetSec())
        } catch {
          /* 定位失败则交由下面的重新推起处理 */
        }
      }
      this._playing = false        // 交回给状态判定,别继续谎报"正在播放"
      this._playStalled = true     // 这是"播放卡顿"而不是"连接中断":状态文案分开
      this._ensurePlaying()
      this._syncLiveState()
      return
    }
    this._stallFixes = 0
    this._reconnect()
  }

  /// 周期裁剪:移除"播放头 − KEEP_BEHIND_SEC"之前的旧数据(内存与收听时长解耦)
  async _trimBehind() {
    const a = this.audio
    const sb = this._sb
    if (!sb || !a.buffered || !a.buffered.length) return
    const cutEnd = a.currentTime - KEEP_BEHIND_SEC
    if (cutEnd <= a.buffered.start(0) + 0.01) return
    try {
      await this._remove({ start: a.buffered.start(0), end: cutEnd })
    } catch {
      /* 与追加交错:下次再裁 */
    }
  }

  /// 起播定位:从直播边缘起播时缓冲起点不是 0,必须用 buffered.start(0) 定位
  _positionPlayback() {
    const a = this.audio
    if (!a.buffered || !a.buffered.length || !this._sb) return
    this._positioned = true
    const start = a.buffered.start(0)
    const end = a.buffered.end(a.buffered.length - 1)
    const needSec = Math.max((this.segmentMs / 1000) * 2 * 0.9, 0.12)
    if (end - start < needSec) {
      this._positioned = false    // 攒够 2~3 片再起播(服务端会持续推,很快够)
      return
    }
    // 落点直接定在"边缘 − 目标余量":起播余量就等于目标余量,不必等一次前跳才收下来
    // (服务端补了 3 片,这里只留 2 片,省下的 100ms 就是读数的下降量)
    const want = Math.max(start, end - this._baseTargetSec())
    try {
      a.currentTime = want
    } catch {
      /* 忽略 */
    }
    this._ensurePlaying()
  }

  /// 缓冲余量目标(秒):自适应余量 + 帧间隔抖动折算的链路裕量 + 起播所需片数
  _baseTargetSec() {
    const segLead = (this.segmentMs / 1000) * 2
    const jitterLead = this._jitterEwma
      ? (this._jitterEwma / 1000) * JITTER_LEAD_FACTOR + JITTER_LEAD_MARGIN_SEC
      : 0
    return Math.max(this._leadSec, jitterLead, segLead)
  }

  /// 按"是否真的在播"收敛状态:播放中 / 播放卡顿自愈中 / 未收到数据(重连中) / 连接中
  _syncLiveState() {
    if (this._stopped || this._paused) return
    if (this._playing) return this._setState('playing')
    if (this._playStalled) return this._setState('stalled', '播放卡顿,正在恢复')
    this._setState(this._stalled ? 'reconnecting' : 'connecting',
                   this._stalled ? '未收到音频数据' : '等待首片就绪')
  }

  /* ================= 内部:错误收敛 ================= */

  /// 内部收敛:释放元素并落到指定状态(不再重连)
  _abort(state) {
    this._stopped = true
    this._gen += 1
    this._stopTimers()
    this._closeWs()
    this._releaseElement()
    this.streamId = ''
    this._setState(state)
  }

  /* ================= 内部:小工具 ================= */

  _setState(key, detail) {
    const d = detail || ''
    if (this.state === key && this.stateDetail === d) return
    this.state = key
    this.stateDetail = d
    this.onState(key, d)
  }
}
