/* ============================================================
   ZiMo 服务器音频 · 设计稿共享脚本
   ① 主题(亮/暗 + ?theme= 预览参数)  ② 舞台状态渲染  ③ 时间驱动特效画布
   注意:真实实现中特效必须是"按播放时间驱动"的,禁止接 WebAudio 分析
   ============================================================ */
window.SA = (function () {
  /* ---------- 主题 ---------- */
  function initTheme() {
    var q = new URLSearchParams(location.search).get('theme');
    if (q) document.documentElement.dataset.theme = q;
    else if (localStorage.getItem('mock-theme')) document.documentElement.dataset.theme = localStorage.getItem('mock-theme');
  }
  function toggleTheme() {
    var d = document.documentElement;
    d.dataset.theme = d.dataset.theme === 'dark' ? 'light' : 'dark';
    localStorage.setItem('mock-theme', d.dataset.theme);
  }

  /* ---------- 状态字典(文案与界面一一对应需求 §6.4 / 交接 §6.4) ---------- */
  var ICON = {
    play:      '<svg width="46" height="46" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M8.6 5.4a1.2 1.2 0 0 1 1.82-1.03l8.1 5.6a1.2 1.2 0 0 1 0 1.98l-8.1 5.6A1.2 1.2 0 0 1 8.6 16.6Z"/></svg>',
    pause:     '<svg width="44" height="44" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="6.6" y="5" width="3.9" height="14" rx="1.6"/><rect x="13.5" y="5" width="3.9" height="14" rx="1.6"/></svg>',
    spinner:   '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 3.4a8.6 8.6 0 0 1 8.6 8.6" opacity=".95"/><path d="M12 20.6A8.6 8.6 0 0 1 3.4 12" opacity=".45"/><path d="M3.4 12A8.6 8.6 0 0 1 12 3.4" opacity=".28"/></svg>',
    refresh:   '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.3" stroke-linecap="round" aria-hidden="true"><path d="M20.6 12a8.6 8.6 0 1 1-2.5-6.05"/><path d="M20.6 4.2v5.4h-5.4"/></svg>',
    warn:      '<svg width="42" height="42" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 4.6 2.9 19.6a1.1 1.1 0 0 0 .95 1.65h16.3a1.1 1.1 0 0 0 .95-1.65Z"/><path d="M12 10.2v4.3"/><circle cx="12" cy="17.6" r="1.05" fill="currentColor" stroke="none"/></svg>',
    info:      '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11.2v5"/><circle cx="12" cy="7.8" r="1.05" fill="currentColor" stroke="none"/></svg>',
    wave:      '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M3 12h2.6l1.8-4.6L10 17l2.6-6.4L14.6 14h6.4"/></svg>',
    waveFaint: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M3 12h3l1.6-3.2L9.6 15l2-3h9.4"/></svg>',
    spinSm:    '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M12 3.6a8.4 8.4 0 0 1 8.4 8.4"/><path d="M20.4 12A8.4 8.4 0 0 1 12 20.4" opacity=".5"/></svg>',
    warnSm:    '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 4.6 2.9 19.6a1.1 1.1 0 0 0 .95 1.65h16.3a1.1 1.1 0 0 0 .95-1.65Z"/><path d="M12 10.2v4.3"/><circle cx="12" cy="17.6" r="1.05" fill="currentColor" stroke="none"/></svg>',
    pauseSm:   '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="7" y="5.6" width="3.6" height="12.8" rx="1.5"/><rect x="13.4" y="5.6" width="3.6" height="12.8" rx="1.5"/></svg>'
  };

  var STATES = {
    idle:        { text: '聆听服务器',          extra: '',                                  ico: 'play',    cap: '点击开始收听', cls: 'idle',         tone: '',     chip: '空闲',        sico: 'info' },
    connecting:  { text: '连接中…',             extra: '握手取流转描述 + 等待首片就绪',        ico: 'spinner', cap: '点击取消',     cls: 'connecting',   tone: '',     chip: '准备中',      sico: 'spinSm' },
    playing:     { text: '正在播放',            extra: '',                                  ico: 'pause',   cap: '点击停止',     cls: 'playing',      tone: 'ok',   chip: '采集中',      sico: 'wave' },
    paused:      { text: '已暂停',              extra: '保留连接 · 60 秒内可无缝续听',         ico: 'play',    cap: '点击继续',     cls: 'paused',       tone: '',     chip: '采集中',      sico: 'pauseSm' },
    reconnecting:{ text: '连接中断,重连中…',     extra: '网络请求超时 · 第 2 次重试',           ico: 'refresh', cap: '点击停止',     cls: 'reconnecting', tone: 'warn', chip: '重连中',      sico: 'warnSm' },
    'no-device': { text: '服务器无可用音频设备',  extra: '请检查默认播放设备后重试',             ico: 'warn',    cap: '点击重试',     cls: 'nodevice',     tone: 'err',  chip: '设备不可用',  sico: 'warnSm' }
  };

  /* ---------- 特效画布:按时间驱动的律动(不接音频分析) ---------- */
  var PAL = {
    idle:         ['#38BDF8', '#22D3EE', '#7DD3FC'],
    connecting:   ['#38BDF8', '#7DD3FC', '#22D3EE'],
    playing:      ['#22D3EE', '#38BDF8', '#F472B6'],
    paused:       ['#38BDF8', '#22D3EE', '#7DD3FC'],
    reconnecting: ['#FBBF24', '#F59E0B', '#FCD34D'],
    'no-device':  ['#F87171', '#FBBF24', '#FCA5A5']
  };
  var LEVEL = { idle: .07, connecting: .2, playing: .62, paused: .1, reconnecting: .24, 'no-device': .04 };
  var SPIN  = { idle: .05, connecting: .5, playing: .34, paused: .05, reconnecting: .28, 'no-device': .02 };

  function mix(hex, a) {
    var n = parseInt(hex.slice(1), 16);
    return 'rgba(' + (n >> 16 & 255) + ',' + (n >> 8 & 255) + ',' + (n & 255) + ',' + a + ')';
  }

  function fx(canvas, disc, getState) {
    var ctx = canvas.getContext('2d'), dpr = 1, w = 0, h = 0, cx = 0, cy = 0, R = 90, rot = 0;
    var reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    var raf = null, last = 0, t = 0;

    function layout() {
      var cr = canvas.getBoundingClientRect(), dr = disc.getBoundingClientRect();
      if (!cr.width || !cr.height) return;
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      w = cr.width; h = cr.height;
      canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      cx = dr.left - cr.left + dr.width / 2;
      cy = dr.top - cr.top + dr.height / 2 - 6;
      R = dr.width / 2 + 4;
    }

    function frame(dt) {
      var key = getState() || 'idle';
      var pal = PAL[key] || PAL.idle, lv = LEVEL[key] || .08, spin = SPIN[key] || .1;
      rot += dt * spin;
      t += dt;
      ctx.clearRect(0, 0, w, h);

      /* 盘后柔光 */
      var g = ctx.createRadialGradient(cx, cy, R * .4, cx, cy, Math.max(R * 2.5, 180));
      g.addColorStop(0, mix(pal[0], .16 + .3 * lv));
      g.addColorStop(.55, mix(pal[1], .05 + .1 * lv));
      g.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(cx, cy, Math.max(R * 2.5, 180), 0, Math.PI * 2); ctx.fill();

      /* 放射律动条 */
      var N = 68, maxAmp = Math.min(R * .5, 86);
      ctx.lineCap = 'round';
      for (var i = 0; i < N; i++) {
        var a = (i / N) * Math.PI * 2 + rot;
        var wv = Math.sin(t * 1.35 + i * .42) * .5 + .5;
        var wv2 = Math.sin(t * .62 + i * .19 + 1.3) * .5 + .5;
        var amp = lv * (.3 + .7 * wv * wv2) * maxAmp;
        if (amp < .6) continue;
        var r0 = R + 10, r1 = R + 10 + amp;
        ctx.strokeStyle = mix(pal[i % 3], .18 + .62 * wv * Math.min(lv * 1.8, 1));
        ctx.lineWidth = i % 4 === 0 ? 3.4 : 2.2;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0);
        ctx.lineTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1);
        ctx.stroke();
      }

      /* 基环 + 呼吸环 */
      ctx.strokeStyle = mix(pal[0], .16 + .18 * lv);
      ctx.lineWidth = 1.2;
      ctx.beginPath(); ctx.arc(cx, cy, R + 6, 0, Math.PI * 2); ctx.stroke();
      var breath = .5 + .5 * Math.sin(t * 1.1);
      ctx.strokeStyle = mix(pal[2], .06 + .2 * breath * Math.min(lv * 2.4, 1));
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.arc(cx, cy, R + 26 + breath * 10, 0, Math.PI * 2); ctx.stroke();

      /* 外圈虚线环(缓慢旋转) */
      ctx.save();
      ctx.setLineDash([3, 9]);
      ctx.strokeStyle = mix(pal[1], .2 + .12 * lv);
      ctx.lineWidth = 1.1;
      ctx.beginPath(); ctx.arc(cx, cy, R + 52, rot * .6, rot * .6 + Math.PI * 2); ctx.stroke();
      ctx.restore();
    }

    function loop(ts) {
      var dt = last ? Math.min((ts - last) / 1000, .05) : .016;
      last = ts; frame(dt);
      raf = requestAnimationFrame(loop);
    }
    function start() {
      layout();
      if (reduce) { frame(.016); return; }
      if (raf == null) raf = requestAnimationFrame(loop);
    }
    function stop() { if (raf != null) { cancelAnimationFrame(raf); raf = null; } }
    function resize() { layout(); if (reduce) frame(.016); }
    window.addEventListener('resize', resize);
    return { start: start, stop: stop, resize: resize, reduced: reduce };
  }

  /* ---------- 舞台挂载:把状态映射到圆盘/状态行/顶栏徽标 ---------- */
  function mount(opt) {
    var st = opt.state || 'idle';
    var chipEl = opt.chip, chipText = opt.chipText, chipCls = 'sa-chip';
    var engine = fx(opt.canvas, opt.disc, function () { return st; });

    function setState(key) {
      var s = STATES[key] || STATES.idle;
      st = key;
      opt.disc.className = 'sa-disc is-' + s.cls;
      opt.ico.innerHTML = ICON[s.ico];
      if (opt.cap) opt.cap.textContent = s.cap;
      opt.text.textContent = s.text;
      if (opt.statusIco) opt.statusIco.innerHTML = ICON[s.sico] || ICON.info;
      if (opt.extra) { opt.extra.textContent = s.extra; opt.extra.style.display = s.extra ? '' : 'none'; }
      if (opt.status) opt.status.className = 'sa-status' + (s.tone ? ' is-' + s.tone : '');
      if (chipEl && chipText) {
        chipText.textContent = s.chip;
        chipEl.className = chipCls + (s.tone === 'ok' ? ' ok' : s.tone === 'warn' || s.tone === 'err' ? ' warn' : ' idle');
      }
      if (opt.onState) opt.onState(key, s);
    }
    setState(st);
    engine.start();
    return { setState: setState, engine: engine, get state() { return st; } };
  }

  /* ---------- 小工具 ---------- */
  function initMockThemeBtn() {
    initTheme();
    var b = document.getElementById('themeBtn');
    if (b) b.addEventListener('click', toggleTheme);
  }
  function setRange(el) {
    function upd() { el.style.setProperty('--v', el.value + '%'); }
    el.addEventListener('input', upd); upd();
  }
  function pickStateFromQuery(def) {
    var q = new URLSearchParams(location.search).get('state');
    return (q && STATES[q]) ? q : (def || 'idle');
  }
  function pickQuery(name, def) {
    var q = new URLSearchParams(location.search).get(name);
    return q == null ? def : q;
  }

  return {
    ICON: ICON, STATES: STATES, PAL: PAL,
    initTheme: initTheme, toggleTheme: toggleTheme, initMockThemeBtn: initMockThemeBtn,
    mount: mount, setRange: setRange, pickStateFromQuery: pickStateFromQuery, pickQuery: pickQuery
  };
})();