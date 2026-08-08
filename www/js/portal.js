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
    localStorage.setItem('portalRedirect', location.pathname);
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
          <h3 class="ws-section-title">最近动态</h3>
          <div class="ws-lines">
            <div class="ws-line">· 门户框架上线(顶栏 / 功能目录 / 工作区)</div>
            <div class="ws-line">· 用户系统上线(登录 / 注册 / 找回密码 / 会话管理)</div>
            <div class="ws-line">· 权限模型就绪(管理员 / 用户 / 游客,模块授权)</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">我的模块</h3>
          <div class="ws-lines">
            ${modules.map((m) => `· ${escapeHtml(m.name)} — ${escapeHtml(m.url)}`).join('')}
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">快速开始</h3>
          <div class="ws-lines">
            <div class="ws-line">· 通过左侧功能目录切换模块,工作区互不干扰</div>
            <div class="ws-line">· 目录支持收起/展开,状态自动记忆</div>
            <div class="ws-line">· 工作区滚动位置在切换后自动保留</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">关于</h3>
          <div class="ws-lines">
            <div class="ws-line">· ZiMo Service — 子墨服务门户</div>
            <div class="ws-line">· 更多功能模块正在建设中,敬请期待</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">服务信息</h3>
          <div class="ws-lines ws-mono">
            <div class="ws-line">REST API     https://${location.hostname}:39441/zimo/api</div>
            <div class="ws-line">JRPC        https://${location.hostname}:39440/zimo/jrpc</div>
            <div class="ws-line">静态服务    https://${location.hostname}/</div>
            <div class="ws-line">广播端口    39640</div>
            <div class="ws-line">SOCKS5      39540</div>
            <div class="ws-line">TLS         自签证书(开发环境)</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">会话与安全</h3>
          <div class="ws-lines">
            <div class="ws-line">· 会话有效期:30 天滑动,90 天绝对上限</div>
            <div class="ws-line">· 每账号最多 5 个活跃会话,超出自动踢除最久未活动者</div>
            <div class="ws-line">· 登录失败锁定:同 IP 连续失败阶梯锁定</div>
            <div class="ws-line">· 密码存储:PBKDF2-HMAC-SHA256 + 随机盐</div>
            <div class="ws-line">· 救援码:找回密码的唯一凭证,请妥善保存</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">使用技巧</h3>
          <div class="ws-lines">
            <div class="ws-line">· 顶栏状态灯:绿色在线 / 琥珀色连接中断,断网不影响已打开的页面</div>
            <div class="ws-line">· 目录支持收起展开,状态自动记忆;移动端为抽屉样式</div>
            <div class="ws-line">· 工作区滚动位置在模块切换后自动保留</div>
            <div class="ws-line">· 刷新页面停留在当前模块(History 路由)</div>
            <div class="ws-line">· 浏览器建议使用 Chrome / Edge 最新版</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">开发计划</h3>
          <div class="ws-lines">
            <div class="ws-line">· 文件中心:目录管理 / 上传下载 / 扫码分享</div>
            <div class="ws-line">· 服务器音频传输:远程音频采集与播放</div>
            <div class="ws-line">· 用户管理:账号停用 / 启用 / 删除 / 重置密码 / 模块授权</div>
            <div class="ws-line">· 个人中心 / 账户设置</div>
            <div class="ws-line">· 云平台部署:域名 + 正式证书 + 自动续期</div>
          </div>
        </section>
        <section class="ws-section">
          <h3 class="ws-section-title">更新日志</h3>
          <div class="ws-lines">
            <div class="ws-line">· v1.0(2026-08-09):门户框架上线,权限模型就绪</div>
            <div class="ws-line">· v0.9(2026-08-08):用户系统上线(登录/注册/找回密码/会话管理)</div>
            <div class="ws-line">· v0.8:隧道 / 广播 / 文件中心服务端能力</div>
            <div class="ws-line">· v0.7:远程音频传输能力</div>
            <div class="ws-line">· v0.6:DeepSeek 余额查询</div>
          </div>
        </section>
      </div>`;
  }

  function renderPlaceholder(entry, mod) {
    if (mod.code === 'users') {
      renderUsers(entry);   // 用户管理已实现
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
      mask.addEventListener('click', (e) => {
        if (e.target === mask) {
          mask.remove();
          resolve({ action: 'cancel', value: null, values: [] });
        }
      });
      document.body.appendChild(mask);
      const input = mask.querySelector('[data-modal-input]');
      if (input) input.focus();
    });
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
