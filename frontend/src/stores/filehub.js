import { defineStore } from 'pinia'
import { filehubApi, downloadByUrl } from '../api/filehub'

// 文件中心跨组件状态:传输任务面板 + 上传队列
// 轮询契约(设计文档 §8.3,硬约束):
//   ① 只打 GET /filehub/tasks/active 单接口,1.5s 一次,一次拿全部进行中任务
//   ② 无进行中任务 → 停止轮询(不常驻请求)
//   ③ 页面切走(onDeactivated)停,回前台(onActivated)立即刷新一次
//   ④ 上传进度为客户端视角(已完成字节),服务端 done_size 在合并阶段更新,两者不必一致

export const useFilehubStore = defineStore('filehub', {
  state: () => ({
    // 服务端任务(tasks/active 轮询结果:task_no/type/name/status/done_size/size/done_items/total_items)
    serverTasks: [],
    // 客户端上传队列任务(前端视角进度)
    uploads: [],
    // 已完成历史(服务端 tasks 列表懒加载)
    history: [],
    historyTotal: 0,
    panelOpen: false,
    polling: false
  }),
  getters: {
    activeCount: (s) => s.serverTasks.length + s.uploads.filter(u => u.status === 'run' || u.status === 'wait').length,
    failedCount: (s) => s.history.filter(t => Number(t.status) === 4).length + s.uploads.filter(u => u.status === 'fail').length
  },
  actions: {
    // ── 轮询(唯一入口,面板打开或有进行中任务时才启动) ──
    ensurePolling() {
      if (this.polling) return
      this.polling = true
      this._tick()
    },
    async _tick() {
      if (!this.polling) return
      try {
        const list = await filehubApi.tasksActive()
        this.serverTasks = list || []
      } catch { /* 网络抖动静默,下轮重试 */ }
      if (!this.polling) return
      const busy = this.serverTasks.length > 0 || this.uploads.some(u => u.status === 'run' || u.status === 'wait')
      if (busy || this.panelOpen) {
        this._timer = setTimeout(() => this._tick(), 1500)
      } else {
        this.polling = false   // 无任务且面板未开 → 停轮询
      }
    },
    stopPolling() {
      this.polling = false
      if (this._timer) { clearTimeout(this._timer); this._timer = null }
    },
    // 页面切走/回来(keep-alive 钩子由页面转发)
    onDeactivated() {
      this._paused = true
      this.stopPolling()
    },
    onActivated() {
      this._paused = false
      this.refreshActive()
      this.ensurePolling()
    },
    refreshActive() {
      // 立即刷一次(回前台/打开面板)
      filehubApi.tasksActive().then(l => { this.serverTasks = l || [] }).catch(() => {})
    },
    togglePanel(open) {
      this.panelOpen = open === undefined ? !this.panelOpen : open
      if (this.panelOpen) { this.refreshActive(); this.ensurePolling() }
    },
    // ── 上传队列(并发 3,顺序 = 用户选择顺序,§3.8.1) ──
    // dirOf:可选。文件夹上传时按文件返回各自的目标目录 id(缺省用 dirId)
    enqueueUploads(files, { space, dirId, conflict = 'ask', batchName = '', onDone, dirOf }) {
      const batch = batchName || (files.length > 1 ? `${files.length} 个文件` : '')
      const jobs = files.map(f => ({
        key: 'up_' + Math.random().toString(36).slice(2, 10),
        type: 'upload', name: f.name, size: f.size, done: 0,
        status: 'wait', batch: batch, error: '', conflicts: [],
        ctrl: new AbortController(), session: '', file: f,
        space, dirId: dirOf ? dirOf(f) : dirId, conflict, onDone
      }))
      this.uploads.push(...jobs)
      this._pump()
      this.ensurePolling()
      return jobs.length
    },
    _pump() {
      const running = this.uploads.filter(u => u.status === 'run').length
      let slot = 3 - running
      for (const u of this.uploads) {
        if (slot <= 0) break
        if (u.status === 'wait') { slot--; this._runUpload(u) }
      }
    },
    async _runUpload(u) {
      u.status = 'run'
      try {
        const r = await filehubApi.upload(u.file, {
          space: u.space, dirId: u.dirId, conflict: u.conflict,
          signal: u.ctrl.signal,
          onProgress: (bytes) => { u.done = bytes },
          // 记住会话号:取消上传时据此让服务端回收分片与任务
          onSession: (uploadId) => { u.session = uploadId }
        })
        u.status = 'ok'; u.done = u.size
        if (u.onDone) u.onDone(r)
        this._notifyChange()
      } catch (e) {
        if (e && e.name === 'AbortError') { u.status = 'stop'; u.error = '已取消' }
        else {
          u.status = 'fail'
          u.error = (e && e.message) || '上传失败'
          // 同名冲突:服务端带回了冲突清单,须由用户选策略后按该策略重发;
          // 直接"重试"仍是 conflict=ask,会再次 409 —— 死循环
          u.conflicts = (e && e.code === 'NAME_EXISTS' && e.data && e.data.conflicts) || []
        }
      } finally {
        this._pump()
        // 全部结束后 4s 清理已完成项(避免面板堆积)
        if (!this.uploads.some(x => x.status === 'run' || x.status === 'wait')) {
          setTimeout(() => { this.uploads = this.uploads.filter(x => x.status === 'run' || x.status === 'wait' || x.status === 'fail') }, 4000)
        }
      }
    },
    cancelUpload(key) {
      const u = this.uploads.find(x => x.key === key)
      if (!u) return
      if (u.status === 'run') u.ctrl.abort()
      else if (u.status === 'wait') { u.status = 'stop'; u.error = '已取消' }
      // 分片上传已有会话:通知服务端删分片、置任务取消(否则任务会挂到会话过期)
      if (u.session) filehubApi.uploadCancel(u.session).catch(() => {})
      this._notifyChange()
    },
    // 重试:带 conflict 时按所选策略重发(同名冲突的出口),不带则沿用原策略
    async retryUpload(key, conflict) {
      const u = this.uploads.find(x => x.key === key)
      if (!u || u.status !== 'fail') return
      if (conflict) { u.conflict = conflict; u.conflicts = [] }
      u.status = 'wait'; u.done = 0; u.error = ''
      this._pump(); this.ensurePolling()
    },
    clearFinished() {
      this.uploads = this.uploads.filter(x => x.status === 'run' || x.status === 'wait')
      this.serverTasks = []
    },
    // 列表变更通知(上传完成/移动删除等 → 主页刷新当前目录)
    onChange(cb) { this._changeCbs = this._changeCbs || []; this._changeCbs.push(cb) },
    _notifyChange() { (this._changeCbs || []).forEach(cb => { try { cb() } catch { /* 忽略 */ } }) },
    // ── 下载/打包便捷封装 ──
    async download(ids) {
      const r = await filehubApi.downloadToken(ids)
      if (r && r.url) downloadByUrl(r.url)
      else if (r && r.task_no) { this.refreshActive(); this.ensurePolling(); this.togglePanel(true) }
      return r
    },
    async downloadZip(taskNo) {
      // 打包产物下载:按 task_no 换取直链
      const r = await filehubApi.downloadPack(taskNo)
      if (r && r.url) downloadByUrl(r.url)
    }
  }
})
