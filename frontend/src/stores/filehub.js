import { defineStore } from 'pinia'
import { filehubApi, downloadByUrl } from '../api/filehub'

// 文件中心跨组件状态:传输任务面板 + 上传队列
// 轮询契约(设计文档 §8.3,硬约束):
//   ① 只打 GET /filehub/tasks/active 单接口,1.5s 一次,一次拿全部进行中任务
//   ② 无进行中任务 → 停止轮询(不常驻请求)
//   ③ 页面切走(onDeactivated)停,回前台(onActivated)立即刷新一次
//   ④ 上传进度为客户端视角(已完成字节),服务端 done_size 在合并阶段更新,两者不必一致

// 任务终态(与服务端 zm_file 的 kTask* 一致)
const PACK_DONE = 3                              // 已完成
const PACK_TERMINAL = [3, 4, 5, 6]               // 已完成/失败/已取消/已中断
// 自动下载的跟踪上限:逐秒查一次,30 分钟仍未结束就放弃(打包 20GB 上限足够宽裕)
const PACK_WATCH_MAX = 1800

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
    polling: false,
    // 待自动下载的打包任务号:打包完成后自动换取直链并触发一次下载(见 watchPackDone)
    pendingPacks: []
  }),
  getters: {
    /// 进行中列表里的服务端任务:上传(type=1)由客户端上传队列呈现(带字节级进度),
    /// 服务端上传任务行只进历史 —— 否则一次上传会同时出现"排队中"和"上传中"两条
    activeServerTasks: (s) => (s.serverTasks || []).filter(t => Number(t.type) !== 1),
    activeCount() {
      return this.activeServerTasks.length + this.uploads.filter(u => u.status === 'run' || u.status === 'wait').length
    },
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
      // 无进行中任务即停轮询(设计 §8.3);面板打开时由 togglePanel 补一次 refreshActive,
      // 不复用面板开合状态续期 —— 否则面板常开会变成常驻请求
      if (busy) {
        this._timer = setTimeout(() => this._tick(), 1500)
      } else {
        this.polling = false
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
    /** 登记一个打包任务:完成后自动换取直链并触发一次下载 */
    queuePackDownload(taskNo) {
      if (!taskNo || this.pendingPacks.includes(taskNo)) return
      this.pendingPacks.push(taskNo)
      this.watchPackDone()
    },
    /**
     * 跟踪队首打包任务,完成即自动触发下载
     *
     * 与任务面板的 1.5s 轮询相互独立(那条只管"进行中"列表,任务一完成就消失了,
     * 拿不到最终状态),这里按 task_no 逐秒查详情,直到进入终态。
     *
     * @param tries 已重试次数(网络抖动/任务未结束都算,超过上限即放弃跟踪)
     */
    async watchPackDone(tries = 0) {
      const taskNo = this.pendingPacks[0]
      if (!taskNo) return
      let status = 0
      try {
        status = Number((await filehubApi.taskDetail(taskNo)).status)
      } catch (e) {
        // 记录已不在(被删除/清历史):没什么可等的,停止跟踪
        if (e && e.status === 404) { this.pendingPacks.shift(); return }
        if (tries < PACK_WATCH_MAX) setTimeout(() => this.watchPackDone(tries + 1), 1000)
        else this.pendingPacks.shift()
        return
      }
      if (status !== PACK_DONE && !PACK_TERMINAL.includes(status)) {
        if (tries < PACK_WATCH_MAX) setTimeout(() => this.watchPackDone(tries + 1), 1000)
        else this.pendingPacks.shift()
        return
      }
      this.pendingPacks.shift()
      // 只有"完成"才自动下载;失败/取消的终态由任务面板展示,不打扰用户
      if (status === PACK_DONE) this.downloadZip(taskNo).catch(() => {})
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
        // 服务端在 skip 策略下返回 {skipped:true}(文件未入位),秒传返回 {instant:true};
        // 两者都不是"真的传了字节",文案要区分,否则用户会以为文件已上传
        u.skipped = !!(r && r.skipped)
        u.instant = !!(r && r.instant)
        if (u.onDone) u.onDone(r)
        this._notifyChange()
      } catch (e) {
        if (u.canceled || (e && e.name === 'AbortError')) { u.status = 'stop'; u.error = '已取消' }
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
      u.canceled = true   // 先标记:中止后 worker 抛的可能不是 AbortError(如分片 404),
                          // 没有这个标记就会把"已取消"显示成"失败"
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
    // 订阅上传队列变化;返回注销函数(页面卸载时务必调用,否则回调会随重挂累积)
    onChange(cb) {
      this._changeCbs = this._changeCbs || []
      this._changeCbs.push(cb)
      return () => {
        const i = (this._changeCbs || []).indexOf(cb)
        if (i >= 0) this._changeCbs.splice(i, 1)
      }
    },
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
