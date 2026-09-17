<script setup>
// 文件中心管理(/portal/system-manager/filehub-admin,权限点 filehubAdmin)
// 四区块:①概览 ②一致性同步(手动触发/进度/取消) ③缓存与任务(缓存区/全量回收站/全量任务) ④审计日志(业务日志+分享访问日志)
import { ref, reactive, computed, watch, onMounted, onBeforeUnmount, inject } from 'vue'
import { useSessionStore } from '../../stores/session'
import { filehubApi, fmtSize, fmtTime } from '../../api/filehub'
import { useFilehubStore } from '../../stores/filehub'
import Modal from '../../components/Modal.vue'
import FileIcon from '../filehub/FileIcon.vue'
import '../filehub/filehub.css'

const toast = inject('toast')
const session = useSessionStore()
const store = useFilehubStore()

const denied403 = ref(false)
function hasPerm(code) {
  const ps = session.user && session.user.permissions
  return Array.isArray(ps) && ps.includes(code)
}
// 权限判定:按会话权限码(权限点 filehubAdmin 由服务端启动时登记给 developer/admin)
const denied = computed(() => denied403.value || !hasPerm('filehubAdmin'))

const tab = ref('overview')
const TABS = [
  { key: 'overview', name: '概览' },
  { key: 'sync', name: '一致性同步' },
  { key: 'cache', name: '缓存与任务' },
  { key: 'audit', name: '审计日志' }
]

// ── ① 概览 ──
const stats = ref(null)
async function loadStats() {
  try { stats.value = await filehubApi.admin.stats() }
  catch (e) { if (e.status === 403) denied403.value = true; else toast(e.message || '加载概览失败', 'err') }
}

// ── ② 一致性同步(运行中 1.5s 轮询) ──
const sync = reactive({ running: false, progress: null, last: null, starting: false })
let syncTimer = null
async function loadSync() {
  try {
    const d = await filehubApi.admin.syncStatus()
    sync.running = !!d.running
    sync.progress = d.progress
    sync.last = d.last
    if (sync.running && !syncTimer) syncTimer = setInterval(loadSync, 1500)
    if (!sync.running && syncTimer) { clearInterval(syncTimer); syncTimer = null; loadStats() }
  } catch { /* 静默 */ }
}
async function startSync(dryRun) {
  sync.starting = true
  try {
    await filehubApi.admin.syncStart(dryRun)
    toast('一致性同步已启动', 'ok')
    loadSync()
  } catch (e) { toast(e.message || '触发失败', 'err') } finally { sync.starting = false }
}
async function cancelSync() {
  try { await filehubApi.admin.syncCancel(); toast('已取消(已完成部分保留)', 'warn'); loadSync() } catch { /* 静默 */ }
}
const dryRun = ref(true)

// ── ③ 缓存与任务 ──
const cache = ref([])
const adminTrashRows = ref([])
const adminTasks = ref([])
async function loadCache() {
  try { const d = await filehubApi.admin.cache(); cache.value = d.list || [] } catch { /* 静默 */ }
}
async function cleanCache(names) {
  try {
    const r = await filehubApi.admin.cacheClean(names)
    toast(`已清理 ${r.deleted} 项,释放 ${fmtSize(r.bytes)}`, 'ok')
    loadCache()
  } catch (e) { toast(e.message || '清理失败', 'err') }
}
const cacheConfirm = reactive({ show: false, name: '' })
async function loadAdminTrash() {
  try { const d = await filehubApi.admin.trash({ page: 1, size: 50 }); adminTrashRows.value = d.list || [] } catch { /* 静默 */ }
}
const dangerConfirm = reactive({ show: false, title: '', html: '', fn: null })
function askDanger(title, html, fn) { dangerConfirm.title = title; dangerConfirm.html = html; dangerConfirm.fn = fn; dangerConfirm.show = true }
function cleanAllTrash() {
  const n = adminTrashRows.value.length
  const bytes = adminTrashRows.value.reduce((a, x) => a + Number(x.size || 0), 0)
  askDanger('整体清空回收站?', `将<b style="color:var(--color-err)">物理删除</b>全部 <b>${n}</b> 个条目,共 <b>${fmtSize(bytes)}</b>,不可撤销。`, async () => {
    try {
      const r = await filehubApi.admin.trashClear('')
      toast(`已清空 ${r.purged} 项 / ${fmtSize(r.bytes)}`, 'ok')
      loadAdminTrash(); loadStats()
    } catch (e) { toast(e.message || '操作失败', 'err') }
  })
}
// 保留期清理:只清理已过 30 天保留期的条目(与"整体清空"是两件事)
function cleanExpiredTrash() {
  askDanger('立即执行保留期清理?', '只会物理删除<b>已超过 30 天保留期</b>的回收站条目;未到期的条目保留。', async () => {
    try {
      const r = await filehubApi.admin.trashClean('')
      toast(`保留期清理完成:${r.purged} 项 / ${fmtSize(r.bytes)}`, 'ok')
      loadAdminTrash(); loadStats()
    } catch (e) { toast(e.message || '操作失败', 'err') }
  })
}
function adminPurge(row) {
  askDanger('彻底删除该条目?', `「${row.name}」将<b style="color:var(--color-err)">物理删除</b>,不可撤销。`, async () => {
    try { await filehubApi.admin.trashPurge([row.id]); toast('已删除', 'ok'); loadAdminTrash() } catch (e) { toast(e.message || '失败', 'err') }
  })
}
function adminRestore(row) {
  askDanger('恢复该条目?', `「${row.name}」将恢复到原位置(管理端操作不受归属限制,记审计 admin_ 前缀)。`, async () => {
    try { await filehubApi.admin.trashRestore([row.id]); toast('已恢复', 'ok'); loadAdminTrash() } catch (e) { toast(e.message || '失败', 'err') }
  })
}
async function loadAdminTasks() {
  try { const d = await filehubApi.admin.tasks({ page: 1, size: 50 }); adminTasks.value = d.list || [] } catch { /* 静默 */ }
}
async function forceCancel(t) {
  try { await filehubApi.admin.taskCancel(t.task_no); toast('已强制取消', 'warn'); loadAdminTasks() } catch { /* 静默 */ }
}

// ── ④ 审计日志 ──
const logs = ref([])
const logsTotal = ref(0)
const logQuery = reactive({ action: '', page: 1, size: 20 })
const shareLogs = ref([])
const shareLogsTotal = ref(0)
const auditTab = ref('file')
async function loadLogs() {
  try {
    const d = await filehubApi.admin.logs({ ...logQuery })
    logs.value = d.list || []; logsTotal.value = d.total || 0
  } catch { /* 静默 */ }
}
async function loadShareLogs() {
  try {
    const d = await filehubApi.admin.shareLogs({ page: 1, size: 20 })
    shareLogs.value = d.list || []; shareLogsTotal.value = d.total || 0
  } catch { /* 静默 */ }
}
watch(auditTab, (v) => { v === 'file' ? loadLogs() : loadShareLogs() })

const ACTION_NAME = {
  upload: '上传', download: '下载', pack: '打包', mkdir: '新建', rename: '重命名', move: '移动',
  copy: '复制', delete: '删除', restore: '恢复', purge: '彻底删除', trash_clear: '清空回收站',
  share_create: '分享创建', share_cancel: '分享取消', share_access: '分享访问',
  admin_sync: '一致性同步', admin_cache_clean: '缓存清理'
}
// 任务字段本地映射(服务端只下发需求 §5.6 的字段,不依赖冗余展示字段)
const TASK_TYPE_NAME = { 1: '上传', 2: '复制', 3: '打包下载', 4: '目录统计', 5: '一致性同步', 6: '回收站清理' }
const TASK_STATUS_NAME = { 1: '排队中', 2: '进行中', 3: '已完成', 4: '失败', 5: '已取消', 6: '已中断' }
const SHARE_ACTION = { 1: '访问', 2: '提取码校验', 3: '列目录', 4: '下载', 5: '打包下载' }

onMounted(() => {
  loadStats(); loadSync(); loadCache(); loadAdminTrash(); loadAdminTasks(); loadLogs()
})
onBeforeUnmount(() => { if (syncTimer) clearInterval(syncTimer) })
</script>

<template>
  <div>
    <div class="underline-tabs">
      <button v-for="t in TABS" :key="t.key" type="button" class="utab" :class="{ on: tab === t.key }" @click="tab = t.key">
        {{ t.name }}
      </button>
    </div>

    <div v-if="denied" class="card work-card" style="margin-top:18px">
      <div class="empty">
        <div class="empty-icon">⛔</div>
        <div class="empty-title">无权限访问</div>
        <div class="empty-sub">您没有文件中心管理权限(filehubAdmin),请联系开发者授权</div>
      </div>
    </div>

    <template v-else>
      <!-- ① 概览 -->
      <div v-if="tab === 'overview'" class="stat-grid" style="margin-top:18px">
        <div class="card stat-card">
          <span class="s-label">公共空间</span>
          <span class="s-val">{{ stats ? fmtSize(stats.public.bytes) : '…' }}</span>
          <span class="s-sub">{{ stats ? `${stats.public.items} 项 · 回收站占用 ${fmtSize(stats.public.trash_bytes)}` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">个人空间</span>
          <span class="s-val">{{ stats ? fmtSize(stats.personal.bytes) : '…' }}</span>
          <span class="s-sub">{{ stats ? `${stats.personal.users} 个用户 · 最大占用 ${stats.personal.top[0] ? stats.personal.top[0].name : '-'}` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">回收站(全量)</span>
          <span class="s-val">{{ stats ? fmtSize(stats.trash.bytes) : '…' }}</span>
          <span class="s-sub">{{ stats ? `${stats.trash.items} 项 · 7 天内到期 ${stats.trash.expiring_7d} 项` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">缓存区 space_cache</span>
          <span class="s-val">{{ stats ? fmtSize(stats.cache.bytes) : '…' }}</span>
          <span class="s-sub">{{ stats ? `${stats.cache.zips} 个压缩包 · ${stats.cache.chunks} 个分片目录` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">分享</span>
          <span class="s-val">{{ stats ? stats.share.active : '…' }}</span>
          <span class="s-sub">{{ stats ? `有效 · 今日访问 ${stats.share.today_views} / 下载 ${stats.share.today_downloads}` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">任务</span>
          <span class="s-val">{{ stats ? stats.task.running : '…' }}</span>
          <span class="s-sub">{{ stats ? `进行中 · 今日完成 ${stats.task.today_ok} / 失败 ${stats.task.today_fail}` : '' }}</span>
        </div>
        <div class="card stat-card">
          <span class="s-label">库文件 filehub.db</span>
          <span class="s-val">{{ stats ? fmtSize(stats.db_size) : '…' }}</span>
          <span class="s-sub">独立库,与 user.db 物理隔离</span>
        </div>
      </div>

      <!-- ② 一致性同步 -->
      <div v-else-if="tab === 'sync'" class="card work-card">
        <div class="sync-banner">
          <span class="sync-ring" :class="{ spin: sync.running }">
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 3v6h-6"/></svg>
          </span>
          <div style="flex:1;min-width:260px">
            <div class="row between">
              <b>{{ sync.running ? '全空间同步进行中…' : '空闲(每日 03:00 自动校验)' }}</b>
              <span v-if="sync.progress" class="cap num" style="color:var(--color-text-3)">已运行 {{ Math.round((sync.progress.elapsed || 0) / 1000) }}s</span>
            </div>
            <div class="tprog" style="margin-top:8px"><i :style="{ width: (sync.progress ? Math.min(100, sync.progress.scanned / (sync.progress.total || 1) * 100) : 0) + '%' }"></i></div>
            <div v-if="sync.progress" class="sync-stats">
              <span class="ss"><b class="num">{{ sync.progress.scanned }}<span style="font-size:12px;color:var(--color-text-3)"> / {{ sync.progress.total }}</span></b><span>已扫描目录</span></span>
              <span class="ss"><b class="num delta-up">+{{ sync.progress.added }}</b><span>补建(磁盘→库)</span></span>
              <span class="ss"><b class="num delta-del">-{{ sync.progress.removed }}</b><span>删除(库→磁盘)</span></span>
              <span class="ss"><b class="num delta-fix">{{ sync.progress.fixed }}</b><span>名称/类型修正</span></span>
              <span class="ss"><b class="num">{{ sync.progress.skipped }}</b><span>跳过</span></span>
            </div>
          </div>
          <div class="row" style="gap:8px">
            <button v-if="sync.running" class="btn btn-ghost" type="button" @click="cancelSync">取消</button>
            <button v-else class="btn btn-primary" type="button" :disabled="sync.starting" @click="startSync(dryRun)">触发同步</button>
          </div>
        </div>
        <div class="row between" style="padding:0 20px 16px;flex-wrap:wrap;gap:10px">
          <span class="form-hint">以磁盘为准修复漂移:补库/删行,不修改物理文件;同一时刻仅允许一次(重复触发 409 SYNC_RUNNING);回收站条目计入"已登记",不补建重复行。</span>
          <label class="row" style="gap:6px;font-size:var(--fs-cap);color:var(--color-text-2)">
            <input v-model="dryRun" type="checkbox" style="accent-color:var(--color-primary)" />先试运行(dry_run)
          </label>
        </div>
        <div v-if="sync.last" class="cap" style="padding:0 20px 16px;color:var(--color-text-3)">
          最近一次结果(<span class="num">{{ fmtTime(sync.last.end_time) }}</span>):
          补建 {{ sync.last.report.added || 0 }} / 删除 {{ sync.last.report.removed || 0 }} / 修正 {{ sync.last.report.fixed || 0 }} / 跳过 {{ sync.last.report.skipped || 0 }}
        </div>
      </div>

      <!-- ③ 缓存与任务 -->
      <template v-else-if="tab === 'cache'">
        <div class="row between" style="margin-top:18px;margin-bottom:10px;flex-wrap:wrap;gap:8px">
          <b>缓存区(压缩包 / 上传分片)</b>
          <button class="btn btn-danger-soft btn-sm" type="button" @click="cacheConfirm.show = true; cacheConfirm.name = ''">全部清理</button>
        </div>
        <div class="card work-card" style="padding:0;overflow:hidden">
          <div style="overflow-x:auto">
            <table class="table">
              <thead><tr><th style="min-width:220px">名称</th><th style="width:90px">所属</th><th style="width:100px">大小</th><th style="width:150px">创建时间</th><th style="width:150px">最后访问</th><th style="width:70px"></th></tr></thead>
              <tbody>
                <tr v-for="c in cache" :key="c.name">
                  <td>
                    <div class="cell-user">
                      <FileIcon :node="{ type: 2, ext: c.name.endsWith('.zip') ? 'zip' : 'part', name: c.name }" />
                      <span style="font-family:Consolas,monospace;font-size:12.5px">{{ c.name }}</span>
                    </div>
                  </td>
                  <td>{{ c.owner }}</td>
                  <td class="num">{{ fmtSize(c.size) }}</td>
                  <td class="num">{{ fmtTime(c.create_time) }}</td>
                  <td class="num">{{ fmtTime(c.access_time) }}</td>
                  <td><div class="tbl-actions"><button class="btn btn-danger-soft btn-sm" type="button" @click="cacheConfirm.show = true; cacheConfirm.name = c.name">删除</button></div></td>
                </tr>
                <tr v-if="!cache.length"><td :colspan="6"><div class="empty"><div class="empty-icon">🧹</div><div class="empty-title">缓存区为空</div></div></td></tr>
              </tbody>
            </table>
          </div>
        </div>

        <div class="row between" style="margin:18px 0 10px;flex-wrap:wrap;gap:8px">
          <b>全量回收站 <span class="badge badge-dim num">跨用户 / 跨空间</span></b>
          <span class="row" style="gap:8px">
            <button class="btn btn-ghost btn-sm" type="button" @click="cleanExpiredTrash">立即执行保留期清理</button>
            <button class="btn btn-danger btn-sm" type="button" @click="cleanAllTrash">整体清空</button>
          </span>
        </div>
        <div class="card work-card" style="padding:0;overflow:hidden">
          <div style="overflow-x:auto">
            <table class="table">
              <thead><tr><th style="min-width:160px">名称</th><th style="width:100px">空间</th><th style="width:80px">类型</th><th style="width:100px">大小</th><th style="width:90px">删除者</th><th style="width:140px">删除时间</th><th style="width:140px"></th></tr></thead>
              <tbody>
                <tr v-for="r in adminTrashRows" :key="r.id">
                  <td><b>{{ r.name }}</b></td>
                  <td>{{ r.space_name }}</td>
                  <td>{{ Number(r.type) === 1 ? '文件夹' : '文件' }}</td>
                  <td class="num">{{ Number(r.type) === 1 ? `${r.items} 项 · ${fmtSize(r.size)}` : fmtSize(r.size) }}</td>
                  <td>{{ r.del_owner_name }}</td>
                  <td class="num">{{ fmtTime(r.delete_time) }}</td>
                  <td>
                    <div class="tbl-actions">
                      <button class="btn btn-secondary btn-sm" type="button" @click="adminRestore(r)">恢复</button>
                      <button class="btn btn-danger-soft btn-sm" type="button" @click="adminPurge(r)">清除</button>
                    </div>
                  </td>
                </tr>
                <tr v-if="!adminTrashRows.length"><td :colspan="7"><div class="empty"><div class="empty-icon">✅</div><div class="empty-title">回收站为空</div></div></td></tr>
              </tbody>
            </table>
          </div>
        </div>

        <div class="row between" style="margin:18px 0 10px"><b>全量任务</b><button class="btn btn-ghost btn-sm" type="button" @click="loadAdminTasks">刷新</button></div>
        <div class="card work-card" style="padding:0;overflow:hidden">
          <div style="overflow-x:auto">
            <table class="table">
              <thead><tr><th style="min-width:180px">名称</th><th style="width:90px">类型</th><th style="width:90px">用户</th><th style="width:120px">进度</th><th style="width:80px">状态</th><th style="width:140px">时间</th><th style="width:90px"></th></tr></thead>
              <tbody>
                <tr v-for="t in adminTasks" :key="t.task_no">
                  <td><b>{{ t.name }}</b></td>
                  <td>{{ TASK_TYPE_NAME[Number(t.type)] || t.type }}</td>
                  <td>{{ '用户' + t.uid }}</td>
                  <td>
                    <div class="tprog" :class="{ done: Number(t.status) === 3, err: Number(t.status) === 4 }"><i :style="{ width: (Number(t.total_items) ? t.done_items / t.total_items * 100 : Number(t.size) ? t.done_size / t.size * 100 : Number(t.status) === 3 ? 100 : 30) + '%' }"></i></div>
                  </td>
                  <td><span class="badge" :class="{ 'badge-ok': Number(t.status) === 3, 'badge-err': Number(t.status) === 4, 'badge-info': Number(t.status) === 2, 'badge-dim': Number(t.status) === 5 || Number(t.status) === 6 }">{{ TASK_STATUS_NAME[Number(t.status)] || t.status }}</span></td>
                  <td class="num">{{ fmtTime(t.create_time) }}</td>
                  <td><div class="tbl-actions"><button v-if="Number(t.status) === 1 || Number(t.status) === 2" class="btn btn-danger-soft btn-sm" type="button" @click="forceCancel(t)">强制取消</button></div></td>
                </tr>
                <tr v-if="!adminTasks.length"><td :colspan="7"><div class="empty"><div class="empty-icon">🗂</div><div class="empty-title">暂无任务</div></div></td></tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>

      <!-- ④ 审计日志 -->
      <template v-else-if="tab === 'audit'">
        <div class="seg" style="width:220px;margin-top:18px">
          <button type="button" class="seg-item" :class="{ active: auditTab === 'file' }" @click="auditTab = 'file'">业务日志</button>
          <button type="button" class="seg-item" :class="{ active: auditTab === 'share' }" @click="auditTab = 'share'">分享访问日志</button>
        </div>
        <div v-if="auditTab === 'file'" class="card work-card" style="padding:0;overflow:hidden">
          <div class="toolbar" style="padding:14px 16px 0;margin-bottom:0">
            <select v-model="logQuery.action" class="input tb-sel" @change="logQuery.page = 1; loadLogs()">
              <option value="">全部动作</option>
              <option v-for="(n, k) in ACTION_NAME" :key="k" :value="k">{{ n }}</option>
            </select>
            <span class="pg-info">共 <b class="num">{{ logsTotal }}</b> 条 · 保留 90 天</span>
          </div>
          <div style="overflow-x:auto">
            <table class="table">
              <thead><tr><th style="width:140px">时间</th><th style="width:100px">用户</th><th style="width:100px">动作</th><th style="min-width:200px">对象</th><th style="min-width:180px">详情</th><th style="width:110px">IP</th><th style="width:70px">结果</th></tr></thead>
              <tbody>
                <tr v-for="l in logs" :key="l.id">
                  <td class="num">{{ fmtTime(l.create_time) }}</td>
                  <td>{{ l.account }}</td>
                  <td><span class="badge" :class="l.result === 1 ? 'badge-info' : 'badge-err'">{{ ACTION_NAME[l.action] || l.action }}</span></td>
                  <td>{{ l.node_name }}</td>
                  <td><span style="font-size:var(--fs-cap);color:var(--color-text-2)">{{ l.detail }}</span></td>
                  <td class="num">{{ l.ip }}</td>
                  <td><span class="badge" :class="l.result === 1 ? 'badge-ok' : 'badge-err'">{{ l.result === 1 ? '成功' : '失败' }}</span></td>
                </tr>
                <tr v-if="!logs.length"><td :colspan="7"><div class="empty"><div class="empty-icon">📭</div><div class="empty-title">暂无日志</div></div></td></tr>
              </tbody>
            </table>
          </div>
        </div>
        <div v-else class="card work-card" style="padding:0;overflow:hidden">
          <div style="overflow-x:auto">
            <table class="table">
              <thead><tr><th style="width:140px">时间</th><th style="width:110px">动作</th><th style="width:70px">结果</th><th style="width:110px">IP</th><th style="min-width:180px">UA 摘要</th><th style="min-width:120px">说明</th></tr></thead>
              <tbody>
                <tr v-for="l in shareLogs" :key="l.id">
                  <td class="num">{{ fmtTime(l.create_time) }}</td>
                  <td>{{ SHARE_ACTION[l.action] || l.action }}</td>
                  <td><span class="badge" :class="l.result === 1 ? 'badge-ok' : 'badge-err'">{{ l.result === 1 ? '成功' : '失败' }}</span></td>
                  <td class="num">{{ l.ip }}</td>
                  <td><span style="font-size:var(--fs-cap);color:var(--color-text-2)">{{ l.ua }}</span></td>
                  <td><span style="font-size:var(--fs-cap);color:var(--color-text-2)">{{ l.detail }}</span></td>
                </tr>
                <tr v-if="!shareLogs.length"><td :colspan="6"><div class="empty"><div class="empty-icon">📭</div><div class="empty-title">暂无日志</div></div></td></tr>
              </tbody>
            </table>
          </div>
        </div>
      </template>
    </template>

    <!-- 缓存清理确认 -->
    <Modal :show="cacheConfirm.show" title="清理缓存" @close="cacheConfirm.show = false">
      <p style="line-height:24px;margin:0">
        {{ cacheConfirm.name ? `将删除「${cacheConfirm.name}」;正在使用的分片/压缩包被删除后,对应上传/下载任务将失败。` : '将删除缓存区全部压缩包与分片目录;正在进行的上传/打包任务会失败,请确认无进行中任务。' }}
      </p>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="cacheConfirm.show = false">取消</button>
        <button class="btn btn-danger" type="button" @click="cacheConfirm.show = false; cleanCache(cacheConfirm.name ? [cacheConfirm.name] : [])">确认清理</button>
      </template>
    </Modal>

    <!-- 危险操作确认 -->
    <Modal :show="dangerConfirm.show" :title="dangerConfirm.title" @close="dangerConfirm.show = false">
      <p style="line-height:24px;margin:0" v-html="dangerConfirm.html"></p>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="dangerConfirm.show = false">取消</button>
        <button class="btn btn-danger" type="button" @click="dangerConfirm.show = false; dangerConfirm.fn && dangerConfirm.fn()">确认执行</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.utab{
  display:inline-flex;align-items:center;gap:6px;padding:10px 16px;margin-bottom:-1px;
  font-weight:600;font-size:var(--fs-body);color:var(--color-text-2);
  border-bottom:2px solid transparent;transition:color var(--dur),border-color var(--dur);
}
.utab:hover{color:var(--color-text)}
.utab.on{color:var(--color-primary);border-color:var(--color-primary)}
</style>
