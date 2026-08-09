/**
 * ZiMo Service — 登录页(login.html)
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  const $ = (id) => document.getElementById(id);
  const accountEl = $('account');
  const passwordEl = $('password');
  const formErr = $('formErr');
  const submitBtn = $('submitBtn');
  const beacon = $('beacon');

  function fieldError(id, msg) {
    $(id + 'Err').textContent = msg;
    $(id + 'Field').classList.toggle('invalid', !!msg);
  }

  function clearFieldError(id) {
    fieldError(id, '');
  }

  function showFormErr(msg) {
    formErr.textContent = msg;
    formErr.hidden = !msg;
  }

  /* 账号实时转小写 + 清除错误 */
  accountEl.addEventListener('input', () => {
    accountEl.value = accountEl.value.toLowerCase();
    clearFieldError('account');
    showFormErr('');
  });
  passwordEl.addEventListener('input', () => {
    clearFieldError('password');
    showFormErr('');
  });

  /* 输入时信标呼吸,停止输入 1.5s 后回到空闲 */
  let idleTimer = null;
  function inputBeacon() {
    A.setBeacon(beacon, 'breathe');
    clearTimeout(idleTimer);
    idleTimer = setTimeout(() => A.setBeacon(beacon, 'idle'), 1500);
  }
  [accountEl, passwordEl].forEach((el) =>
    el.addEventListener('input', inputBeacon));

  /* 跳转提示(如个人分享需登录):302 带 hint 参数时显示 */
  const hint = new URLSearchParams(location.search).get('hint');
  if (hint) showFormErr(decodeURIComponent(hint));

  /* 回跳处理(分享场景:触发下载后进门户;普通场景:跳 redirect 或主页) */
  function doRedirect() {
    const redirect = new URLSearchParams(location.search).get('redirect') || '';
    if (redirect.startsWith('/share/')) {
      const a = document.createElement('a');
      a.href = redirect;
      document.body.appendChild(a);
      a.click();
      a.remove();
      window.location.href = '/portal';
      return;
    }
    window.location.href = (redirect.startsWith('/') && !redirect.startsWith('//')) ? redirect : '/';
  }

  /* 已登录会话:直接跳转(不再显示登录表单;未登录/网络异常则正常显示) */
  (async function checkAuthed() {
    try {
      await A.api.me();
      doRedirect();
    } catch (e) {
      /* 未登录(401)或网络异常:保持登录页 */
    }
  })();

  /* 登录失败错误映射:401/429 → 顶部;400 → 字段;网络错误 → 顶部兜底 */
  function mapError(e) {
    if (e.code === 401 || e.code === 429) {
      showFormErr(e.message);
      return;
    }
    if (e.code === 400) {
      if (e.message.includes('账号')) fieldError('account', e.message);
      else if (e.message.includes('密码')) fieldError('password', e.message);
      else showFormErr(e.message);
      return;
    }
    showFormErr('网络连接失败,请重试');
  }

  $('loginForm').addEventListener('submit', async (ev) => {
    ev.preventDefault();

    const account = accountEl.value.toLowerCase();
    const password = passwordEl.value;

    const ae = A.validateAccount(account);
    const pe = A.validatePassword(password);
    if (ae || pe) {
      if (ae) fieldError('account', ae);
      if (pe) fieldError('password', pe);
      return;
    }

    submitBtn.disabled = true;
    A.setBeacon(beacon, 'pulse');
    try {
      const r = await A.api.login(account, password);
      A.setBeacon(beacon, 'ok');
      // 强制重置(管理员重置过密码):先设置新密码与新救援码
      if (r.forceChange) {
        A.toast('请先强制重置密码');
        window.location.href = '/force-reset';
        return;
      }
      // 回跳:仅允许站内相对路径(防开放重定向);分享场景触发下载后进门户
      doRedirect();
    } catch (e) {
      A.setBeacon(beacon, 'err');
      mapError(e);
      submitBtn.disabled = false;
    }
  });
})();
