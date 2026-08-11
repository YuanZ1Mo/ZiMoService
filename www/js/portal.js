/**
 * ZiMo Service — 门户壳(portal.html)
 * 顶栏(状态/昵称下拉)/ 功能目录 / History 路由工作区 / 401 统一拦截 / 心跳
 */
(function () {
  'use strict';
  const A = window.ZmAuth;
  const $ = (id) => document.getElementById(id);

  let user = null;
  let modules = [];
  let currentPath = null;
  /** 模块视图缓存:url → { el, mod, onActivate, onDeactivate }
   *  容器常驻不销毁(keep-alive),切换仅隐藏/显示 —— 模块运行中状态天然保留;
   *  各容器独立滚动,滚动位置随容器保留。刷新后缓存清空(重新初始化)。
   *  模块可挂载生命周期钩子(框架约定):onActivate(进入/恢复)、onDeactivate(离开/暂停)。 */
  const viewCache = {};

  const ICONS = { home: '⌂', filehub: '▤', audio: '♫', users: '◈' };

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g,
      (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  /* ========================================================================
     401 统一拦截:记录回跳地址 → 登录页
     ======================================================================== */

  function toLogin() {
    window.location.href = '/login?redirect=' + encodeURIComponent(location.pathname);
  }

  /* ========================================================================
     初始化:拉取用户信息 + 已授权模块
     ======================================================================== */

  async function loadInfo() {
    const r = await A.api.portalInfo();
    user = r.user;
    modules = r.modules || [];
    renderTopbar();
    renderSideNav();
    $('portalLoading').hidden = true;
    $('workspace').hidden = false;
    render(location.pathname);   // 按当前 URL 渲染(直接访问子路由时恢复对应工作区)
  }

  /* ========================================================================
     顶栏
     ======================================================================== */

  function renderTopbar() {
    $('userMenuTrigger').textContent = (user.nickname || user.account) + ' ▾';
    startHeartbeat();
  }

  /* ========================================================================
     功能目录(服务端已按权限过滤,未授权模块不渲染)
     ======================================================================== */

  function renderSideNav() {
    const nav = $('sideNav');
    nav.innerHTML = '';
    modules.forEach((m) => {
      const btn = document.createElement('button');
      btn.className = 'side-item';
      btn.dataset.url = m.url;
      btn.innerHTML =
        `<span class="side-icon">${ICONS[m.code] || '•'}</span><span class="side-name">${escapeHtml(m.name)}</span>`;
      btn.addEventListener('click', () => {
        navigateTo(m.url, true);
        closeDrawer();
      });
      nav.appendChild(btn);
    });
    updateActive();
  }

  function updateActive() {
    document.querySelectorAll('.side-item').forEach((el) => {
      el.classList.toggle('active', el.dataset.url === location.pathname);
    });
  }

  /* ========================================================================
     History 路由:本地鉴权 → pushState → 渲染
     ======================================================================== */

  function navigateTo(path, push) {
    if (!modules.some((m) => m.url === path)) {
      A.toast('无权限访问该模块', 'err');
      return;
    }
    if (push) history.pushState({}, '', path);
    render(path);
  }

  window.addEventListener('popstate', () => render(location.pathname));

  function render(path) {
    const mod = modules.find((m) => m.url === path);
    if (!mod) {
      // 直接访问无权限/未知路径:回主页(主页必在列表内)
      A.toast('无权限访问该模块', 'err');
      history.replaceState({}, '', '/portal');
      render('/portal');
      return;
    }

    // 离开当前模块:隐藏容器 + onDeactivate 钩子(容器不销毁,状态保留)
    if (currentPath && viewCache[currentPath]) {
      const cur = viewCache[currentPath];
      cur.el.classList.remove('active');
      if (cur.onDeactivate) cur.onDeactivate();
    }

    // 进入目标模块:首次创建渲染,之后复用缓存容器
    let entry = viewCache[path];
    if (!entry) {
      const el = document.createElement('div');
      el.className = 'ws-view';
      entry = { el, mod, onActivate: null, onDeactivate: null };
      viewCache[path] = entry;
      $('workspace').appendChild(el);
      if (mod.code === 'home') renderHome(entry);
      else renderPlaceholder(entry, mod);
    }
    entry.el.classList.add('active');
    if (entry.onActivate) entry.onActivate();

    currentPath = path;
    updateActive();
  }

  function renderHome(entry) {
    entry.el.innerHTML = `
      <div class="ws-home">
        <div class="hello">欢迎回来,${escapeHtml(user.nickname || user.account)}</div>
        <div class="meta">ZiMo Service v1.0 · 门户框架已就绪</div>
      </div>
      <div class="ws-sections">
        <section class="ws-section">
          <h3 class="ws-section-title">服务概览</h3>
          <div class="stat-grid">
            <div class="stat-card"><div class="stat-num">${modules.length}</div><div class="stat-label">功能模块</div></div>
            <div class="stat-card"><div class="stat-num">${escapeHtml(user.role)}</div><div class="stat-label">账号角色</div></div>
            <div class="stat-card"><div class="stat-num">v1.0</div><div class="stat-label">服务版本</div></div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">我的模块</h3>
          <div class="ws-lines">
            ${modules.map((m) => `· ${escapeHtml(m.name)} — ${escapeHtml(m.url)}`).join('')}
          </div>
        </section>
      </div>`;
  }

  function renderPlaceholder(entry, mod) {
    if (mod.code === 'users') {
      renderUsers(entry);   // 用户管理已实现
      return;
    }
    if (mod.code === 'audio') {
      renderAudio(entry);   // 服务器音频传输已实现
      return;
    }
    if (mod.code === 'filehub') {
      window.renderFilehub(entry);   // 文件中心已实现(filehub.js 独立文件)
      return;
    }
    entry.el.innerHTML = `
      <div class="ws-placeholder">
        <div class="ws-icon">${ICONS[mod.code] || '•'}</div>
        <div class="ws-title">${escapeHtml(mod.name)}</div>
        <div>建设中,敬请期待</div>
      </div>`;
  }

  /* ========================================================================
     通用弹窗(返回 {action, value, values})
     ======================================================================== */

  function showModal({ title, content, buttons }) {
    return new Promise((resolve) => {
      const mask = document.createElement('div');
      mask.className = 'modal-mask';
      mask.innerHTML = `
        <div class="modal">
          <div class="modal-title">${escapeHtml(title)}</div>
          <div class="modal-body">${content}</div>
          <div class="modal-actions"></div>
        </div>`;
      const actions = mask.querySelector('.modal-actions');
      buttons.forEach((b) => {
        const btn = document.createElement('button');
        btn.className = b.primary ? 'btn-primary' : 'btn-ghost';
        btn.style.cssText = b.primary
          ? 'width:auto;padding:7px 18px;font-size:13px' + (b.danger ? ';background:var(--err);' : '')
          : 'padding:7px 14px;font-size:13px';
        btn.textContent = b.label;
        btn.addEventListener('click', () => {
          const input = mask.querySelector('[data-modal-input]');
          const checked = Array.from(mask.querySelectorAll('[data-modal-value]:checked'))
            .map((c) => c.dataset.modalValue);   // checkbox 无 value 属性,取 data-modal-value
          mask.remove();
          resolve({ action: b.value, value: input ? input.value : null, values: checked });
        });
        actions.appendChild(btn);
      });
      // 手动关闭:点击遮罩不关闭(需点按钮/关闭入口)
      document.body.appendChild(mask);
      const input = mask.querySelector('[data-modal-input]');
      if (input) input.focus();
    });
  }

  /* ========================================================================
     服务器音频传输工作区(圆形频谱声场)
     解码/调度/重连逻辑沿用旧前端成熟实现,UI 全新设计
     ======================================================================== */

  function renderAudio(entry) {
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

    entry.el.innerHTML = `
      <div class="au-stage">
        <canvas class="au-spectrum" id="auCanvas"></canvas>
        <button class="au-toggle" id="auToggle" title="开始收听">
          <span class="au-disc"></span>
          <span class="au-hub" id="auIcon">▶</span>
        </button>
        <div class="au-status" id="auStatus">聆听服务器</div>
      </div>
      <div class="au-meta">
        <div class="au-meta-mono" id="auParams">48kHz · 64kbps · 立体声 · 延迟 —</div>
        <div class="au-meta-mono" id="auListeners">未在收听</div>
        <div class="au-volume"><span>音量</span><input type="range" id="auVolume" min="0" max="100" value="100"></div>
      </div>`;

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
      auIcon.textContent = s === 'playing' ? '⏸' : '▶';
      statusEl.textContent =
        s === 'idle' ? '聆听服务器' :
        s === 'connecting' ? '连接中…' :
        s === 'playing' ? '采集中' :
        s === 'reconnecting' ? '连接断开,重连中…' :
        s === 'no-device' ? '服务器无可用音频设备' : '已停止';
      // 非播放状态:频谱画布复位(清残留帧 + 停 rAF)+ 收听数复位
      if (s !== 'playing') {
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
          nextPlayTime = now + 0.05;            // 时间线落后重锚定,防重叠爆音
        }
        if (nextPlayTime - now > 0.5) break;    // 积压 >500ms 暂停排播
        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(gain);
        gain.connect(analyser);
        analyser.connect(ctx.destination);
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
      if (lastSeq !== 0 && seq !== lastSeq + 1) {
        resync();                                // 丢帧/采集重启 → 重同步
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
        ctx2d.strokeStyle = i % 8 === 0 ? 'rgba(232,174,96,0.85)' : 'rgba(192,86,46,0.7)';
        ctx2d.lineWidth = i % 8 === 0 ? 2 : 1.2;
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

  /* ========================================================================
     用户管理工作区
     ======================================================================== */

  const ROLE_LV = { developer: 3, admin: 2, user: 1 };
  const ROLE_NAMES = { developer: '开发者', admin: '管理员', user: '用户' };

  function renderUsers(entry) {
    const st = { keyword: '', role: '', status: 0, page: 1, pageSize: 20, total: 0, list: [], opRole: user.role };

    entry.el.innerHTML = `
      <div class="um-toolbar">
        <div class="um-search">
          <select id="umRole">
            <option value="">全部角色</option>
            <option value="developer">开发者</option>
            <option value="admin">管理员</option>
            <option value="user">用户</option>
          </select>
          <select id="umStatus">
            <option value="0">全部状态</option>
            <option value="1">正常</option>
            <option value="2">已停用</option>
          </select>
          <input id="umKeyword" placeholder="搜索账号" spellcheck="false">
          <button class="um-btn" id="umSearchBtn">搜索</button>
        </div>
      </div>
      <div class="um-table-wrap">
        <table class="um-table">
          <thead><tr>
            <th>账号</th><th>昵称</th><th>角色</th><th>状态</th>
            <th>注册时间</th><th>最后登录</th><th>操作</th>
          </tr></thead>
          <tbody id="umBody"></tbody>
        </table>
      </div>
      <div class="um-pager">
        <button id="umPrev">上一页</button>
        <span id="umPageInfo">1/1</span>
        <button id="umNext">下一页</button>
      </div>`;

    function canOperate(target) {
      const opLv = ROLE_LV[st.opRole] || 0;
      const tgtLv = ROLE_LV[target.role] || 0;
      return opLv > tgtLv;
    }

    async function loadUsers() {
      try {
        const r = await A.api.userList(st.keyword, st.role, st.status, st.page, st.pageSize);
        st.total = r.total;
        st.list = r.list || [];
        renderTable();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    function renderTable() {
      const body = entry.el.querySelector('#umBody');
      if (!st.list.length) {
        body.innerHTML = '<tr><td colspan="7" style="text-align:center;padding:32px;color:var(--fg3)">暂无用户</td></tr>';
        return;
      }
      body.innerHTML = st.list.map((u) => {
        const canOp = canOperate(u);
        const self = u.account === user.account;
        const dis = canOp && !self ? '' : 'disabled';
        const roleBadge = `<span class="badge ${u.role}">${ROLE_NAMES[u.role] || u.role}</span>`;
        const statusBadge = u.disabled
          ? '<span class="badge disabled">已停用</span>'
          : '<span class="badge user">正常</span>';
        const btns = [];
        btns.push(u.disabled
          ? `<button class="um-btn" data-act="enable" data-id="${u.id}" ${dis}>启用</button>`
          : `<button class="um-btn" data-act="disable" data-id="${u.id}" ${dis}>停用</button>`);
        btns.push(`<button class="um-btn" data-act="reset" data-id="${u.id}" ${dis}>重置密码</button>`);
        btns.push(`<button class="um-btn" data-act="nickname" data-id="${u.id}" ${dis}>昵称</button>`);
        btns.push(`<button class="um-btn" data-act="modules" data-id="${u.id}" ${dis}>授权</button>`);
        btns.push(`<button class="um-btn" data-act="role" data-id="${u.id}" ${dis}>角色</button>`);
        btns.push(`<button class="um-btn danger" data-act="delete" data-id="${u.id}" ${dis}>删除</button>`);
        return `<tr>
          <td class="mono">${escapeHtml(u.account)}</td>
          <td>${escapeHtml(u.nickname)}</td>
          <td>${roleBadge}</td>
          <td>${statusBadge}</td>
          <td><span class="mono-time">${A.formatTime(u.registerTime)}</span></td>
          <td><span class="mono-time">${A.formatTime(u.lastLoginTime)}</span></td>
          <td><div class="um-actions">${btns.join('')}</div></td>
        </tr>`;
      }).join('');
      const pages = Math.max(1, Math.ceil(st.total / st.pageSize));
      entry.el.querySelector('#umPageInfo').textContent = `${st.page}/${pages}`;
      entry.el.querySelector('#umPrev').disabled = st.page <= 1;
      entry.el.querySelector('#umNext').disabled = st.page >= pages;
    }

    async function doAction(id, action, body) {
      try {
        await A.api.userAction(id, action, body);
        A.toast('操作成功');
        loadUsers();
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    async function onUmClick(e) {
      const btn = e.target.closest('button[data-act]');
      if (!btn || btn.disabled) return;
      const id = Number(btn.dataset.id);
      const act = btn.dataset.act;

      if (act === 'disable' || act === 'enable') {
        await doAction(id, act, {});
      } else if (act === 'delete') {
        const r = await showModal({
          title: '删除用户',
          content: '<div class="modal-hint">删除后该账号不可登录,用户列表不再显示;可随时恢复。确定删除?</div>',
          buttons: [{ label: '取消', value: 'cancel' }, { label: '确认删除', value: 'ok', primary: true, danger: true }],
        });
        if (r.action === 'ok') await doAction(id, 'delete', {});
      } else if (act === 'reset') {
        const r = await showModal({
          title: '重置密码',
          content: '<div class="modal-hint">将生成一次性临时密码,并强制该用户下次登录时设置新密码与新救援码。确定继续?</div>',
          buttons: [{ label: '取消', value: 'cancel' }, { label: '确认重置', value: 'ok', primary: true }],
        });
        if (r.action !== 'ok') return;
        try {
          const resp = await A.api.userAction(id, 'reset-password');
          await showModal({
            title: '临时密码(仅显示一次)',
            content: `<div class="temp-pwd">${escapeHtml(resp.tempPassword)}</div>
              <div class="modal-hint">请立即转交用户。用户首次登录后须设置新密码与新救援码,完成后此密码失效。</div>`,
            buttons: [{ label: '我已转交', value: 'ok', primary: true }],
          });
          loadUsers();
        } catch (e2) {
          if (e2.code !== 401) A.toast(e2.message, 'err');
        }
      } else if (act === 'nickname') {
        // 默认填入当前昵称,修改者直接改
        const row = st.list.find((u) => u.id === id);
        const current = row ? row.nickname : '';
        const r = await showModal({
          title: '修改昵称',
          content: `<div class="field"><input data-modal-input maxlength="20" placeholder="1-20 位" value="${escapeHtml(current)}"></div>`,
          buttons: [{ label: '取消', value: 'cancel' }, { label: '保存', value: 'ok', primary: true }],
        });
        if (r.action === 'ok' && r.value) await doAction(id, 'nickname', { nickname: r.value });
      } else if (act === 'modules') {
        await openModulesModal(id);
      } else if (act === 'role') {
        await openRoleModal(id);
      }
    }

    async function openModulesModal(id) {
      try {
        const [detail, info] = await Promise.all([A.api.userDetail(id), A.api.portalInfo()]);
        const allMods = info.modules || [];
        const owned = new Set((detail.modules || []).map((m) => m.code));
        const checks = allMods.map((m) => {
          const forced = m.code === 'users' && detail.role === 'admin';   // 管理员必备
          return `<label><input type="checkbox" data-modal-value="${escapeHtml(m.code)}"
            ${owned.has(m.code) || forced ? 'checked' : ''} ${forced ? 'disabled' : ''}>
            ${escapeHtml(m.name)}
            ${forced ? '<span style="color:var(--fg3);font-size:12px">管理员必备</span>' : ''}
            <span style="color:var(--fg3);font-size:12px">${escapeHtml(m.url)}</span></label>`;
        }).join('');
        const r = await showModal({
          title: `模块授权 · ${escapeHtml(detail.account)}`,
          content: `<div class="modal-checks">${checks}</div>
            <div class="modal-hint">授权变更在用户下次进入门户时生效。</div>`,
          buttons: [{ label: '取消', value: 'cancel' }, { label: '保存', value: 'ok', primary: true }],
        });
        if (r.action === 'ok') {
          await doAction(id, 'modules', { modules: r.values });
        }
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    async function openRoleModal(id) {
      try {
        const detail = await A.api.userDetail(id);
        const opLv = ROLE_LV[st.opRole] || 0;
        const opts = Object.keys(ROLE_LV).filter((r) => ROLE_LV[r] > 0 && ROLE_LV[r] < opLv);
        const options = opts.map((r) =>
          `<option value="${r}" ${r === detail.role ? 'selected' : ''}>${ROLE_NAMES[r]}</option>`).join('');
        const r = await showModal({
          title: `角色修改 · ${escapeHtml(detail.account)}`,
          content: `<select class="modal-select" data-modal-input>${options}</select>
            <div class="modal-hint">仅可授予低于自己等级的角色(提升最高到自己的下一级);同级与更高级不可操作。</div>`,
          buttons: [{ label: '取消', value: 'cancel' }, { label: '保存', value: 'ok', primary: true }],
        });
        if (r.action === 'ok' && r.value) await doAction(id, 'role', { role: r.value });
      } catch (e) {
        if (e.code !== 401) A.toast(e.message, 'err');
      }
    }

    // 事件绑定
    entry.el.addEventListener('click', onUmClick);
    const kwInput = entry.el.querySelector('#umKeyword');
    const roleSel = entry.el.querySelector('#umRole');
    const statusSel = entry.el.querySelector('#umStatus');
    const doSearch = () => {
      st.keyword = kwInput.value.trim();
      st.role = roleSel.value;
      st.status = Number(statusSel.value) || 0;
      st.page = 1;
      loadUsers();
    };
    entry.el.querySelector('#umSearchBtn').addEventListener('click', doSearch);
    kwInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') doSearch(); });
    roleSel.addEventListener('change', doSearch);      // 筛选即时生效
    statusSel.addEventListener('change', doSearch);
    entry.el.querySelector('#umPrev').addEventListener('click', () => {
      if (st.page > 1) { st.page--; loadUsers(); }
    });
    entry.el.querySelector('#umNext').addEventListener('click', () => {
      if (st.page * st.pageSize < st.total) { st.page++; loadUsers(); }
    });

    // 激活时刷新列表(keep-alive 下每次切入重新拉取,数据保持最新)
    entry.onActivate = loadUsers;
  }

  /* ========================================================================
     心跳(与 index 一致:在线/连接中断/401 立即下线/服务端错误 1 次下线)
     ======================================================================== */

  function startHeartbeat() {
    const connState = $('connState');
    const connText = $('connText');
    let netDownShown = false;
    A.startHeartbeat({
      onExpired: toLogin,
      onServerFail() {
        A.toast('连接异常,请重新登录', 'err');
        setTimeout(toLogin, 1000);
      },
      onNetDown() {
        connState.classList.add('warn');
        connState.classList.remove('err');
        connText.textContent = '连接中断';
        if (!netDownShown) {
          netDownShown = true;
          A.toast('网络连接中断');
        }
      },
      onNetUp() {
        connState.classList.remove('warn', 'err');
        connText.textContent = '在线';
        netDownShown = false;
        A.toast('网络已恢复');
      },
    });
  }

  /* ========================================================================
     昵称下拉(桌面 hover;移动端点击)
     ======================================================================== */

  const userMenu = $('userMenu');
  const menuTrigger = $('userMenuTrigger');
  const dropdown = $('userDropdown');

  let hideTimer = null;

  function showDropdown(show) {
    clearTimeout(hideTimer);
    dropdown.hidden = !show;
  }

  {
    const isMobile = window.matchMedia('(max-width: 768px)').matches;
    if (isMobile) {
      menuTrigger.addEventListener('click', (e) => {
        e.stopPropagation();
        showDropdown(dropdown.hidden);
      });
    } else {
      // hover 显示;延迟关闭(200ms)防鼠标快速掠过间隙时闪烁消失
      userMenu.addEventListener('mouseenter', () => showDropdown(true));
      userMenu.addEventListener('mouseleave', () => {
        hideTimer = setTimeout(() => { dropdown.hidden = true; }, 200);
      });
      dropdown.addEventListener('mouseenter', () => clearTimeout(hideTimer));
      menuTrigger.addEventListener('click', () => showDropdown(dropdown.hidden));
    }
  }
  document.addEventListener('click', (e) => {
    if (!userMenu.contains(e.target)) showDropdown(false);
  });

  dropdown.addEventListener('click', async (e) => {
    const action = e.target.dataset && e.target.dataset.action;
    if (!action) return;
    showDropdown(false);
    if (action === 'logout') {
      try {
        await A.api.logout();
        window.location.href = '/login';
      } catch (err) {
        if (err.code === 401) window.location.href = '/login';
        else A.toast('登出失败,请检查网络', 'err');
      }
    } else {
      A.toast('建设中,敬请期待');
    }
  });

  /* ========================================================================
     目录展开/收起(桌面,localStorage 记忆);移动端抽屉
     ======================================================================== */

  const sideToggle = $('sideToggle');
  const sideMask = $('sideMask');

  function closeDrawer() {
    document.body.classList.remove('portal-open');
    sideMask.hidden = true;
  }

  {
    const isMobile = window.matchMedia('(max-width: 768px)').matches;
    if (isMobile) {
      const toggleDrawer = () => {
        const open = document.body.classList.toggle('portal-open');
        sideMask.hidden = !open;
      };
      $('sideToggleMobile').addEventListener('click', toggleDrawer);   // 顶栏汉堡
      sideMask.addEventListener('click', closeDrawer);
    } else {
      if (localStorage.getItem('portalCollapsed') === '1')
        document.body.classList.add('portal-collapsed');
      sideToggle.addEventListener('click', () => {
        const collapsed = document.body.classList.toggle('portal-collapsed');
        localStorage.setItem('portalCollapsed', collapsed ? '1' : '0');
      });
    }
  }

  /* ========================================================================
     启动
     ======================================================================== */

  loadInfo().catch((e) => {
    if (e.code === 401) {
      toLogin();
      return;
    }
    $('portalLoading').hidden = true;
    $('portalError').hidden = false;
  });

  $('retryBtn').addEventListener('click', () => window.location.reload());
})();
