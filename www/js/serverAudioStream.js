/**
 * ZiMo Service — 服务器音频传输模块(门户工作区)
 * UI 模板:modules/serverAudioStream.html(懒加载注入);由门户壳 render() 调用 window.renderServerAudioStream(entry)
 * 从 portal.js 拆分(原 renderAudio(现 renderServerAudioStream)),解码/调度/重连逻辑未改动
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  window.renderServerAudioStream = async function (entry) {
    // 浏览器兼容性
    if (!window.isSecureContext || typeof AudioDecoder === 'undefined') {
      entry.el.innerHTML = `
        <div class="ws-placeholder">
          <div class="ws-icon">♫</div>
          <div class="ws-title">不支持的浏览器</div>
          <div>当前浏览器不支持音频解码(需 HTTPS + WebCodecs),请使用 Chrome / Edge</div>
        </div>`;
      return;
    }

    // 模块 UI 模板独立文件(modules/serverAudioStream.html),懒加载注入
    entry.el.innerHTML = await fetch('/modules/serverAudioStream.html').then((r) => r.text());

    const toggle = entry.el.querySelector('#auToggle');
    const auIcon = entry.el.querySelector('#auIcon');
    const statusEl = entry.el.querySelector('#auStatus');
    const paramsEl = entry.el.querySelector('#auParams');
    const listenersEl = entry.el.querySelector('#auListeners');
    const volumeEl = entry.el.querySelector('#auVolume');
    const canvas = entry.el.querySelector('#auCanvas');
    const ctx2d = canvas.getContext('2d');

    // ---- 音频状态 ----
    let ctx = null;          // AudioContext
    let gain = null;         // GainNode(音量)
    let analyser = null;     // AnalyserNode(频谱)
    let decoder = null;
    let playQueue = [];
    let nextPlayTime = 0;
    let lastSeq = 0;
    let abortCtrl = null;
    let retryDelay = 1000;
    let playing = false;
    let state = 'idle';
    let statTimer = null;
    let raf = 0;
    let lastFrameArrival = 0;
    let avgInterval = 0;
    let reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    function readLE32(u8, off) {
      return u8[off] | (u8[off + 1] << 8) | (u8[off + 2] << 16) | (u8[off + 3] << 24);
    }
    function concatBytes(a, b) {
      const out = new Uint8Array(a.length + b.length);
      out.set(a); out.set(b, a.length);
      return out;
    }

    function setState(s) {
      state = s;
      statusEl.className = 'au-status' + (s === 'reconnecting' ? ' reconnecting'
        : s === 'playing' ? ' playing' : '');
      toggle.classList.toggle('playing', s === 'playing');
      toggle.classList.toggle('connecting', s === 'connecting');
      // 错误态(断线重连/无设备):警示图标,与空闲/连接中的 ▶ 区分
      toggle.classList.toggle('err', s === 'reconnecting' || s === 'no-device');
      auIcon.textContent = s === 'playing' ? '⏸'
        : (s === 'reconnecting' || s === 'no-device') ? '⚠' : '▶';
      statusEl.textContent =
        s === 'idle' ? '聆听服务器' :
        s === 'connecting' ? '连接中…' :
        s === 'playing' ? '采集中' :
        s === 'reconnecting' ? '连接断开,重连中…' :
        s === 'no-device' ? '服务器无可用音频设备' : '已停止';
      if (s === 'playing') {
        // 恢复播放:重启频谱 rAF。错误恢复(setState('playing'))不会走 render 尾部/onActivate,
        // 若此处不重启,出错→恢复后四周音乐效果永久消失(直到切走再切回)
        if (!raf && !reduced) raf = requestAnimationFrame(drawSpectrum);
      } else {
        // 非播放状态:频谱画布复位(清残留帧 + 停 rAF)+ 收听数复位
        if (raf) { cancelAnimationFrame(raf); raf = 0; }
        ctx2d.clearRect(0, 0, canvas.width, canvas.height);
        listenersEl.textContent = '未在收听';
      }
    }

    function cleanup() {
      if (decoder) { try { decoder.close(); } catch (e) {} decoder = null; }
      playQueue = [];
      nextPlayTime = 0;
      lastSeq = 0;
    }

    function resync() {
      playQueue = [];
      nextPlayTime = 0;
      if (decoder) {
        try { decoder.reset(); } catch (e) {}
        try { decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 }); } catch (e) {}
      }
    }

    // ---- 解码输出:AudioData → AudioBuffer → 时间线排播(接入 gain/analyser) ----
    function onDecoded(audioData) {
      if (!playing) { audioData.close(); return; }
      try {
        const fmt = audioData.format || '';
        const chs = audioData.numberOfChannels;
        const rate = audioData.sampleRate;
        const isFloat = fmt.indexOf('f32') === 0;
        let nFrames = audioData.numberOfFrames;
        const planes = [];

        if (fmt === 'f32' || fmt === 's16' || fmt === 'u8') {
          const bytes = isFloat ? 4 : 2;
          let size = audioData.allocationSize({ planeIndex: 0 }) || (chs * nFrames * bytes);
          let raw = null;
          for (let k = 0; k < 8 && !raw; k++) {
            try {
              const dest = isFloat ? new Float32Array(size / 4) : new Int16Array(size / 2);
              audioData.copyTo(dest, { planeIndex: 0 });
              raw = dest;
            } catch (e) { size *= 2; }
          }
          if (!raw) throw new Error('copyTo interleaved failed');
          const f = raw;
          const per = Math.floor(f.length / chs);
          nFrames = per;
          for (let c = 0; c < chs; c++) {
            const p = new Float32Array(per);
            for (let i = 0; i < per; i++) p[i] = isFloat ? f[i * chs + c] : f[i * chs + c] / 32768.0;
            planes.push(p);
          }
        } else {
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
            if (!ab) throw new Error('copyTo plane failed');
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

        if (!ctx || ctx.state !== 'running') return;
        const ab = ctx.createBuffer(chs, nFrames, rate);
        for (let c = 0; c < chs; c++) ab.copyToChannel(planes[c], c);
        playQueue.push(ab);
        schedulePlayback();
      } catch (e) {
        try { audioData.close(); } catch (e2) {}
      }
    }

    function schedulePlayback() {
      if (!ctx || ctx.state !== 'running') return;
      // 积压超 50 帧(1s)丢弃重来(网络抖动恢复自动重同步)
      if (playQueue.length > 50) {
        playQueue = [];
        nextPlayTime = 0;
        return;
      }
      while (playQueue.length) {
        const buf = playQueue[0];
        const now = ctx.currentTime;
        if (nextPlayTime === 0) {
          nextPlayTime = now + 0.12;            // 首帧 120ms jitter
        } else if (nextPlayTime < now) {
          nextPlayTime = now;                   // 时间线落后:立即接续,勿 +0.05 制造静音空洞(爆音)
        }
        if (nextPlayTime - now > 0.5) break;    // 积压 >500ms 暂停排播
        const src = ctx.createBufferSource();
        src.buffer = buf;
        // 仅源→增益为每帧连接(gain→analyser→destination 初始化时已固定)
        src.connect(gain);
        src.start(nextPlayTime);
        nextPlayTime += buf.duration;
        playQueue.shift();
      }
    }

    // ---- 主循环:连接 → 读流拆帧 → 断线指数退避重连 ----
    async function runStream() {
      while (playing) {
        const abort = new AbortController();
        abortCtrl = abort;
        try {
          const resp = await fetch(A.api.audioStreamUrl(), { signal: abort.signal, credentials: 'include' });
          if (!resp.ok || !resp.body) {
            const txt = await resp.text().catch(() => '');
            let msg = `服务器返回 ${resp.status}`;
            try {
              const j = JSON.parse(txt);
              if (j.error && j.error.message) msg += ' ' + j.error.message;
            } catch (e) { if (txt) msg += ' ' + txt; }
            if (resp.status === 401) { A.toast('会话已失效', 'err'); setTimeout(toLogin, 800); return; }
            if (resp.status === 403) { A.toast('权限不足', 'err'); setState('idle'); playing = false; return; }
            if (resp.status === 503) {
              setState('no-device');
              A.toast('服务器无可用音频设备', 'err');
              playing = false;
              return;
            }
            statusEl.textContent = msg;
            throw new Error(msg);
          }
          setState('playing');
          await pump(resp.body.getReader());
        } catch (err) {
          if (!playing) return;                 // 主动停止
          if (err.name === 'AbortError') return;
          setState('reconnecting');
          // 错误可见化(供排查):状态行显示 fetch 失败原因
          const msg = err && err.message ? err.message : String(err);
          console.error('[audio]', err);
          statusEl.textContent = `连接断开,重连中…(${msg})`;
          cleanup();
          await new Promise((r) => setTimeout(r, retryDelay));
          retryDelay = Math.min(retryDelay * 2, 8000);
        }
      }
    }

    async function pump(reader) {
      decoder = new AudioDecoder({
        output: (audioData) => onDecoded(audioData),
        error: () => { resync(); },
      });
      try {
        decoder.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
      } catch (e) { throw e; }

      let buf = new Uint8Array(0);
      while (playing) {
        const { done, value } = await reader.read();
        if (done) break;
        if (value && value.length) buf = buf.length ? concatBytes(buf, value) : value;
        while (buf.length >= 8) {
          const len = readLE32(buf, 0);
          const seq = readLE32(buf, 4);
          if (buf.length < 8 + len) break;
          onFrame(seq, buf.subarray(8, 8 + len));
          buf = buf.subarray(8 + len);
        }
      }
      try { decoder.close(); } catch (e) {}
      decoder = null;
    }

    function onFrame(seq, opusFrame) {
      if (lastSeq !== 0) {
        const gap = seq - lastSeq;
        if (gap > 1 && gap <= 5) {
          // 单帧/少量丢帧(服务端网络慢丢最旧帧):不重同步。
          // 时间戳基于 seq 保持连续,20~100ms 内容空洞由排播自然静音,
          // 硬 resync 会清队列+重建锚点 → 声→静→声爆音(咔哒声主因)
          if (decoder) {
            try { decoder.reset(); } catch (e) {}   // 仅清解码器内部状态,不动排播时间线
          }
        } else if (gap > 5 || gap < 0) {
          resync();                                // 大幅失步/采集重启(seq 回退) → 重同步
        }
      }
      lastSeq = seq;
      // 延迟推算:帧到达间隔(理想 20ms)
      const now = performance.now();
      if (lastFrameArrival) {
        const iv = now - lastFrameArrival;
        avgInterval = avgInterval ? avgInterval * 0.9 + iv * 0.1 : iv;
      }
      lastFrameArrival = now;
      if (decoder) {
        decoder.decode(new EncodedAudioChunk({
          type: 'key', timestamp: seq * 20000, duration: 20000, data: opusFrame,
        }));
      }
    }

    // ---- 圆形频谱 ----
    function drawSpectrum() {
      raf = 0;
      if (!playing || !analyser || reduced) return;
      const w = canvas.width, h = canvas.height;
      const cx = w / 2, cy = h / 2;
      const base = Math.min(w, h) * 0.34;
      const data = new Uint8Array(analyser.frequencyBinCount);
      analyser.getByteFrequencyData(data);
      ctx2d.clearRect(0, 0, w, h);
      const N = 64;
      for (let i = 0; i < N; i++) {
        const v = data[Math.floor(i * (data.length / N))] / 255;
        const ang = (i / N) * Math.PI * 2;
        const r = base + v * (Math.min(w, h) * 0.18);
        const x = cx + Math.cos(ang) * r;
        const y = cy + Math.sin(ang) * r;
        // 彩色频谱:色相随角度全环渐变(HSL 彩虹),高亮线更亮更粗
        const hue = (i / N) * 360;
        const isHi = i % 8 === 0;
        ctx2d.strokeStyle = `hsla(${hue.toFixed(0)}, 75%, ${isHi ? 62 : 52}%, ${isHi ? 0.95 : 0.75})`;
        ctx2d.lineWidth = isHi ? 2 : 1.2;
        ctx2d.beginPath();
        ctx2d.moveTo(cx + Math.cos(ang) * base * 0.86, cy + Math.sin(ang) * base * 0.86);
        ctx2d.lineTo(x, y);
        ctx2d.stroke();
      }
      if (playing) raf = requestAnimationFrame(drawSpectrum);
    }

    function resizeCanvas() {
      const rect = canvas.parentElement.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      canvas.width = Math.round(rect.width * dpr);
      canvas.height = Math.round(rect.height * dpr);
    }
    window.addEventListener('resize', resizeCanvas);

    // ---- 音量 ----
    volumeEl.addEventListener('input', () => {
      if (gain) gain.gain.value = Number(volumeEl.value) / 100;
    });

    // ---- 端数轮询(兼权限感知:403 = 授权被取消,停止播放并提示) ----
    function startStatPolling() {
      clearInterval(statTimer);
      statTimer = setInterval(async () => {
        try {
          const r = await A.api.audioStatus();
          if (playing) listenersEl.textContent = `当前 ${r.subscriberCount} 个会话在收听`;
          if (!playing && !r.capturing) setState('idle');
        } catch (e) {
          if (e.code === 403) {
            // 授权被取消:停止播放(主动断流释放带宽)+ 提示
            A.toast('音频权限已被取消', 'err');
            stop();
          }
          // 401 由全局拦截跳登录;其他错误静默(网络抖动,流自行重连)
        }
      }, 5000);
    }

    // ---- 参数行(延迟) ----
    setInterval(() => {
      if (playing && avgInterval) {
        const delay = Math.max(0, Math.round(avgInterval - 20));
        paramsEl.textContent = `48kHz · 64kbps · 立体声 · 延迟 ${delay}ms`;
      }
    }, 1000);

    // ---- 开始 / 停止 ----
    async function start() {
      try {
        playing = true;
        setState('connecting');
        retryDelay = 1000;
        // 用户手势内创建 AudioContext(规避自动播放策略)
        if (!ctx) {
          ctx = new AudioContext({ sampleRate: 48000 });
          gain = ctx.createGain();
          gain.gain.value = Number(volumeEl.value) / 100;
          analyser = ctx.createAnalyser();
          analyser.fftSize = 256;
          // 音频图固定链路只连接一次:每帧重复 connect(gain→analyser→destination)
          // 会累积求和,输出指数放大 → 削波声噪(实测越听越严重)
          gain.connect(analyser);
          analyser.connect(ctx.destination);
          ctx.onstatechange = () => {
            if (ctx.state === 'running' && playing) resync();
          };
        }
        if (ctx.state === 'suspended') ctx.resume().catch(() => {});
        resizeCanvas();
        startStatPolling();
        if (!reduced) raf = requestAnimationFrame(drawSpectrum);   // 启动频谱循环
        await runStream();
      } catch (e) {
        // 启动失败可见化(供排查)
        A.toast('音频启动失败: ' + (e && e.message ? e.message : String(e)), 'err');
        playing = false;
        clearInterval(statTimer);
        setState('idle');
      }
    }

    function stop() {
      playing = false;
      if (abortCtrl) abortCtrl.abort();
      clearInterval(statTimer);
      cleanup();
      if (ctx) ctx.suspend().catch(() => {});
      setState('idle');
    }

    toggle.addEventListener('click', () => {
      playing ? stop() : start();
    });

    // keep-alive:切走不销毁,继续播放;onActivate 不重置
    entry.onDeactivate = () => {
      if (raf) cancelAnimationFrame(raf);
      raf = 0;
    };
    entry.onActivate = () => {
      if (playing) {
        resizeCanvas();
        if (!reduced) raf = requestAnimationFrame(drawSpectrum);
      }
    };
    if (!reduced) raf = requestAnimationFrame(drawSpectrum);
  }
})();
