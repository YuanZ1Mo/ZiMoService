/**
 * ZiMo Service — 墨聚成印 粒子仪式(主页主体特效)
 *
 * 入场序列:墨色粒子从四周汇聚(螺旋) → 凝聚成朱砂方印 → 爆开粒子波环
 * 常驻态:  朱砂圆点呼吸 + 光尘粒子椭圆轨道环绕 + 鼠标视差
 * 降级:    prefers-reduced-motion 时不启动,页面显示 CSS 静态圆点
 */
(function () {
  'use strict';

  const canvas = document.getElementById('inkCanvas');
  if (!canvas) return;
  if (window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;

  const ctx = canvas.getContext('2d');
  const DPR = window.devicePixelRatio || 1;
  const AMBIENT_FPS_INTERVAL = 33;   // 常驻态 30fps(慢速环绕,肉眼无差别)

  const GATHER_COUNT = 480;   // 入场汇聚粒子数
  const DUST_COUNT = 140;     // 常驻光尘粒子数

  // 阶段时间线(ms);光尘在爆开尾声提前渐入,与方印飞散重叠过渡
  const T_GATHER = 1400;
  const T_SEAL = 2000;
  const T_BURST = 2700;
  const T_AMBIENT_FADE = 400;   // 爆开结束前 400ms 光尘开始渐入

  // 配色(与 style.css token 一致);一阶段/方印保持原墨色+朱砂,三阶段光尘另行混色
  const INK_COLOR = [233, 236, 240];   // 墨色
  const CINNABAR = [192, 86, 46];      // 朱砂
  const GOLD = [232, 174, 96];         // 暖金
  const AMBER = [246, 190, 112];       // 琥珀
  /** 光尘粒子(三阶段):墨色/朱砂/暖金/琥珀 混合 */
  const DUST_PALETTE = [INK_COLOR, INK_COLOR, CINNABAR, GOLD, AMBER];

  let W = 0, H = 0, cx = 0, cy = 0;
  let sealSize = 72;          // 方印尺寸(随留白区域缩放)
  let auraRadius = 34;        // 中心光晕半径
  let dustRxMax = 200;        // 光尘轨道横向最大半径
  let dustRyMax = 150;        // 光尘轨道纵向最大半径
  let particles = [];
  let ring = null;              // 爆开波环
  let startedAt = 0;
  let sealAssigned = false;
  let burstDone = false;
  let ambientDone = false;
  let raf = 0;
  let running = false;
  let lastDrawAt = 0;    // 常驻态降帧用
  let lastFrameAt = 0;   // 真实帧间隔计算用
  const mouse = { x: 0, y: 0, tx: 0, ty: 0 };

  /* ========================================================================
     尺寸与视差
     ======================================================================== */

  function resize() {
    const parent = canvas.parentElement;
    const rect = parent.getBoundingClientRect();
    W = Math.max(rect.width, 1);
    H = Math.max(rect.height, 1);
    canvas.width = Math.round(W * DPR);
    canvas.height = Math.round(H * DPR);
    canvas.style.width = W + 'px';
    canvas.style.height = H + 'px';
    ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
    cx = W / 2;
    cy = H / 2;
    // 特效尺寸随留白区域缩放,尽量铺满
    sealSize = Math.round(Math.min(W, H) * 0.30);
    auraRadius = Math.round(Math.min(W, H) * 0.30);
    dustRxMax = W * 0.46;
    dustRyMax = H * 0.44;
  }

  window.addEventListener('resize', resize);

  /* ========================================================================
     粒子结构
     ======================================================================== */

  function makeParticle(x, y, tx, ty, size, alpha, color) {
    return {
      x, y, tx, ty,
      size,
      alpha, targetAlpha: alpha,
      color: color.slice(), targetColor: color.slice(),
      vx: 0, vy: 0,
      ang: 0, rx: 0, ry: 0, speed: 0, wobble: 0,   // 光尘轨道参数
      fading: false,
    };
  }

  /** 入场:墨色粒子从四周生成,目标 = 中心附近散开 */
  function spawnGather() {
    particles = [];
    for (let i = 0; i < GATHER_COUNT; i++) {
      const edge = Math.floor(Math.random() * 4);
      let x, y;
      if (edge === 0)      { x = Math.random() * W; y = -12; }
      else if (edge === 1) { x = Math.random() * W; y = H + 12; }
      else if (edge === 2) { x = -12; y = Math.random() * H; }
      else                 { x = W + 12; y = Math.random() * H; }
      // 汇聚目标:中心附近较大范围散开(汇聚感大气)
      const spread = Math.max(48, sealSize * 0.22);
      const p = makeParticle(x, y,
        cx + (Math.random() - 0.5) * spread * 2,
        cy + (Math.random() - 0.5) * spread * 2,
        1 + Math.random() * 2,
        0.12 + Math.random() * 0.15,
        INK_COLOR);
      p.t = Math.random() * 0.5;   // 到达时间偏移(错落感)
      particles.push(p);
    }
  }

  /** 成印:粒子目标重排到方印轮廓与内部,颜色转朱砂 */
  function assignSealTargets() {
    const half = sealSize / 2;
    const edgeCount = Math.floor(particles.length * 0.4);
    for (let i = 0; i < particles.length; i++) {
      const p = particles[i];
      if (i < edgeCount) {
        const edge = Math.floor(Math.random() * 4);
        if (edge === 0)      { p.tx = cx + (Math.random() * 2 - 1) * half; p.ty = cy - half; }
        else if (edge === 1) { p.tx = cx + (Math.random() * 2 - 1) * half; p.ty = cy + half; }
        else if (edge === 2) { p.tx = cx - half; p.ty = cy + (Math.random() * 2 - 1) * half; }
        else                 { p.tx = cx + half; p.ty = cy + (Math.random() * 2 - 1) * half; }
      } else {
        p.tx = cx + (Math.random() * 2 - 1) * (half - 5);
        p.ty = cy + (Math.random() * 2 - 1) * (half - 5);
      }
      p.targetColor = CINNABAR;
      p.targetAlpha = 0.55 + Math.random() * 0.25;
    }
  }

  /** 爆开:粒子径向飞散 + 波环 */
  function burst() {
    for (const p of particles) {
      const ang = Math.atan2(p.y - cy, p.x - cx) + (Math.random() - 0.5) * 0.6;
      const sp = 3 + Math.random() * 5;
      p.vx = Math.cos(ang) * sp;
      p.vy = Math.sin(ang) * sp;
      p.fading = true;
    }
    ring = { r: 8, alpha: 0.75 };
  }

  /** 常驻:光尘粒子(椭圆轨道环绕,少量朱砂点缀);追加而非替换,与方印飞散重叠过渡,alpha 从 0 渐入 */
  function spawnDust() {
    for (let i = 0; i < DUST_COUNT; i++) {
      const p = makeParticle(0, 0, 0, 0,
        0.6 + Math.random() * 1.5,
        0,   // alpha 从 0 渐入,与爆开余波无缝衔接
        DUST_PALETTE[Math.floor(Math.random() * DUST_PALETTE.length)]);
      p.targetAlpha = 0.20 + Math.random() * 0.25;   // 提亮
      p.ang = Math.random() * Math.PI * 2;
      p.rx = dustRxMax * (0.2 + Math.random() * 0.8);
      p.ry = dustRyMax * (0.2 + Math.random() * 0.8);
      p.speed = (0.00035 + Math.random() * 0.00075) * 60;   // 弧度/秒(帧率无关)
      p.wobble = Math.random() * Math.PI * 2;
      p.wobbleSpeed = 0.0012 * 60;   // 弧度/秒
      particles.push(p);
    }
  }

  /* ========================================================================
     帧更新
     ======================================================================== */

  function updatePhase(elapsed, dtMs) {
    if (elapsed < T_GATHER) {
      // 汇聚:向中心缓动 + 轻微螺旋
      for (const p of particles) {
        const dx = p.tx - p.x;
        const dy = p.ty - p.y;
        p.x += dx * 0.055 + (p.y - cy) * 0.0022;
        p.y += dy * 0.055 - (p.x - cx) * 0.0022;
      }
    } else if (elapsed < T_SEAL) {
      if (!sealAssigned) { assignSealTargets(); sealAssigned = true; }
      for (const p of particles) {
        p.x += (p.tx - p.x) * 0.09;
        p.y += (p.ty - p.y) * 0.09;
        lerpColor(p, 0.15);
      }
    } else if (elapsed < T_BURST - T_AMBIENT_FADE) {
      if (!burstDone) {
        burst();
        burstDone = true;
      }
      updateBurst();
    } else {
      if (!ambientDone) { spawnDust(); ambientDone = true; }
      updateBurst();   // 方印飞散余波继续
      updateDust(dtMs);    // 光尘同时渐入,重叠过渡
    }
  }

  function updateBurst() {
    for (let i = particles.length - 1; i >= 0; i--) {
      const p = particles[i];
      if (!p.fading) continue;   // 只处理飞散粒子(光尘不在此列)
      p.x += p.vx;
      p.y += p.vy;
      p.vx *= 0.955;
      p.vy *= 0.955;
      p.alpha -= 0.018;
      if (p.alpha <= 0.02) particles.splice(i, 1);
    }
    if (ring) {
      ring.r += Math.max(7, Math.min(W, H) * 0.014);   // 波环扩散速度随区域缩放
      ring.alpha -= 0.016;
      if (ring.alpha <= 0) ring = null;
    }
  }

  function updateDust(dtMs) {
    mouse.x += (mouse.tx - mouse.x) * 0.08;   // 视差缓动
    mouse.y += (mouse.ty - mouse.y) * 0.08;
    const ofsX = mouse.x * 10;
    const ofsY = mouse.y * 8;
    const dt = dtMs / 1000;   // 秒;角速度按时间缩放,帧率无关
    for (const p of particles) {
      if (!p.rx) continue;   // 只处理光尘粒子(飞散粒子不在此列)
      p.alpha += (p.targetAlpha - p.alpha) * 0.03;   // 光尘渐入
      p.ang += p.speed * dt;
      p.wobble += p.wobbleSpeed * dt;
      p.x = cx + ofsX + Math.cos(p.ang) * p.rx + Math.sin(p.wobble) * 4;
      p.y = cy + ofsY + Math.sin(p.ang) * p.ry + Math.cos(p.wobble) * 4;
    }
  }

  function lerpColor(p, k) {
    for (let i = 0; i < 3; i++) {
      p.color[i] += (p.targetColor[i] - p.color[i]) * k;
    }
    p.alpha += (p.targetAlpha - p.alpha) * k;
  }

  /* ========================================================================
     绘制
     ======================================================================== */

  function draw(elapsed) {
    ctx.clearRect(0, 0, W, H);

    const ofx = cx + mouse.x * 10;   // 视差偏移
    const ofy = cy + mouse.y * 8;

    // 粒子(含爆开后的文字粒子);光尘带闪烁
    for (const p of particles) {
      let a = p.alpha;
      if (p.rx) a *= 0.72 + 0.28 * Math.sin(p.wobble * 3);   // 光尘闪烁(提亮基线)
      if (a <= 0) continue;
      const [r, g, b] = p.color;
      ctx.fillStyle = `rgba(${r | 0}, ${g | 0}, ${b | 0}, ${a})`;
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
      ctx.fill();
    }

    // 爆开波环
    if (ring) {
      ctx.strokeStyle = `rgba(192, 86, 46, ${ring.alpha})`;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(cx, cy, ring.r, 0, Math.PI * 2);
      ctx.stroke();
    }
  }

  /* ========================================================================
     主循环
     ======================================================================== */

  function loop(now) {
    if (!running) return;
    const elapsed = now - startedAt;
    const dtMs = now - lastFrameAt;   // 真实帧间隔(含跳过绘制的帧),驱动时间相关动画
    lastFrameAt = now;
    // 常驻态降帧:30fps 足够,减半渲染开销
    if (elapsed >= T_BURST && now - lastDrawAt < AMBIENT_FPS_INTERVAL) {
      raf = requestAnimationFrame(loop);
      return;
    }
    try {
      updatePhase(elapsed, dtMs);
      draw(elapsed);
      lastDrawAt = now;
    } catch (e) {
      console.error('ink ceremony error:', e);   // 单帧异常不中断动画循环
    }
    raf = requestAnimationFrame(loop);
  }

  function onVisibility() {
    if (document.hidden) {
      if (running) {
        running = false;
        cancelAnimationFrame(raf);
      }
    } else if (!running) {
      running = true;
      raf = requestAnimationFrame(loop);
    }
  }

  /* ========================================================================
     启动
     ======================================================================== */

  window.ZmInk = {
    start() {
      if (running) return;
      resize();
      spawnGather();
      startedAt = performance.now();
      running = true;
      document.addEventListener('visibilitychange', onVisibility);
      canvas.parentElement.addEventListener('mousemove', (e) => {
        const rect = canvas.getBoundingClientRect();
        mouse.tx = ((e.clientX - rect.left) / rect.width - 0.5) * 2;
        mouse.ty = ((e.clientY - rect.top) / rect.height - 0.5) * 2;
      });
      raf = requestAnimationFrame(loop);
    },
  };
})();
