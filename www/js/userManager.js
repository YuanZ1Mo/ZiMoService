/**
 * ZiMo Service — 用户管理模块(门户工作区)
 * UI 模板:modules/userManager.html(懒加载注入);由门户壳 render() 调用 window.renderUserManager(entry)
 * 从 portal.js 拆分(原 renderUsers),逻辑未改动;escapeHtml/showModal 来自共享层 ZmAuth
 */
(function () {
  'use strict';
  const A = window.ZmAuth;
  // 共享层工具(原 portal.js 内部实现,拆分后归 ZmAuth)
  const escapeHtml = A.escapeHtml;
  const showModal = A.showModal;

  const ROLE_LV = { developer: 3, admin: 2, user: 1 };
  const ROLE_NAMES = { developer: '开发者', admin: '管理员', user: '用户' };

  /** @param {object} ctx 门户上下文: { user } (由壳注入) */
  window.renderUserManager = async function (entry, ctx) {
    const user = ctx.user;
    const st = { keyword: '', role: '', status: 0, page: 1, pageSize: 20, total: 0, list: [], opRole: user.role };

    // 模块 UI 模板独立文件(modules/userManager.html),懒加载注入
    entry.el.innerHTML = await fetch('/modules/userManager.html').then((r) => r.text());

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
})();
