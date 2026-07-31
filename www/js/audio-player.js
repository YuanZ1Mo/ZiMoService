const { createApp } = Vue;

/** 音频流端点(与 filehub.js 的 REST_URL 同源约定) */
const STREAM_URL = `//${window.location.hostname}:39441/zimo/api/audio/stream`;

/* ===== 临时调试探针(排查"连接成功但无声音")===== */
const DBG = {
  bytes: 0, frames: 0, decoded: 0, scheduled: 0, errors: [], state: 'init',
  log(msg) {
    const t = new Date().toISOString().slice(11, 23);
    this.errors.push(`${t} ${msg}`);
    if (this.errors.length > 30) this.errors.shift();
    console.log('[audio-dbg]', msg);
  },
};
const dbgEl = document.createElement('div');
dbgEl.style.cssText = 'position:fixed;left:0;right:0;bottom:0;background:#111;color:#0f0;' +
  'font:11px monospace;padding:6px;z-index:9999;max-height:40vh;overflow:auto;white-space:pre-wrap;';
document.body.appendChild(dbgEl);
setInterval(() => {
  dbgEl.textContent = `bytes=${DBG.bytes} frames=${DBG.frames} decoded=${DBG.decoded} ` +
    `scheduled=${DBG.scheduled} ctx=${DBG.state}\n` + DBG.errors.join('\n');
}, 500);

/** 帧协议:len(4B 小端) + seq(4B 小端) + Opus 帧 */
function readLE32(u8, off) {
  return u8[off] | (u8[off+1] << 8) | (u8[off+2] << 16) | (u8[off+3] << 24);
}
function concatBytes(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a); out.set(b, a.length);
  return out;
}

createApp({
  data() {
    return {
      playing: false,
      statusText: '未连接',
      retryDelay: 1000,
      fatalError: '',
    };
  },
  created() {
    if (!window.isSecureContext) {
      this.fatalError = '需要 HTTPS 安全上下文,请使用 https:// 访问(WebCodecs 要求)';
      this.statusText = this.fatalError;
    } else if (typeof AudioDecoder === 'undefined') {
      this.fatalError = '当前浏览器不支持 WebCodecs(AudioDecoder)';
      this.statusText = this.fatalError;
    }
  },
  methods: {
    toggle() {
      if (!this.fatalError) { this.playing ? this.stop() : this.start(); }
    },
    async start() {
      this.playing = true;
      this.statusText = '连接中...';
      this.retryDelay = 1000;
      // 用户手势内创建 AudioContext,规避自动播放策略
      if (!this._ctx) {
        this._ctx = new AudioContext({ sampleRate: 48000 });
        // 音频上下文恢复(如后台挂起后 resume)→ 同样丢弃积压保持实时
        this._ctx.onstatechange = () => {
          if (this._ctx.state === 'running' && this.playing) {
            DBG.log('ctx statechange → 实时重同步');
            this.resync();
          }
        };
      }
      DBG.log('AudioContext state=' + this._ctx.state + ' rate=' + this._ctx.sampleRate);
      if (this._ctx.state === 'suspended') this._ctx.resume().catch(() => {});
      await this.runStream();
    },
    stop() {
      this.playing = false;
      if (this._abort) this._abort.abort();
      this.cleanup();
      this.statusText = '已停止';
    },
    cleanup() {
      if (this._decoder) { try { this._decoder.close(); } catch (e) {} this._decoder = null; }
      this._playQueue = [];
      this._lastSeq = 0;
      this._nextPlayTime = 0;
      this._resync = false;
    },
    /** 主循环:连接 → 读流 → 断线指数退避重连 */
    async runStream() {
      while (this.playing && !this.fatalError) {
        const abort = new AbortController();
        this._abort = abort;
        try {
          const resp = await fetch(STREAM_URL, { signal: abort.signal });
          DBG.log('fetch ok=' + resp.ok + ' status=' + resp.status);
          if (!resp.ok || !resp.body) {
            const txt = await resp.text().catch(() => '');
            // 503 等错误响应:提取服务端 JSON 错误信息展示
            let msg = `服务器返回 ${resp.status}`;
            try {
              const j = JSON.parse(txt);
              if (j.error && j.error.message) msg += ' ' + j.error.message;
            } catch (e) { if (txt) msg += ' ' + txt; }
            this.statusText = msg;
            throw new Error(msg);
          }
          this.statusText = '连接成功,等待音频...';
          await this.pump(resp.body.getReader());
        } catch (err) {
          if (!this.playing) return;                 // 主动停止
          if (err.name === 'AbortError') return;
          if (!this.statusText.startsWith('服务器返回')) {
            this.statusText = '连接断开,重连中...';
          }
          this.cleanup();
          await new Promise(r => setTimeout(r, this.retryDelay));
          this.retryDelay = Math.min(this.retryDelay * 2, 8000);
        }
      }
    },
    /** 实时重同步:丢弃积压 + 重置解码器 + 时间线归零(后台恢复/上下文恢复时调用) */
    resync() {
      this._playQueue = [];
      this._nextPlayTime = 0;
      if (this._decoder) {
        try { this._decoder.reset(); } catch (e) {}
        try {
          this._decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
        } catch (e) {}
      }
    },
    /** 读流拆帧 */
    async pump(reader) {
      const decoder = new AudioDecoder({
        output: (audioData) => this.onDecoded(audioData),
        error: (e) => { DBG.log('decoder ERROR: ' + (e && e.message ? e.message : e)); this._resync = true; },
      });
      try {
        decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
        DBG.log('decoder configured');
      } catch (e) {
        DBG.log('configure THROW: ' + e.message);
        throw e;
      }
      this._decoder = decoder;
      this._resync = false;

      let buf = new Uint8Array(0);
      while (this.playing) {
        const { done, value } = await reader.read();
        if (done) break;
        if (value && value.length) DBG.bytes += value.length;
        buf = buf.length ? concatBytes(buf, value) : value;
        while (buf.length >= 8) {
          const len = readLE32(buf, 0);
          const seq = readLE32(buf, 4);
          if (buf.length < 8 + len) break;
          DBG.frames++;
          this.onFrame(decoder, seq, buf.subarray(8, 8 + len));
          buf = buf.subarray(8 + len);
        }
      }
      DBG.log('pump end (reader done)');
      try { decoder.close(); } catch (e) {}
    },
    /** 单帧:seq 校验 + 解码 */
    onFrame(decoder, seq, opusFrame) {
      if (this._lastSeq !== 0 && seq !== this._lastSeq + 1) {
        // 丢帧/采集重启 → 重同步(清积压 + 重置解码器)
        this._resync = true;
        this._playQueue = [];
        this._nextPlayTime = 0;
        try { decoder.reset(); } catch (e) {}
        decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
      }
      this._lastSeq = seq;
      decoder.decode(new EncodedAudioChunk({
        type: 'key',
        timestamp: seq * 20000,   // 20ms 一帧
        duration: 20000,
        data: opusFrame,
      }));
    },
    /** 解码输出:PCM → AudioBuffer → 时间线排播 */
    onDecoded(audioData) {
      if (!this.playing) { audioData.close(); return; }
      try {
        const fmt = audioData.format || '';
        const chs = audioData.numberOfChannels;
        const rate = audioData.sampleRate;
        const isFloat = fmt.indexOf('f32') === 0;
        let nFrames = audioData.numberOfFrames;
        const planes = [];

        if (fmt === 'f32' || fmt === 's16' || fmt === 'u8') {
          // 交错格式(真机 Chrome opus 输出 f32 交错):整块拷贝后按声道拆分。
          // 交错格式下 planeIndex 必须为 0,单平面数据量为 frames×channels;
          // 此 Chrome 版本:allocationSize 必须显式传参;copyTo 目标用类型化数组
          const bytes = isFloat ? 4 : 2;
          let size = audioData.allocationSize({ planeIndex: 0 }) || (chs * nFrames * bytes);
          let raw = null;
          let firstErr = '';
          for (let k = 0; k < 8 && !raw; k++) {
            try {
              const dest = isFloat ? new Float32Array(size / 4) : new Int16Array(size / 2);
              audioData.copyTo(dest, { planeIndex: 0 });
              raw = dest;
            } catch (e) {
              if (!firstErr) firstErr = e.message;
              size *= 2;
            }
          }
          if (!raw) throw new Error('copyTo interleaved failed: ' + firstErr);
          const f = raw;   // Float32Array 或 Int16Array
          const per = Math.floor(f.length / chs);
          nFrames = per;
          for (let c = 0; c < chs; c++) {
            const p = new Float32Array(per);
            for (let i = 0; i < per; i++) {
              p[i] = isFloat ? f[i * chs + c] : f[i * chs + c] / 32768.0;
            }
            planes.push(p);
          }
        } else {
          // 平面格式:逐平面拷贝;尺寸按 allocationSize 精确分配,
          // 元数据异常时翻倍扩容重试,消除"destination is not large enough"
          for (let c = 0; c < chs; c++) {
            let size = audioData.allocationSize({ planeIndex: c });
            let ab = null;
            for (let k = 0; k < 8 && !ab; k++) {
              try {
                const buf = new ArrayBuffer(size);
                audioData.copyTo(buf, { planeIndex: c });
                ab = buf;
              } catch (e) { size *= 2; }
            }
            if (!ab) throw new Error('copyTo plane ' + c + ' failed');
            if (isFloat) {
              planes.push(new Float32Array(ab));
            } else {
              const i16 = new Int16Array(ab);
              const p = new Float32Array(i16.length);
              for (let i = 0; i < i16.length; i++) p[i] = i16[i] / 32768.0;
              planes.push(p);
            }
          }
        }
        audioData.close();
        DBG.decoded++;
        DBG.log('decoded chs=' + chs + ' frames=' + nFrames + ' rate=' + rate + ' fmt=' + fmt);
        if (!this._ctx || this._ctx.state !== 'running') {
          DBG.state = this._ctx ? this._ctx.state : 'null';
          DBG.log('onDecoded 丢弃: ctx state=' + (this._ctx ? this._ctx.state : 'null'));
          return;
        }
        // 声道/采样率与解码输出严格一致(播放时由 AudioContext 自动重采样)
        const ab = this._ctx.createBuffer(chs, nFrames, rate);
        for (let c = 0; c < chs; c++) ab.copyToChannel(planes[c], c);
        this._playQueue.push(ab);
        this.schedulePlayback();
      } catch (e) {
        DBG.log('onDecoded EXCEPTION: ' + e.message);
        try { audioData.close(); } catch (e2) {}
      }
    },
    schedulePlayback() {
      if (!this._ctx || this._ctx.state !== 'running') {
        DBG.state = this._ctx ? this._ctx.state : 'null';
        return;
      }
      // 积压超过 50 帧(1s)仍未排播 → 丢弃重来(网络抖动恢复后自动重同步)
      if (this._playQueue.length > 50) {
        DBG.log('schedule 积压>50 清空');
        this._playQueue = [];
        this._nextPlayTime = 0;
        return;
      }
      while (this._playQueue.length) {
        const buf = this._playQueue[0];
        const now = this._ctx.currentTime;
        if (this._nextPlayTime === 0) {
          // 首帧:预留 120ms jitter
          this._nextPlayTime = now + 0.12;
        } else if (this._nextPlayTime < now) {
          // 时间线落后(静音/网络间隙后恢复):重新锚定到 now+50ms。
          // 若继续用过去时刻 start(),WebAudio 会钳制到 currentTime,
          // 队列中多个缓冲几乎同时启动 → 重叠播放 → 撕裂爆音
          DBG.log('schedule 时间线落后重锚定 now=' + now.toFixed(3));
          this._nextPlayTime = now + 0.05;
        }
        if (this._nextPlayTime - now > 0.5) { DBG.log('schedule 积压>500ms 暂停'); break; }
        const src = this._ctx.createBufferSource();
        src.buffer = buf;
        src.connect(this._ctx.destination);
        src.start(this._nextPlayTime);
        this._nextPlayTime += buf.duration;
        this._playQueue.shift();
        DBG.scheduled++;
        this.statusText = '采集中';
      }
    },
  },
}).mount('#app');
