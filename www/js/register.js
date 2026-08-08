/**
 * ZiMo Service — 注册页(register.html)
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  const $ = (id) => document.getElementById(id);
  const accountEl = $('account');
  const passwordEl = $('password');
  const nicknameEl = $('nickname');
  const rescueEl = $('rescue');
  const formErr = $('formErr');
  const submitBtn = $('submitBtn');
  const beacon = $('beacon');
  const segs = document.querySelectorAll('.strength-seg');
  const strengthLabel = $('strengthLabel');

  const FIELD_IDS = ['account', 'password', 'nickname', 'rescue'];

  function fieldError(id, msg) {
    $(id + 'Err').textContent = msg;
    $(id + 'Field').classList.toggle('invalid', !!msg);
  }

  function showFormErr(msg) {
    formErr.textContent = msg;
    formErr.hidden = !msg;
  }

  function clearAll() {
    FIELD_IDS.forEach((id) => fieldError(id, ''));
    showFormErr('');
  }

  /* 账号实时小写;救援码实时小写 + 实时校验 */
  accountEl.addEventListener('input', () => {
    accountEl.value = accountEl.value.toLowerCase();
    fieldError('account', '');
    showFormErr('');
  });
  rescueEl.addEventListener('input', () => {
    rescueEl.value = rescueEl.value.toLowerCase();
    fieldError('rescue', A.validateRescue(rescueEl.value) || '');
    showFormErr('');
  });
  nicknameEl.addEventListener('input', () => {
    fieldError('nickname', '');
    showFormErr('');
  });
  passwordEl.addEventListener('input', () => {
    fieldError('password', '');
    showFormErr('');
    const score = A.passwordStrength(passwordEl.value).score;
    A.renderStrength(segs, strengthLabel, score);
  });

  /* 输入时信标呼吸,停止输入 1.5s 后回到空闲 */
  let idleTimer = null;
  function inputBeacon() {
    A.setBeacon(beacon, 'breathe');
    clearTimeout(idleTimer);
    idleTimer = setTimeout(() => A.setBeacon(beacon, 'idle'), 1500);
  }
  [accountEl, passwordEl, nicknameEl, rescueEl].forEach((el) =>
    el.addEventListener('input', inputBeacon));

  /* 注册失败错误映射:429 → 顶部;400 → 字段(关键词) */
  function mapError(e) {
    if (e.code === 429) {
      showFormErr(e.message);
      return;
    }
    if (e.code === 400) {
      if (e.message.includes('账号')) fieldError('account', e.message);
      else if (e.message.includes('密码')) fieldError('password', e.message);
      else if (e.message.includes('昵称')) fieldError('nickname', e.message);
      else if (e.message.includes('救援码')) fieldError('rescue', e.message);
      else showFormErr(e.message);
    } else {
      showFormErr(e.message);
    }
  }

  $('registerForm').addEventListener('submit', async (ev) => {
    ev.preventDefault();

    const account = accountEl.value.toLowerCase();
    const password = passwordEl.value;
    const nickname = nicknameEl.value;
    const rescue = rescueEl.value.toLowerCase();

    /* 全量校验:字段级 */
    const errors = {
      account: A.validateAccount(account),
      password: A.validatePassword(password),
      nickname: A.validateNickname(nickname),
      rescue: A.validateRescue(rescue),
    };
    let hasError = false;
    FIELD_IDS.forEach((id) => {
      if (errors[id]) {
        fieldError(id, errors[id]);
        hasError = true;
      }
    });

    /* zxcvbn:评分 < 2 阻止提交(弱密码) */
    if (!errors.password) {
      const score = A.passwordStrength(password).score;
      if (score < 2) {
        fieldError('password', '密码强度过低,请增加长度或混合字符类型');
        hasError = true;
      }
    }
    if (hasError) return;

    submitBtn.disabled = true;
    A.setBeacon(beacon, 'pulse');
    try {
      await A.api.register(account, password, nickname, rescue);
      A.setBeacon(beacon, 'ok');
      A.toast('注册成功,正在自动登录');
      setTimeout(() => { window.location.href = '/'; }, 1500);   // 延迟跳转,让提示可见
    } catch (e) {
      A.setBeacon(beacon, 'err');
      mapError(e);
      submitBtn.disabled = false;
    }
  });
})();
