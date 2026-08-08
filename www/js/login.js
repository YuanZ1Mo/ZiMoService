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
      // 回跳:仅允许站内相对路径(防开放重定向)
      const redirect = new URLSearchParams(location.search).get('redirect') || '';
      window.location.href = (redirect.startsWith('/') && !redirect.startsWith('//')) ? redirect : '/';
    } catch (e) {
      A.setBeacon(beacon, 'err');
      mapError(e);
      submitBtn.disabled = false;
    }
  });
})();
