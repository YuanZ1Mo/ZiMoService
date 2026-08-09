/**
 * ZiMo Service — 前端共享层(用户系统)
 * API 封装 / 心跳 / 会话检查 / 表单校验 / 信标 / toast
 */
(function () {
  'use strict';

  /** RESTful API 端点 */
  const REST_URL = `//${window.location.hostname}:39441/zimo/api`;

  /**
   * 调用 RESTful API
   * @returns {Promise<object>} 成功返回 result 对象
   * @throws {Error} err.code = HTTP 状态码(网络错误为 0), err.message = 服务端文案, err.network = 网络层失败
   */
  async function restCall(method, path, body = null) {
    // 页面(443)与 REST API(39441)跨端口同站,须 include 才能携带会话 cookie
    const opts = { method, credentials: 'include' };
    if (body !== null) {
      opts.headers = { 'Content-Type': 'application/json' };
      opts.body = JSON.stringify(body);
    }
    let r;
    try {
      r = await fetch(REST_URL + path, opts);
    } catch (e) {
      const err = new Error('网络连接失败');
      err.code = 0;
      err.network = true;
      throw err;
    }
    let json = null;
    try {
      json = await r.json();
    } catch (e) { /* 非 JSON 响应 */ }
    if (json && json.error) {
      const err = new Error(json.error.message || '请求失败');
      err.code = json.error.code || r.status;
      err.network = false;
      throw err;
    }
    if (!r.ok || !json) {
      const err = new Error(`请求失败(${r.status})`);
      err.code = r.status;
      err.network = false;
      throw err;
    }
    return json.result;
  }

  /** 认证 API */
  const api = {
    login: (account, password) => restCall('POST', '/auth/login', { account, password }),
    register: (account, password, nickname, rescue) =>
      restCall('POST', '/auth/register', { account, password, nickname, rescue }),
    reset: (account, password, rescue) =>
      restCall('POST', '/auth/reset', { account, password, rescue }),
    logout: () => restCall('POST', '/auth/logout'),
    me: () => restCall('GET', '/auth/me'),
    heartbeat: () => restCall('GET', '/auth/heartbeat'),
    portalInfo: () => restCall('GET', '/portal/info'),
    completeChange: (password, rescue) =>
      restCall('POST', '/auth/complete-change', { password, rescue }),
    userList: (keyword, role, status, page, pageSize) =>
      restCall('GET', `/portal/users?keyword=${encodeURIComponent(keyword)}&role=${encodeURIComponent(role)}&status=${status}&page=${page}&pageSize=${pageSize}`),
    audioStatus: () => restCall('GET', '/portal/audio/status'),
    /** 音频流端点(流式,不经 restCall):需 credentials include 携带会话 */
    audioStreamUrl: () => `//${window.location.hostname}:39441/zimo/api/portal/audio/stream`,
    userDetail: (id) => restCall('GET', `/portal/users/${id}`),
    userAction: (id, action, body = {}) =>
      restCall('POST', `/portal/users/${id}/${action}`, body),
    // ── 文件中心 ────────────────────────────────────────────────
    filehubList: (space, dirId = 0, sort = 'name', order = 'asc') =>
      restCall('GET', `/portal/filehub/list?space=${space}&dir_id=${dirId}&sort=${sort}&order=${order}`),
    filehubSearch: (space, keyword) =>
      restCall('GET', `/portal/filehub/search?space=${space}&keyword=${encodeURIComponent(keyword)}`),
    filehubMkdir: (space, parentId, name) =>
      restCall('POST', `/portal/filehub/mkdir?space=${space}`, { parent_id: parentId, name }),
    filehubRename: (space, type, id, newName) =>
      restCall('POST', `/portal/filehub/rename?space=${space}`, { type, id, new_name: newName }),
    filehubMove: (space, ids, targetDirId) =>
      restCall('POST', `/portal/filehub/move?space=${space}`, { ids, target_dir_id: targetDirId }),
    filehubCopy: (space, ids, targetDirId, targetSpace) =>
      restCall('POST', `/portal/filehub/copy?space=${space}`, { ids, target_dir_id: targetDirId, target_space: targetSpace }),
    filehubDelete: (space, ids) =>
      restCall('POST', `/portal/filehub/delete?space=${space}`, { ids }),
    filehubShare: (type, id) =>
      restCall('POST', '/portal/filehub/share', { type, id }),
    filehubUnshare: (shareId) =>
      restCall('POST', '/portal/filehub/unshare', { share_id: shareId }),
    filehubShares: () => restCall('GET', '/portal/filehub/shares'),
    /** 单文件下载 URL(界面内已登录场景) */
    filehubDownloadUrl: (space, fileId) =>
      `//${window.location.hostname}:39441/zimo/api/portal/filehub/download?space=${space}&file_id=${fileId}`,
    /** 单文件下载(fetch blob → 触发下载;用于队列跟踪,失去浏览器原生下载器接管) */
    async filehubDownload(space, fileId) {
      const r = await fetch(REST_URL + `/portal/filehub/download?space=${space}&file_id=${fileId}`, {
        method: 'GET',
        credentials: 'include',
      });
      if (!r.ok) {
        let msg = `请求失败(${r.status})`;
        try {
          const j = await r.json();
          if (j.error) msg = j.error.message || msg;
        } catch (e) { /* 非 JSON */ }
        const err = new Error(msg);
        err.code = r.status;
        throw err;
      }
      const blob = await r.blob();
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = 'file';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
    },
    /** 分享链接(页面端口,免登录可达公共分享) */
    filehubShareUrl: (token) => `//${window.location.hostname}/share/${token}`,
    /** zip 打包下载(多选/文件夹):流式 fetch → blob → 触发下载
     *  @param fallbackName 本地构造的下载名(跨源 fetch 拿不到 Content-Disposition 头,
     *                     需 CORS Expose-Headers;前端用列表数据构造兜底) */
    async filehubZip(space, ids, fallbackName) {
      // 超时保护:打包可能较慢,120s 无响应强制中止,队列标记失败继续下一个
      const ctrl = new AbortController();
      const zipTimer = setTimeout(() => ctrl.abort(), 120000);
      let r;
      try {
        r = await fetch(REST_URL + `/portal/filehub/zip?space=${space}`, {
          method: 'POST',
          credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ ids }),
          signal: ctrl.signal,
        });
      } catch (e) {
        clearTimeout(zipTimer);
        if (e.name === 'AbortError') {
          const err = new Error('打包超时');
          err.code = 0;
          throw err;
        }
        throw e;
      }
      clearTimeout(zipTimer);
      if (!r.ok) {
        let msg = `请求失败(${r.status})`;
        try {
          const j = await r.json();
          if (j.error) msg = j.error.message || msg;
        } catch (e) { /* 非 JSON */ }
        const err = new Error(msg);
        err.code = r.status;
        throw err;
      }
      const blob = await r.blob();
      const disp = r.headers.get('Content-Disposition') || '';
      // 优先 RFC 5987 filename*(UTF-8 百分号编码,中文名可靠);
      // 拿不到头(跨源 fetch 不暴露)时用本地构造的 fallbackName 兜底
      let name = fallbackName || 'filehub.zip';
      const star = disp.match(/filename\*=UTF-8''([^;]+)/i);
      if (star) {
        try { name = decodeURIComponent(star[1]); } catch (e) { name = star[1]; }
      } else {
        const m = disp.match(/filename="?([^";]+)"?/i);
        if (m) name = m[1];
      }
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = name;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
      return { name };
    },
    /** 上传单个文件(流式;X-File-Size 声明供服务端完整性校验) */
    async filehubUpload(space, dirId, name, file, onProgress) {
      return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        // 超时保护:任何文件挂起(不触发 onload/onerror)时强制中止并报错,
        // 保证队列链不被单个卡住的文件阻断。
        // 超时按文件大小缩放:小文件(含 0 字节)10s 内必须收尾——否则立即失败
        // 可见,队列继续;大文件按带宽估 60s 封顶。
        const timeoutMs = Math.max(10000, Math.min(60000, Math.ceil(file.size / 2048)));
        const timer = setTimeout(() => {
          try { xhr.abort(); } catch (e) { /* 忽略 */ }
          const err = new Error('上传超时');
          err.code = 0;
          reject(err);
        }, timeoutMs);
        xhr.open('POST', REST_URL + `/portal/filehub/upload?space=${space}&dir_id=${dirId}&name=${encodeURIComponent(name)}`);
        xhr.withCredentials = true;
        xhr.setRequestHeader('X-File-Size', String(file.size));
        xhr.upload.onprogress = (e) => {
          if (onProgress && e.lengthComputable) onProgress(e.loaded, e.total);
        };
        xhr.onload = () => {
          clearTimeout(timer);
          if (xhr.status >= 200 && xhr.status < 300) {
            try { resolve(JSON.parse(xhr.responseText).result); }
            catch (e) { reject(new Error('响应解析失败')); }
          } else {
            let msg = `请求失败(${xhr.status})`;
            try {
              const j = JSON.parse(xhr.responseText);
              if (j.error) msg = j.error.message || msg;
            } catch (e) { /* 非 JSON */ }
            const err = new Error(msg);
            err.code = xhr.status;
            reject(err);
          }
        };
        xhr.onerror = () => {
          clearTimeout(timer);
          const err = new Error('网络连接失败');
          err.code = 0;
          err.network = true;
          reject(err);
        };
        // 0 字节文件:send(file) 时 Chrome 不触发 onload/onerror(空 body 怪癖),
        // promise 永不 settle → 队列卡死、后续任务中断;改 send 空串
        xhr.send(file.size ? file : '');
      });
    },
  };

  /** unix 秒 → 本地 "YYYY-MM-DD HH:mm:ss" */
  function formatTime(unixSec) {
    if (!unixSec) return '—';
    const d = new Date(unixSec * 1000);
    const p = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  /* ========================================================================
     Toast
     ======================================================================== */

  let toastWrap = null;

  function ensureToastWrap() {
    if (!toastWrap) {
      toastWrap = document.createElement('div');
      toastWrap.className = 'toast-wrap';
      document.body.appendChild(toastWrap);
    }
    return toastWrap;
  }

  /**
   * @param {string} msg 文案
   * @param {string} [type] 'err' 错误 3.5s 自动消失,其余 2.5s —— toast 一律临时提示,
   *                          持久性错误用页面内联错误条展示,不用 toast 常驻挡界面
   */
  function toast(msg, type) {
    const el = document.createElement('div');
    el.className = 'toast' + (type === 'err' ? ' err' : '');
    el.textContent = msg;
    ensureToastWrap().appendChild(el);
    const duration = type === 'err' ? 3500 : 2500;
    setTimeout(() => {
      el.style.transition = 'opacity .3s';
      el.style.opacity = '0';
      setTimeout(() => el.remove(), 300);
    }, duration);
  }

  /* ========================================================================
     脉冲信标
     ======================================================================== */

  const BEACON_STATES = {
    idle: 'beacon-idle',       // 空闲:暗
    breathe: 'beacon-breathe', // 输入:慢呼吸
    pulse: 'beacon-pulse',     // 提交:脉冲
    ok: 'beacon-ok',           // 成功:常亮
    err: 'beacon-err',         // 失败:红闪
    warn: 'beacon-warn',       // 连接中断
  };

  function setBeacon(el, state) {
    if (!el) return;
    el.className = 'beacon';
    if (BEACON_STATES[state]) el.classList.add(BEACON_STATES[state]);
  }

  /* ========================================================================
     会话检查与心跳
     ======================================================================== */

  /**
   * 页面加载会话检查(三态分流)
   * @param {object} opts
   *   onAuthed(user)      会话有效
   *   onExpired()         401:会话失效(弹提示后跳登录)
   *   onNetError()        网络失败(显示重试)
   */
  async function checkSession(opts) {
    try {
      const result = await api.me();
      opts.onAuthed && opts.onAuthed(result.user);
    } catch (e) {
      if (e.code === 401) {
        opts.onExpired && opts.onExpired();
      } else {
        opts.onNetError && opts.onNetError(e);
      }
    }
  }

  /**
   * 心跳管理:每 60s 一次
   * - 401 → onExpired(立即判定会话失效)
   * - 其他服务端错误响应 → onServerFail(1 次即强制下线)
   * - 网络错误 → onNetDown(仅提示连接中断,不计失败不下线;恢复后 onNetUp)
   * - 页面隐藏暂停,恢复可见立即补一次
   */
  function startHeartbeat(opts) {
    const HBEAT_INTERVAL = 60 * 1000;
    let timer = null;
    let netDown = false;

    async function tick() {
      try {
        await api.heartbeat();
        if (netDown) {
          netDown = false;
          opts.onNetUp && opts.onNetUp();
        }
        schedule();
      } catch (e) {
        if (e.code === 401) {
          opts.onExpired && opts.onExpired();   // 立即,不再调度
        } else if (e.network) {
          netDown = true;
          opts.onNetDown && opts.onNetDown();
          schedule();                           // 网络错误:继续重试,不下线
        } else {
          opts.onServerFail && opts.onServerFail();  // 服务端失败:1 次即下线,不再调度
        }
      }
    }

    function schedule() {
      timer = setTimeout(tick, HBEAT_INTERVAL);
    }

    function onVisibility() {
      if (document.hidden) {
        if (timer) { clearTimeout(timer); timer = null; }
      } else if (!timer) {
        tick();   // 恢复可见:立即补一次心跳
      }
    }

    document.addEventListener('visibilitychange', onVisibility);
    tick();
  }

  /* ========================================================================
     表单校验(与服务端规则一致;码点计数 = [...str].length)
     ======================================================================== */

  const ACCOUNT_RE = /^[a-z0-9_-]+$/;
  const RESCUE_RE = /^[a-z0-9]+$/;
  /** 空白/控制类(与服务端 IsPrintableCodePoint 排除表一致) */
  const WHITESPACE_CTRL_RE =
    /[\u0000-\u001F\u007F-\u009F\u0020\u00A0\u1680\u2000-\u200A\u2028\u2029\u202F\u205F\u3000]/;

  /** @returns {string|null} 错误文案 */
  function validateAccount(account) {
    const a = account.toLowerCase();
    if (!a) return '请输入账号';
    if (a.length < 4 || a.length > 30) return '账号长度需为 4-30 位';
    if (!ACCOUNT_RE.test(a)) return '账号仅支持字母、数字、下划线或短横线';
    if (a.startsWith('_') || a.startsWith('-') || a.endsWith('_') || a.endsWith('-'))
      return '账号首尾不能是下划线或短横线';
    return null;
  }

  /** @returns {string|null} 错误文案 */
  function validatePassword(pwd) {
    if (!pwd) return '请输入密码';
    const n = [...pwd].length;
    if (n < 8 || n > 64) return '密码长度需为 8-64 位';
    if (WHITESPACE_CTRL_RE.test(pwd))
      return '密码不能包含空格或控制字符';
    return null;
  }

  /** @returns {string|null} 错误文案(可空:不填则默认与账号一致) */
  function validateNickname(nick) {
    if (!nick) return null;
    const n = [...nick].length;
    if (n > 20) return '昵称长度需为 1-20 位';
    if (WHITESPACE_CTRL_RE.test(nick))
      return '昵称不能包含空白字符';
    return null;
  }

  /** @returns {string|null} 错误文案 */
  function validateRescue(rescue) {
    const r = rescue.toLowerCase();
    if (!r) return '请输入救援码';
    if (r.length < 8 || r.length > 16) return '救援码长度需为 8-16 位';
    if (!RESCUE_RE.test(r)) return '救援码仅支持数字或字母';
    return null;
  }

  /** 密码强度(zxcvbn):score 0-4;无 zxcvbn 时返回 0 */
  function passwordStrength(pwd) {
    if (!pwd || !window.zxcvbn) return { score: 0 };
    return window.zxcvbn(pwd);
  }

  /** 强度条渲染:seg 1-3 点亮(弱红/一般琥珀/强绿) */
  function renderStrength(elSegs, elLabel, score) {
    const colors = ['var(--err)', 'var(--err)', 'var(--warn)', 'var(--ok)'];
    elSegs.forEach((seg, i) => {
      seg.style.background = i <= score && i < 3 ? colors[score] : '';
    });
    const label = score >= 3 ? '强' : score === 2 ? '一般' : '弱';
    elLabel.textContent = label;
    elLabel.className = 'strength-label s' + score;
  }

  window.ZmAuth = {
    api, restCall, formatTime,
    toast, setBeacon, checkSession, startHeartbeat,
    validateAccount, validatePassword, validateNickname, validateRescue,
    passwordStrength, renderStrength,
  };
})();
