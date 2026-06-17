const { createApp } = Vue;

/** JRPC 服务端点 */
const JRPC_URL = `http://${window.location.hostname}:39440/ZiMo/JRPC`;

/**
 * @brief 调用 JRPC 方法
 * @param method JRPC 方法名
 * @param params 参数对象
 * @return {Promise<object>} 解析后的 result 对象
 */
async function jrpcCall(method, params = {}) {
  const r = await fetch(JRPC_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: Date.now(), jsonrpc: '2.0', method, params }),
  });
  const json = await r.json();
  if (json.error)
    throw new Error(json.error.message || JSON.stringify(json.error));
  return json.result;
}

createApp({
  data() {
    return {
      page: 'home',
      online: false,
      time: '',
      cards: [
        { key:'http',    icon:'🌐', label:'HTTP 服务器',    val:'—', ok:false, sub:'' },
        { key:'jrpc',    icon:'📡', label:'JRPC HTTP',      val:'—', ok:false, sub:'' },
        { key:'hub',     icon:'🔀', label:'Hub 路由层',     val:'—', ok:false, sub:'' },
        { key:'jrpcpx',  icon:'🔗', label:'JRPC Proxy',     val:'—', ok:false, sub:'' },
        { key:'ws',      icon:'📡', label:'Broadcast',      val:'—', ok:false, sub:'' },
        { key:'cpu',     icon:'💻', label:'CPU 占用',       val:'—', ok:true,  sub:'' },
        { key:'mem',     icon:'🧠', label:'内存占用',       val:'—', ok:true,  sub:'' },
        { key:'gpu',     icon:'🎮', label:'GPU 占用',       val:'—', ok:true,  sub:'' },
      ],
      routes: [],
      testMethod: 'getStatus', testParams: '', tResult: '', tLoading: false,
      testId: 1, testJsonrpc: '2.0',
      aboutBackend: '<p>加载中...</p>', aboutFrontend: '<p>加载中...</p>',
      _timer: null,
    };
  },

  computed: {
    /** 请求体预览（根据 id/jsonrpc/方法名/参数实时生成） */
    requestPreview() {
      let params = {};
      if (this.testParams) {
        try { params = JSON.parse(this.testParams); }
        catch { params = this.testParams; }
      }
      return JSON.stringify({
        id: this.testId,
        jsonrpc: this.testJsonrpc,
        method: this.testMethod,
        params
      }, null, 2);
    },
  },

  created() {
    this.onHash();
    window.addEventListener('hashchange', this.onHash);
    this.startPolling();
    this.fetchRoutes();
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
        const d = await jrpcCall('getStatus');
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
        if (d.hub) {
          const [v, ok] = s(d.hub.status); set(2, v, ok, '');
        }
        if (d.jrpc_proxy) {
          const [v, ok] = s(d.jrpc_proxy.status); set(3, v, ok, '');
        }
        if (d.broadcast) {
          const [v, ok] = s(d.broadcast.status); set(4, v, ok, '端口 ' + d.broadcast.port);
        }
        if (d.system) {
          set(5, d.system.cpu.toFixed(1) + '%', true, '');
          set(6, d.system.memory.toFixed(1) + '%', true,
              d.system.usedMemMB + ' / ' + d.system.totalMemMB + ' MB');
          if (d.system.gpuAvailable)
            set(7, d.system.gpu.toFixed(1) + '%', true, '');
          else
            set(7, '不可用', false, '未检测到 GPU');
        }
      } catch(e) { this.online = false; }
    },

    async fetchRoutes() {
      try {
        const r = await jrpcCall('getRoutes');
        this.routes = (r.routes || []).map(x => ({ ...x, _open: false }));
      } catch(e) { this.routes = []; }
    },

    async fetchAbout() {
      const md = (text) => {
        if (typeof marked !== 'undefined') return marked.parse(text);
        return '<pre>' + text.replace(/</g,'&lt;').replace(/>/g,'&gt;') + '</pre>';
      };
      try {
        const d = await jrpcCall('getAbout');
        this.aboutBackend = md(d.backend || '');
        this.aboutFrontend = md(d.frontend || '');
      } catch(e) {
        this.aboutBackend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>';
        this.aboutFrontend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>';
      }
    },

    async doTest() {
      this.tLoading = true; this.tResult = '';
      try {
        let params = {};
        if (this.testParams) {
          try { params = JSON.parse(this.testParams); }
          catch { throw new Error('参数 JSON 格式错误'); }
        }
        const body = JSON.stringify({
          id: this.testId,
          jsonrpc: this.testJsonrpc,
          method: this.testMethod,
          params
        });
        const r = await fetch(JRPC_URL, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body
        });
        const json = await r.json();
        if (json.error)
          throw new Error(json.error.message || JSON.stringify(json.error));
        this.tResult = JSON.stringify(json.result, null, 2);
      } catch(e) { this.tResult = 'Error: ' + e.message; }
      this.tLoading = false;
    },

    goTest(r) {
      this.testMethod = r.method;
      try {
        const ex = JSON.parse(r.requestExample);
        this.testId = ex.id || 1;
        this.testJsonrpc = ex.jsonrpc || '2.0';
        this.testParams = ex.params ? JSON.stringify(ex.params, null, 2) : '';
      } catch { this.testParams = ''; }
      this.tResult = '';
      window.location.hash = '#/test';
    },

    async copy(text) {
      try { await navigator.clipboard.writeText(text); }
      catch { /* fallback */ }
    }
  }
}).mount('#app');
