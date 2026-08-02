// ZiMo 用量统计:DeepSeek 官方余额展示(定时刷新 + 手动刷新)
const { createApp } = Vue;

// API 基址:RESTful API 在 39441 端口,与静态页(443)异源——协议相对 + 显式端口(同 app.js/filehub.js 模式)
const REST_URL = `//${window.location.hostname}:39441/zimo/api`;

createApp({
  data() {
    return {
      available: false,
      total: null, granted: null, topped: null, currency: 'CNY',
      lastUpdated: '—', cacheHit: false, cacheAt: '',
      error: '', loading: false,
      _timer: null,
    };
  },
  mounted() {
    this.refresh();
    this._timer = setInterval(() => this.refresh(), 60000);   // 定时 60s 刷新
  },
  beforeUnmount() { if (this._timer) clearInterval(this._timer); },
  methods: {
    async refresh() {
      if (this.loading) return;
      this.loading = true;
      try {
        const resp = await fetch(REST_URL + '/deepseek/usage', { signal: AbortSignal.timeout(15000) });
        let data;
        try { data = await resp.json(); }
        catch { data = { error: '服务器返回非 JSON 响应 (HTTP ' + resp.status + ')' }; }
        // error 字段契约归一化:成功路径为空串;503/404 时 body 为 {error:{code,message}} 对象
        this.error = typeof data.error === 'object'
          ? (data.error.message || '请求失败')
          : (data.error || '');
        this.cacheHit = !!data.cache_hit;
        const infos = (data.balance && data.balance.balance_infos) || [];
        const b = infos[0] || {};
        this.available = !!(data.balance && data.balance.is_available);
        this.total = b.total_balance ?? null;
        this.granted = b.granted_balance ?? null;
        this.topped = b.topped_up_balance ?? null;
        this.currency = b.currency || 'CNY';
        this.lastUpdated = data.updated_at
          ? new Date(data.updated_at * 1000).toLocaleString() : '—';
        this.cacheAt = this.lastUpdated;
      } catch (e) {
        this.error = '请求失败: ' + e.message;
      } finally {
        this.loading = false;
      }
    },
  },
}).mount('#app');
