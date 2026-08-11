/**
 * ZiMo Service — 门户壳(portal.html)
 * 顶栏(状态/昵称下拉)/ 功能目录 / History 路由工作区 / 401 统一拦截 / 心跳
 *
 * 模块拆分后职责:
 *   - 壳:导航/路由/keep-alive 生命周期/懒加载
 *   - 模块 UI 模板:www/modules/<code>.html(独立文件,按需 fetch 注入)
 *   - 模块逻辑:home(壳内 renderHome)/serverAudioStream.js/userManager.js/filehub.js,
 *     统一契约 window['render'+Cap](entry, ctx),ctx = { user, modules }
 */
(function () {
  'use strict';
  const A = window.ZmAuth;
  const $ = (id) => document.getElementById(id);

  let user = null;
  let modules = [];
  let currentPath = null;
  /** 模块视图缓存:url → { el, mod, onActivate, onDeactivate, ready }
   *  容器常驻不销毁(keep-alive),切换仅隐藏/显示 —— 模块运行中状态天然保留;
   *  各容器独立滚动,滚动位置随容器保留。刷新后缓存清空(重新初始化)。
   *  模块可挂载生命周期钩子(框架约定):onActivate(进入/恢复)、onDeactivate(离开/暂停)。
   *  ready:首次渲染 Promise(模板懒加载完成),激活等待其就绪后回调 onActivate。 */
  const viewCache = {};

  const ICONS = { home: '⌂', filehub: '▤', serverAudioStream: '♫', userManager: '◈' };

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
        `<span class="side-icon">${ICONS[m.code] || '•'}</span><span class="side-name">${A.escapeHtml(m.name)}</span>`;
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

    // 进入目标模块:首次创建渲染(异步懒加载),之后复用缓存容器
    let entry = viewCache[path];
    if (!entry) {
      const el = document.createElement('div');
      el.className = 'ws-view';
      entry = { el, mod, onActivate: null, onDeactivate: null, ready: null };
      viewCache[path] = entry;
      $('workspace').appendChild(el);
      entry.ready = loadModule(entry, mod);
    }
    entry.el.classList.add('active');
    currentPath = path;
    updateActive();
    activate(entry, path);
  }

  /** 激活钩子:等待模块首次渲染就绪(模板注入完成)后回调 onActivate;
   *  加载期间用户已切走则跳过(下次进入自然触发) */
  async function activate(entry, path) {
    if (entry.ready) await entry.ready;
    if (currentPath !== path || !entry.el.classList.contains('active')) return;
    if (entry.onActivate) entry.onActivate();
  }

  /* ========================================================================
     模块加载:按 code 分发到 window['render'+Cap](entry, ctx)
     home 由壳直接渲染;serverAudioStream/userManager/filehub 为独立 JS 文件(portal.html 引入)
     ======================================================================== */

  async function loadModule(entry, mod) {
    try {
      const cap = mod.code.charAt(0).toUpperCase() + mod.code.slice(1);
      const renderFn = window['render' + cap];
      if (renderFn) {
        await renderFn(entry, { user, modules });
        return;
      }
    } catch (e) {
      console.error('[portal] 模块渲染失败:', mod.code, e);
    }
    entry.el.innerHTML = placeholderHtml(mod);
  }

  function placeholderHtml(mod) {
    return `
      <div class="ws-placeholder">
        <div class="ws-icon">${ICONS[mod.code] || '•'}</div>
        <div class="ws-title">${A.escapeHtml(mod.name)}</div>
        <div>建设中,敬请期待</div>
      </div>`;
  }

  /* ========================================================================
     主页工作区(模板:modules/home.html,动态值渲染)
     与 serverAudioStream/userManager/filehub 同契约:暴露 window.renderHome 供分发器调用
     ======================================================================== */

  window.renderHome = async function (entry) {
    const html = await fetch('/modules/home.html').then((r) => r.text());
    entry.el.innerHTML = html;
    entry.el.querySelector('#hmNick').textContent = user.nickname || user.account;
    entry.el.querySelector('#hmModules').textContent = modules.length;
    entry.el.querySelector('#hmRole').textContent = user.role;
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
