const { createApp } = Vue;

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
        { key:'ws',      icon:'🔌', label:'WebSocket',      val:'—', ok:false, sub:'' },
        { key:'cpu',     icon:'💻', label:'CPU 占用',       val:'—', ok:true,  sub:'' },
        { key:'mem',     icon:'🧠', label:'内存占用',       val:'—', ok:true,  sub:'' },
        { key:'gpu',     icon:'🎮', label:'GPU 占用',       val:'—', ok:true,  sub:'' },
      ],
      routes: [],
      testMethod: 'GET', testUrl: '/api/service_status', tBody: '', tResult: '', tLoading: false,
      aboutBackend: '<p>加载中...</p>', aboutFrontend: '<p>加载中...</p>',
      _timer: null,
    };
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
        // 时间
        const tr = await fetch('/api/service_time');
        const td = await tr.json();
        this.time = td.time || '—';
        this.online = true;

        // 状态
        const sr = await fetch('/api/service_status');
        const sd = await sr.json();

        const s = (v, label) => v === 'running' ? [label || '✅ 运行中', true] : ['⬜ 未启动', false];
        const c = this.cards;
        const set = (i, val, ok, sub) => { c[i].val = val; c[i].ok = ok; c[i].sub = sub; };

        if (sd.http) {
          const [v, ok] = s(sd.http.status); set(0, v, ok, '端口 ' + sd.http.port);
        }
        if (sd.jrpc_http) {
          const [v, ok] = s(sd.jrpc_http.status); set(1, v, ok, '端口 ' + sd.jrpc_http.port);
        }
        if (sd.hub) {
          const [v, ok] = s(sd.hub.status); set(2, v, ok, '');
        }
        if (sd.jrpc_proxy) {
          const [v, ok] = s(sd.jrpc_proxy.status); set(3, v, ok, '');
        }
        if (sd.websocket) {
          const [v, ok] = s(sd.websocket.status); set(4, v, ok, '端口 ' + sd.websocket.port);
        }
        if (sd.system) {
          set(5, sd.system.cpu.toFixed(1) + '%', true, '');
          set(6, sd.system.memory.toFixed(1) + '%', true,
              sd.system.usedMemMB + ' / ' + sd.system.totalMemMB + ' MB');
          if (sd.system.gpuAvailable)
            set(7, sd.system.gpu.toFixed(1) + '%', true, '');
          else
            set(7, '不可用', false, '未检测到 GPU');
        }
      } catch(e) { this.online = false; }
    },

    async fetchRoutes() {
      try {
        const r = await fetch('/api/routes');
        const d = await r.json();
        this.routes = (d.routes || []).map(x => ({ ...x, _open: false }));
      } catch(e) { this.routes = []; }
    },

    async fetchAbout() {
      const md = (text) => {
        if (typeof marked !== 'undefined') return marked.parse(text);
        return '<pre>' + text.replace(/</g,'&lt;').replace(/>/g,'&gt;') + '</pre>';
      };
      try {
        const b = await fetch('/api/about/backend');
        if (!b.ok) throw new Error('HTTP '+b.status);
        this.aboutBackend = md(await b.text());
      } catch(e) { this.aboutBackend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>'; }
      try {
        const f = await fetch('/api/about/frontend');
        if (!f.ok) throw new Error('HTTP '+f.status);
        this.aboutFrontend = md(await f.text());
      } catch(e) { this.aboutFrontend = '<p style="color:var(--err)">加载失败: '+e.message+'</p>'; }
    },

    async doTest() {
      this.tLoading = true; this.tResult = '';
      try {
        const opts = { method: this.testMethod, headers: {} };
        if (this.testMethod === 'POST' && this.tBody) {
          opts.headers['Content-Type'] = 'application/json';
          opts.body = this.tBody;
        }
        const r = await fetch(this.testUrl, opts);
        const txt = await r.text();
        try { this.tResult = JSON.stringify(JSON.parse(txt), null, 2); }
        catch { this.tResult = txt; }
      } catch(e) { this.tResult = 'Error: ' + e.message; }
      this.tLoading = false;
    },

    goTest(r) {
      this.testMethod = r.method === 'ANY' ? 'POST' : r.method;
      this.testUrl = r.path;
      this.tBody = r.requestExample.includes('{') ? r.requestExample.split('\n').pop() : '';
      this.tResult = '';
      window.location.hash = '#/test';
    },

    async copy(text) {
      try { await navigator.clipboard.writeText(text); }
      catch { /* fallback */ }
    }
  }
}).mount('#app');
