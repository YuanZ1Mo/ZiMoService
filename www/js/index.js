/**
 * ZiMo Service — 主页(index.html)
 * 会话检查 → 用户面板渲染 → 心跳 → 登出
 */
(function () {
  'use strict';
  const A = window.ZmAuth;

  const $ = (id) => document.getElementById(id);
  const loadingView = $('loadingView');
  const netErrorView = $('netErrorView');
  const userPanel = $('userPanel');
  const connState = $('connState');
  const connText = $('connText');
  const logoutBtn = $('logoutBtn');

  /* ---- 会话失效:统一处理 ---- */
  function sessionExpired() {
    A.toast('会话已失效,请重新登录', 'err');
    setTimeout(() => { window.location.href = '/login'; }, 1000);
  }

  function serverFail() {
    A.toast('连接异常,请重新登录', 'err');
    setTimeout(() => { window.location.href = '/login'; }, 1000);
  }

  /* ---- 用户面板渲染 ---- */
  function renderPanel(user) {
    $('userNickname').textContent = user.nickname || '—';
    $('userAccount').textContent = user.account || '—';
    $('userIp').textContent = user.lastLoginIp || '—';
    $('userLoginTime').textContent = A.formatTime(user.lastLoginTime);
    $('userActiveTime').textContent = A.formatTime(user.lastActiveTime);
  }

  function showOnline() {
    connState.classList.remove('warn', 'err');
    connText.textContent = '在线';
  }

  function showNetDown() {
    connState.classList.add('warn');
    connState.classList.remove('err');
    connText.textContent = '连接中断';
  }

  /* ---- 登出 ---- */
  logoutBtn.addEventListener('click', async () => {
    logoutBtn.disabled = true;
    try {
      await A.api.logout();
      window.location.href = '/login';
    } catch (e) {
      logoutBtn.disabled = false;
      if (e.code === 401) {
        window.location.href = '/login';   // 会话本已失效,直接跳转
      } else {
        A.toast('登出失败,请检查网络', 'err');
      }
    }
  });

  /* ---- 进入门户 ---- */
  $('portalBtn').addEventListener('click', () => {
    window.location.href = '/portal';
  });

  /* ---- 会话检查(三态分流) ---- */
  A.checkSession({
    onAuthed(user) {
      loadingView.hidden = true;
      renderPanel(user);
      userPanel.hidden = false;
      showOnline();
      // 墨聚成印 粒子仪式(Canvas;reduced-motion 时 ink.js 不启动,显示静态圆点)
      $('inkStage').hidden = false;
      if (window.ZmInk) window.ZmInk.start();

      let netDownShown = false;   // 网络中断提示去重:仅状态变化时提示一次
      A.startHeartbeat({
        onExpired: sessionExpired,
        onServerFail: serverFail,
        onNetDown() {
          showNetDown();
          if (!netDownShown) {
            netDownShown = true;
            A.toast('网络连接中断');
          }
        },
        onNetUp() {
          showOnline();
          netDownShown = false;
          A.toast('网络已恢复');
        },
      });
    },
    onExpired: sessionExpired,
    onNetError() {
      loadingView.hidden = true;
      netErrorView.hidden = false;
    },
  });

  /* ---- 重试 ---- */
  $('retryBtn').addEventListener('click', () => {
    netErrorView.hidden = true;
    loadingView.hidden = false;
    window.location.reload();
  });
})();
