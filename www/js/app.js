const { createApp } = Vue;

/** RESTful API 端点 */
const REST_URL = `http://${window.location.hostname}:39441/zimo/api`;

/** JRPC API 端点 */
const JRPC_URL = `http://${window.location.hostname}:39440/zimo/jrpc`;

/**
 * @brief 调用 RESTful API
 * @param method HTTP 方法
 * @param path   路径（REST_URL 之后的部分）
 * @param params query string 参数对象
 * @param body   请求体（POST/PUT 时使用，null 表示无 body）
 */
async function restCall(method, path, params = {}, body = null) {
  let url = REST_URL + path;
  const qs = Object.entries(params)
    .filter(([, v]) => v !== '' && v !== null && v !== undefined)
    .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v))
    .join('&');
  if (qs) url += '?' + qs;

  const opts = { method };
  if (body !== null && body !== '') {
    opts.headers = { 'Content-Type': 'application/octet-stream' };
    opts.body = body;
  }

  const r = await fetch(url, opts);
  if (r.headers.get('Content-Type')?.includes('application/json')) {
    const json = await r.json();
    if (json.error)
      throw new Error(json.error.message || JSON.stringify(json.error));
    return json;
  }
  return { _status: r.status, _statusText: r.statusText };
}

/**
 * @brief 调用 JRPC API（POST JSON-RPC 2.0）
 */
async function jrpcCall(method, params = {}, id = 1, jsonrpc = '2.0') {
  const body = JSON.stringify({ id, jsonrpc, method, params });
  const r = await fetch(JRPC_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body
  });
  const json = await r.json();
  if (json.error)
    throw new Error(json.error.message || JSON.stringify(json.error));
  return json;
}

/** JRPC 接口文档（硬编码，与服务端保持一致） */
const JRPC_ROUTES = [
  { method:'ping',                description:'心跳检测',             params:{},        responseExample:'{"result":{"pong":true}}' },
  { method:'drop',                description:'断开 TAP 连接',        params:{},        responseExample:'(无响应)' },
  { method:'getTime',             description:'获取服务器当前时间',   params:{},        responseExample:'{"result":{"time":"2025-...","timestamp":1700000000}}' },
  { method:'getStatus',           description:'获取服务器综合状态',   params:{},        responseExample:'{"result":{"http":{"status":"running","port":80},...}}' },
  { method:'broadcast',           description:'向匹配 tag 的客户端广播消息', params:{topic:'',content:'',tag:''}, responseExample:'{"result":{"success":true}}' },
  { method:'echo',                description:'通用接口测试（回显参数）', params:{key:'val'}, responseExample:'{"result":{"echo":{"key":"val"}}}' },
  { method:'getRoutes',           description:'获取 JRPC 方法文档列表', params:{},      responseExample:'{"result":{"routes":[...],"total":15}}' },
  { method:'getAbout',            description:'获取后端和前端技术信息', params:{},       responseExample:'{"result":{"backend":"...","frontend":"..."}}' },
  { method:'listFiles',           description:'列出目录下的文件和文件夹', params:{path:''},  responseExample:'{"result":{"ok":true,"files":[...]}}' },
  { method:'searchFiles',         description:'模糊搜索文件/文件夹',   params:{keyword:''}, responseExample:'{"result":{"ok":true,"results":[...]}}' },
  { method:'createDir',           description:'新建目录（可选设密码）', params:{path:'',dirName:'',username:'',password:''}, responseExample:'{"result":{"ok":true}}' },
  { method:'deleteItem',          description:'删除文件或文件夹',      params:{path:'',username:'',password:''}, responseExample:'{"result":{"ok":true}}' },
  { method:'verifyDirPassword',   description:'验证目录密码',          params:{path:'',password:''}, responseExample:'{"result":{"ok":true,"valid":true}}' },
  { method:'changeDirPassword',   description:'修改目录密码',          params:{path:'',username:'',oldPassword:'',newPassword:''}, responseExample:'{"result":{"ok":true}}' },
  { method:'batchDelete',         description:'批量删除文件',          params:{paths:'',username:'',password:''}, responseExample:'{"result":{"ok":true,"deleted":3}}' },
];

createApp({
  data() {
    return {
      page: 'home',
      online: false,
      time: '',
      cards: [
        { key:'http',    icon:'🌐', label:'HTTP 服务器',    val:'—', ok:false, sub:'' },
        { key:'jrpc',    icon:'📡', label:'JRPC HTTP',      val:'—', ok:false, sub:'' },
        { key:'restful', icon:'🔌', label:'RESTful HTTP',   val:'—', ok:false, sub:'' },
        { key:'hub',     icon:'🔀', label:'Hub 路由层',     val:'—', ok:false, sub:'' },
        { key:'jrpcpx',  icon:'🔗', label:'JRPC Proxy',     val:'—', ok:false, sub:'' },
        { key:'ws',      icon:'📡', label:'Broadcast',      val:'—', ok:false, sub:'' },
        { key:'cpu',     icon:'💻', label:'CPU 占用',       val:'—', ok:true,  sub:'' },
        { key:'mem',     icon:'🧠', label:'内存占用',       val:'—', ok:true,  sub:'' },
        { key:'gpu',     icon:'🎮', label:'GPU 占用',       val:'—', ok:true,  sub:'' },
      ],
      // 文档
      docTab: 'jrpc',
      jrpcRoutes: JRPC_ROUTES.map(x => ({ ...x, _open: false })),
      restRoutes: [],
      // JRPC 测试
      testTab: 'jrpc',
      jrpcId: 1,
      jrpcJsonrpc: '2.0',
      jrpcMethod: 'getStatus',
      jrpcParams: '',
      jrpcResult: '',
      jrpcLoading: false,
      // RESTful 测试
      restMethod: 'GET',
      restPath: '/status',
      restParams: '',
      restBody: '',
      restResult: '',
      restLoading: false,
      // JSON 格式化
      jsonInput: '', jsonOutput: '', jsonError: '',
      // 其他
      toasts: [], _toastId: 0,
      aboutBackend: '<p>加载中...</p>', aboutFrontend: '<p>加载中...</p>',
      _timer: null,
    };
  },

  computed: {
    host() { return window.location.hostname; },
    /** JRPC 请求体预览 */
    jrpcRequestPreview() {
      let p = {};
      if (this.jrpcParams) {
        try { p = JSON.parse(this.jrpcParams); } catch {}
      }
      return JSON.stringify({
        id: this.jrpcId,
        jsonrpc: this.jrpcJsonrpc,
        method: this.jrpcMethod,
        params: p
      }, null, 2);
    },
    /** RESTful 请求预览 */
    restRequestPreview() {
      let qs = this.restParams || '';
      // 尝试解析为 JSON 再转为 query string
      if (qs && qs.trim().startsWith('{')) {
        try { qs = new URLSearchParams(JSON.parse(qs)).toString(); } catch {}
      }
      let result = this.restMethod + ' ' + REST_URL + this.restPath;
      if (qs) result += '?' + qs;
      if (this.restBody) {
        result += '\n\n[Body ' + this.restBody.length + ' bytes]';
        if (this.restBody.length < 200) result += '\n' + this.restBody;
      }
      return result;
    },
  },

  created() {
    this.onHash();
    window.addEventListener('hashchange', this.onHash);
    this.startPolling();
    this.fetchRestRoutes();
    this.fetchAbout();
  },

  beforeUnmount() {
    window.removeEventListener('hashchange', this.onHash);
    clearInterval(this._timer);
  },

  methods: {
    onHash() {
      const m = { '/': 'home', '/docs': 'docs', '/test': 'test', '/about': 'about' };
      this.page = m[window.location.hash.slice(1)] || 'home';
    },

    startPolling() {
      this.fetchStatus();
      this._timer = setInterval(() => this.fetchStatus(), 1000);
    },

    async fetchStatus() {
      try {
        const d = await restCall('GET', '/status');
        this.time = d.time || '—';
        this.online = true;

        const s = (v, label) => v === 'running' ? [label || '✅ 运行中', true] : ['⬜ 未启动', false];
        const c = this.cards;
        const set = (i, val, ok, sub) => { c[i].val = val; c[i].ok = ok; c[i].sub = sub; };

        if (d.http) {
          const [v, ok] = s(d.http.status); set(0, v, ok, '端口 ' + d.http.port);
        }
        if (d.jrpc_http) {
          const [v, ok] = s(d.jrpc_http.status); set(1, v, ok, '端口 ' + d.jrpc_http.port);
        }
        if (d.restful_http) {
          const [v, ok] = s(d.restful_http.status); set(2, v, ok, '端口 ' + d.restful_http.port);
        }
        if (d.hub) {
          const [v, ok] = s(d.hub.status); set(3, v, ok, '');
        }
        if (d.jrpc_proxy) {
          const [v, ok] = s(d.jrpc_proxy.status); set(4, v, ok, '');
        }
        if (d.broadcast) {
          const [v, ok] = s(d.broadcast.status); set(5, v, ok, '端口 ' + d.broadcast.port);
        }
        if (d.system) {
          set(6, d.system.cpu.toFixed(1) + '%', true, '');
          set(7, d.system.memory.toFixed(1) + '%', true,
              d.system.usedMemMB + ' / ' + d.system.totalMemMB + ' MB');
          if (d.system.gpuAvailable)
            set(8, d.system.gpu.toFixed(1) + '%', true, '');
          else
            set(8, '不可用', false, '未检测到 GPU');
        }
      } catch(e) { this.online = false; }
    },

    async fetchRestRoutes() {
      try {
        const r = await restCall('GET', '/routes');
        this.restRoutes = (r.routes || []).map(x => ({ ...x, _open: false }));
      } catch(e) { this.restRoutes = []; }
    },

    async fetchAbout() {
      const md = (text) => {
        if (typeof marked !== 'undefined') return marked.parse(text);
        return '<pre>' + text.replace(/</g,'&lt;').replace(/>/g,'&gt;') + '</pre>';
      };
      try {
        const d = await restCall('GET', '/about');
        this.aboutBackend = md(d.backend || '');
        this.aboutFrontend = md(d.frontend || '');
      } catch(e) {
        this.aboutBackend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>';
        this.aboutFrontend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>';
      }
    },

    // ── JRPC 测试 ──
    async doJrpcTest() {
      this.jrpcLoading = true; this.jrpcResult = '';
      try {
        let params = {};
        if (this.jrpcParams) {
          try { params = JSON.parse(this.jrpcParams); }
          catch { throw new Error('参数 JSON 格式错误'); }
        }
        const json = await jrpcCall(this.jrpcMethod, params, this.jrpcId, this.jrpcJsonrpc);
        this.jrpcResult = JSON.stringify(json, null, 2);
      } catch(e) { this.jrpcResult = 'Error: ' + e.message; }
      this.jrpcLoading = false;
    },

    goJrpcTest(r) {
      this.testTab = 'jrpc';
      this.jrpcMethod = r.method;
      this.jrpcParams = r.params && Object.keys(r.params).length ? JSON.stringify(r.params) : '';
      this.jrpcResult = '';
      window.location.hash = '#/test';
    },

    // ── RESTful 测试 ──
    async doRestTest() {
      this.restLoading = true; this.restResult = '';
      try {
        let params = {};
        if (this.restParams) {
          const raw = this.restParams.trim();
          if (raw.startsWith('{')) {
            try { params = JSON.parse(raw); }
            catch { throw new Error('参数 JSON 格式错误'); }
          } else {
            // 支持 key=val&key2=val2 格式
            for (const pair of raw.split('&')) {
              const eq = pair.indexOf('=');
              if (eq > 0) params[pair.slice(0, eq)] = pair.slice(eq + 1);
            }
          }
        }
        const json = await restCall(this.restMethod, this.restPath, params, this.restBody || null);
        this.restResult = JSON.stringify(json, null, 2);
      } catch(e) { this.restResult = 'Error: ' + e.message; }
      this.restLoading = false;
    },

    goRestTest(r) {
      this.testTab = 'rest';
      this.restMethod = r.method;
      this.restPath = r.path;
      this.restParams = '';
      this.restBody = '';
      this.restResult = '';
      window.location.hash = '#/test';
    },

    // ── 通用 ──
    jrpcReqExample(r) {
      return JSON.stringify({id:1, jsonrpc:'2.0', method:r.method, params:r.params||{}}, null, 2);
    },

    async copy(text) {
      if (!text || text.trim() === '') {
        this.showTip('⚠️ 没有可复制的内容', 'err');
        return;
      }
      let ok = false;
      try {
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(text);
          ok = true;
        } else {
          ok = this.fallbackCopy(text);
        }
      } catch {
        ok = this.fallbackCopy(text);
      }
      this.showTip(ok ? '✅ 已复制' : '❌ 复制失败', ok ? 'ok' : 'err');
    },

    fallbackCopy(text) {
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      ta.style.top = '-9999px';
      document.body.appendChild(ta);
      ta.focus();
      ta.select();
      try { document.execCommand('copy'); return true; } catch { return false; }
      finally { document.body.removeChild(ta); }
    },

    showTip(msg, type) {
      const id = ++this._toastId;
      this.toasts.push({ id, msg, type });
      setTimeout(() => {
        this.toasts = this.toasts.filter(t => t.id !== id);
      }, 3000);
    },

    jsonFormat(indent) {
      this.jsonError = '';
      try {
        const obj = JSON.parse(this.jsonInput);
        this.jsonOutput = JSON.stringify(obj, null, indent);
      } catch(e) {
        this.jsonError = e.message;
        this.jsonOutput = '';
      }
    },

    jsonEscape() {
      this.jsonError = '';
      this.jsonOutput = JSON.stringify(this.jsonInput);
    },

    jsonUnescape() {
      this.jsonError = '';
      try {
        this.jsonOutput = JSON.parse(this.jsonInput);
        if (typeof this.jsonOutput === 'string') {
        } else {
          this.jsonOutput = JSON.stringify(this.jsonOutput, null, 4);
        }
      } catch(e) {
        this.jsonError = e.message;
        this.jsonOutput = '';
      }
    },
  }
}).mount('#app');
