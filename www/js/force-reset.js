/**
 * ZiMo Service — 强制改密页(force-reset.html)
 * 管理员重置密码后,用户首次登录强制跳转本页设置新密码与新救援码
 */
(function () {
  'use strict';
  const A = window.ZmAuth;
  const $ = (id) => document.getElementById(id);

  const beacon = $('beacon');
  const form = $('forceResetForm');
  const formErr = $('formErr');
  const submitBtn = $('submitBtn');
  const passwordEl = $('password');
  const rescueEl = $('rescue');
  const segs = document.querySelectorAll('.strength-seg');
  const strengthLabel = $('strengthLabel');

  function fieldError(id, msg) {
    $(id + 'Err').textContent = msg;
    $(id + 'Field').classList.toggle('invalid', !!msg);
  }

  function showFormErr(msg) {
    formErr.textContent = msg;
    formErr.hidden = !msg;
  }

  /* 会话检查:401 跳登录;非强制改密状态直接进门户 */
  (async function init() {
    try {
      const r = await A.api.me();
      if (!r.forceChange) {
        window.location.href = '/';   // 未触发改密,回主页
        return;
      }
    } catch (e) {
      if (e.code === 401) {
        window.location.href = '/login';
        return;
      }
      form.hidden = true;
      $('netError').hidden = false;
      $('retryBtn').addEventListener('click', () => window.location.reload());
      return;
    }
    form.hidden = false;
  })();

  passwordEl.addEventListener('input', () => {
    fieldError('password', '');
    showFormErr('');
    const score = A.passwordStrength(passwordEl.value).score;
    A.renderStrength(segs, strengthLabel, score);
    A.setBeacon(beacon, 'breathe');
  });
  rescueEl.addEventListener('input', () => {
    rescueEl.value = rescueEl.value.toLowerCase();
    fieldError('rescue', A.validateRescue(rescueEl.value) || '');
    showFormErr('');
    A.setBeacon(beacon, 'breathe');
  });

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const password = passwordEl.value;
    const rescue = rescueEl.value.toLowerCase();

    const pe = A.validatePassword(password);
    const re = A.validateRescue(rescue);
    if (pe || re) {
      if (pe) fieldError('password', pe);
      if (re) fieldError('rescue', re);
      return;
    }
    const score = A.passwordStrength(password).score;
    if (score < 2) {
      fieldError('password', '密码强度过低,请增加长度或混合字符类型');
      return;
    }

    submitBtn.disabled = true;
    A.setBeacon(beacon, 'pulse');
    try {
      await A.api.completeChange(password, rescue);
      A.setBeacon(beacon, 'ok');
      A.toast('设置完成');
      setTimeout(() => { window.location.href = '/'; }, 1200);   // 与正常登录一致,回主页
    } catch (e) {
      A.setBeacon(beacon, 'err');
      if (e.code === 401) {
        window.location.href = '/login';
        return;
      }
      if (e.code === 400) {
        if (e.message.includes('密码')) fieldError('password', e.message);
        else if (e.message.includes('救援码')) fieldError('rescue', e.message);
        else showFormErr(e.message);
      } else {
        showFormErr(e.message);
      }
      submitBtn.disabled = false;
    }
  });
})();
