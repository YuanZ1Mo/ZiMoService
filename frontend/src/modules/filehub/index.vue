<script setup>
// 文件中心主页面 /portal/filehub(模块 code=filehub,index=2)
// 工具条 + 侧栏(空间树/我的分享/回收站)+ 面包屑 + 列表(虚拟滚动)+ 右键菜单 + 任务面板
// 选中:单击/Ctrl 加选/Shift 连选/Ctrl+A 全选/Esc 取消;批量条替换工具条(§7.2)
import { ref, reactive, computed, watch, onMounted, onBeforeUnmount, onActivated, onDeactivated, inject } from 'vue'
import { useSessionStore } from '../../stores/session'
import { useFilehubStore } from '../../stores/filehub'
import { filehubApi, USE_MOCK, fmtSize, fmtTime, kindOf, FILE_KINDS } from '../../api/filehub'
import FileList from './FileList.vue'
import TrashView from './TrashView.vue'
import TaskPanel from './TaskPanel.vue'
import MoveCopyDialog from './MoveCopyDialog.vue'
import ShareDialog from './ShareDialog.vue'
import Modal from '../../components/Modal.vue'
import QRCode from 'qrcode'
import './filehub.css'

const toast = inject('toast')
const session = useSessionStore()
const store = useFilehubStore()

const denied403 = ref(false)
function hasPerm(code) {
  const ps = session.user && session.user.permissions
  return Array.isArray(ps) && ps.includes(code)
}
// 权限判定:mock 演示模式放行(权限点 filehub 尚未登记进 user.db,见交接文档);真实模式按会话权限码
const denied = computed(() => denied403.value || (!USE_MOCK && !hasPerm('filehub')))

// ── 侧栏 ──
const view = ref('files')            // files | trash | shares
const spaces = ref([])
const space = ref(0)                 // 0=公共,uid=我的空间
const meSpace = computed(() => Number(session.user && session.user.uid) || 0)
const curSpace = computed(() => spaces.value.find(s => s.space === space.value))
async function loadSpaces() {
  try {
    spaces.value = await filehubApi.spaces() || []
    if (!spaces.value.some(s => s.space === space.value)) space.value = 0
  } catch (e) {
    if (e.status === 403) denied403.value = true
    else toast(e.message || '加载空间信息失败', 'err')
  }
}
function switchSpace(s) {
  if (view.value !== 'files') view.value = 'files'
  if (space.value === s) return
  space.value = s
  dirId.value = 0          // 切空间回到根目录(§3.1)
  selected.value = new Set()
  load()
}

// ── 列表状态 ──
const dirId = ref(0)
const breadcrumb = ref([])
const items = ref([])
const total = ref(0)
const loading = ref(false)
const sortKey = ref('name')
const sortOrder = ref('asc')
const pageSize = 200
const query = reactive({ keyword: '', searching: false, truncated: false })

async function load(append = false) {
  loading.value = true
  try {
    const d = await filehubApi.list({
      space: space.value, dir_id: dirId.value,
      sort: sortKey.value, order: sortOrder.value,
      // 追加时按已加载行数翻页;刷新(含上传完成/切回前台)一律回到第 1 页
      page: append ? Math.ceil(items.value.length / pageSize) + 1 : 1, size: pageSize
    })
    items.value = append ? items.value.concat(d.list || []) : (d.list || [])
    total.value = d.total || 0
    breadcrumb.value = d.breadcrumb || []
  } catch (e) {
    if (e.status === 404) toast('目录不存在,可能已被删除,建议触发一致性同步', 'warn')
    else toast(e.message || '加载目录失败', 'err')
  } finally { loading.value = false }
}
function loadMore() { if (items.value.length < total.value) load(true) }

// ── 搜索(当前目录及子目录,§3.2) ──
const searchInput = ref('')
async function doSearch() {
  const kw = searchInput.value.trim()
  if (!kw) { exitSearch(); return }
  query.searching = true
  loading.value = true
  try {
    const d = await filehubApi.search({ space: space.value, dir_id: dirId.value, keyword: kw, page: 1, size: 500 })
    items.value = d.list || []
    total.value = d.total || 0
    query.truncated = !!d.truncated
    breadcrumb.value = []
  } catch (e) { toast(e.message || '搜索失败', 'err') } finally { loading.value = false }
}
function exitSearch() {
  query.searching = false; query.truncated = false; searchInput.value = ''
  items.value = []; load()
}
watch(() => query.searching, () => { selected.value = new Set() })

// ── 选中管理 ──
const selected = ref(new Set())
let lastIdx = -1
function toggleSel(n, e) {
  const s = new Set(selected.value)
  if (e && (e.ctrlKey || e.metaKey)) {
    s.has(n.id) ? s.delete(n.id) : s.add(n.id)
    lastIdx = items.value.findIndex(x => x.id === n.id)
  } else if (e && e.shiftKey && lastIdx >= 0) {
    const a = Math.min(lastIdx, items.value.findIndex(x => x.id === n.id))
    const b = Math.max(lastIdx, items.value.findIndex(x => x.id === n.id))
    items.value.slice(a, b + 1).forEach(x => s.add(x.id))
  } else {
    s.clear(); s.add(n.id)
    lastIdx = items.value.findIndex(x => x.id === n.id)
  }
  selected.value = s
}
function selectAll() { selected.value = new Set(items.value.map(x => x.id)) }
function clearSel() { selected.value = new Set(); lastIdx = -1 }
const selTargets = computed(() => items.value.filter(x => selected.value.has(x.id)))
const selBytes = computed(() => selTargets.value.reduce((a, x) => a + Number(x.size || 0), 0))
function onKeydown(e) {
  if (e.key === 'Escape') { clearSel(); ctxMenu.show = false }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && view.value === 'files' && !query.searching) { e.preventDefault(); selectAll() }
}
onMounted(() => document.addEventListener('keydown', onKeydown))
onBeforeUnmount(() => document.removeEventListener('keydown', onKeydown))

// ── 打开(双击):目录进入,文件直接下载(本期无预览) ──
function openNode(n) {
  if (Number(n.type) === 1) {
    if (query.searching) exitSearch()
    dirId.value = n.id
    selected.value = new Set()
    load()
  } else {
    store.download([n.id])
  }
}

// ── 右键 / 行尾菜单 ──
const ctxMenu = reactive({ show: false, x: 0, y: 0, node: null })
function openCtx(e, node) {
  if (!selected.value.has(node.id)) toggleSel(node, null)
  ctxMenu.node = node
  ctxMenu.show = true
  const mw = 200, mh = 330
  ctxMenu.x = Math.min(e.clientX, window.innerWidth - mw - 8)
  ctxMenu.y = Math.min(e.clientY, window.innerHeight - mh - 8)
}
function closeCtx() { ctxMenu.show = false }
function ctxAct(act) {
  const n = ctxMenu.node
  closeCtx()
  if (act === 'download') store.download([n.id])
  else if (act === 'pack') doPack(selTargets.value.length > 1 ? selTargets.value : [n])
  else if (act === 'rename') openRename(n)
  else if (act === 'move') openMove([n])
  else if (act === 'copy') openCopy([n])
  else if (act === 'share') openShare(n)
  else if (act === 'delete') doDelete(selTargets.value.length > 1 ? selTargets.value : [n])
  else if (act === 'detail') openDetail(n)
}

// ── 操作:新建/重命名/删除/打包 ──
const nameDlg = reactive({ show: false, kind: 'mkdir', id: 0, name: '', busy: false, err: '' })
function openMkdir() { nameDlg.kind = 'mkdir'; nameDlg.name = ''; nameDlg.err = ''; nameDlg.show = true }
function openRename(n) { nameDlg.kind = 'rename'; nameDlg.id = n.id; nameDlg.extChanged = false; nameDlg.oldExt = (n.ext || '').toLowerCase(); nameDlg.name = n.name; nameDlg.err = ''; nameDlg.show = true }
watch(() => nameDlg.name, (v) => {
  if (nameDlg.kind !== 'rename') return
  const e = (v || '').split('.').pop().toLowerCase()
  nameDlg.extChanged = v.includes('.') && e !== nameDlg.oldExt
})
async function submitName() {
  const name = nameDlg.name.trim()
  if (!name) return
  nameDlg.busy = true; nameDlg.err = ''
  try {
    if (nameDlg.kind === 'mkdir') { await filehubApi.createDir(space.value, dirId.value, name); toast('已创建文件夹', 'ok') }
    else await filehubApi.rename(nameDlg.id, name)
    nameDlg.show = false
    load()
  } catch (e) {
    nameDlg.err = e.message || '操作失败'
  } finally { nameDlg.busy = false }
}
const confirmBox = reactive({ show: false, title: '', html: '', fn: null })
function askConfirm(title, html, fn) { confirmBox.title = title; confirmBox.html = html; confirmBox.fn = fn; confirmBox.show = true }
function doDelete(targets) {
  if (!targets.length) return
  const dirCount = targets.filter(t => Number(t.type) === 1).length
  const bytes = targets.reduce((a, t) => a + Number(t.size || 0), 0)
  askConfirm(`删除 ${targets.length} 项?`,
    `将移入回收站并保留 30 天${dirCount ? `,其中 ${dirCount} 个文件夹将连同全部子项一并删除` : ''};共 ${fmtSize(bytes)}。`,
    async () => {
      try {
        const r = await filehubApi.remove(targets.map(t => t.id))
        toast(`已移入回收站 ${r.count} 项`, 'ok')
        clearSel(); load(); loadSpaces()
      } catch (e) { toast(e.message || '删除失败', 'err') }
    })
}
async function doPack(targets) {
  if (!targets.length) return
  // 打包前预估:目录按"1 项 + 子树"粗估,超限由服务端 400 PACK_TOO_LARGE 兜底(§3.10.1)
  let items = 0, bytes = 0
  for (const t of targets) {
    if (Number(t.type) === 1) {
      items += 1 + Number(t.items || 0)
    } else { items++ }
    bytes += Number(t.size || 0)
  }
  askConfirm('打包下载?',
    `将打包 <b>${targets.length}</b> 个条目(约 ${items} 个文件,共 ${fmtSize(bytes)})为 zip 并转入任务面板,完成后可下载。`,
    async () => {
      try {
        await filehubApi.pack(space.value, targets.map(t => t.id))
        store.togglePanel(true)
      } catch (e) { toast(e.message || '创建打包任务失败', 'err') }
    })
}

// ── 移动 / 复制 ──
const mcDlg = reactive({ show: false, mode: 'move', targets: [] })
function openMove(targets) { if (!targets.length) return; mcDlg.mode = 'move'; mcDlg.targets = targets; mcDlg.show = true }
function openCopy(targets) { if (!targets.length) return; mcDlg.mode = 'copy'; mcDlg.targets = targets; mcDlg.show = true }
function onMcDone(r) {
  mcDlg.show = false
  if (r && r.__error) { toast(r.__error.message || '操作失败', 'err'); return }
  const ok = (r.moved || r.copied || []).length
  const skip = (r.skipped || []).length
  toast(`${mcDlg.mode === 'move' ? '移动' : '复制'}完成:${ok} 项成功${skip ? `,${skip} 项跳过` : ''}`, 'ok')
  clearSel(); load(); loadSpaces()
}

// ── 详情 ──
const detail = reactive({ show: false, node: null })
async function openDetail(n) {
  try { detail.node = await filehubApi.node(n.id); detail.show = true }
  catch (e) { toast(e.message || '加载详情失败', 'err') }
}

// ── 分享 ──
const shareDlg = reactive({ show: false, node: null })
function openShare(n) { shareDlg.node = n; shareDlg.show = true }

// ── 上传 ──
const fileInput = ref(null)
const dirInput = ref(null)
function pickFiles() { fileInput.value && fileInput.value.click() }
function pickFolder() { dirInput.value && dirInput.value.click() }
// 上传入队(文件夹/拖入共用):先按相对路径批量建目录(幂等),再按映射把文件派到各自目录
// rels[i] = files[i] 所在目录的相对路径(含顶层文件夹名);dirs = 途中遇到的全部目录(空目录也要建)
async function enqueueWithDirs(files, base, { rels = [], dirs = [] } = {}) {
  const list = [...files]
  if (!list.length && !dirs.length) return
  if (list.length > 5000) { toast('单批最多 5000 个文件,请分批上传', 'warn'); return }
  const relOf = (f, i) => rels[i] !== undefined ? rels[i] : (f.webkitRelativePath || '')
  const dirPaths = [...new Set([
    ...dirs,
    ...list.map((f, i) => { const r = relOf(f, i); const k = r.lastIndexOf('/'); return k > 0 ? r.slice(0, k) : '' })
  ].filter(Boolean))]
  let dirMap = {}
  if (dirPaths.length) {
    try {
      const d = await filehubApi.ensureBatch(space.value, base, dirPaths)
      dirMap = (d && d.map) || {}
    } catch (err) {
      toast(err.message || '创建目录结构失败', 'err')
      return
    }
  }
  const relMap = new Map(list.map((f, i) => [f, relOf(f, i)]))
  const dirOf = (f) => {
    const r = relMap.get(f) || ''
    const k = r.lastIndexOf('/')
    return k > 0 ? (dirMap[r.slice(0, k)] || base) : base
  }
  if (!list.length) { load(); toast(`已创建 ${dirPaths.length} 个空目录`, 'ok'); return }
  store.enqueueUploads(list, {
    space: space.value, dirId: base, conflict: 'ask', dirOf,
    batchName: list.length > 1 ? (relOf(list[0], 0).split('/')[0] || '') : '',
    onDone: () => { load(); loadSpaces() }
  })
  toast(`已加入上传队列 ${list.length} 个文件${dirPaths.length ? `(${dirPaths.length} 个目录)` : ''}`, 'ok')
}
async function onFilesPicked(e, dirIdOverride) {
  const files = [...(e.target.files || [])]
  e.target.value = ''
  if (!files.length) return
  await enqueueWithDirs(files, dirIdOverride || dirId.value)
}
// 拖入(含文件夹):FileList 已把目录递归展开,这里按相对路径建层级再上传
function onListFiles(payload) {
  const { files, rels, dirs, dirId: toDir } = payload
  enqueueWithDirs(files || [], toDir || dirId.value, { rels: rels || [], dirs: dirs || [] })
}

// ── 拖拽条目到文件夹 ──
async function onDragTo({ dirId: toDir, ids }) {
  try {
    const r = await filehubApi.move({ ids, target_space: space.value, target_dir_id: toDir, conflict: 'ask' })
    toast(`已移动 ${(r.moved || []).length} 项`, 'ok')
    clearSel(); load(); loadSpaces()
  } catch (e) {
    if (e.code === 'NAME_EXISTS') toast('存在同名冲突,请用"移动"菜单选择处理策略', 'warn')
    else toast(e.message || '移动失败', 'err')
  }
}

// ── 我的分享 ──
const shares = ref([])
const sharesLoading = ref(false)
const qr = reactive({ show: false, url: '', name: '' })
async function loadShares() {
  sharesLoading.value = true
  try {
    const d = await filehubApi.shareList({ page: 1, size: 200 })
    shares.value = d.list || []
  } catch (e) { toast(e.message || '加载分享失败', 'err') } finally { sharesLoading.value = false }
}
watch(view, (v) => { if (v === 'shares') loadShares() })
async function copyText(t, msg) {
  try { await navigator.clipboard.writeText(t); toast(msg || '已复制', 'ok') } catch { /* 剪贴板不可用 */ }
}
async function resetPwd(s) {
  try {
    const r = await filehubApi.sharePatch(s.id, { reset_pwd: true })
    if (r && r.pwd) { toast(`新提取码:${r.pwd}(仅本次展示)`, 'ok', 6000); loadShares() }
  } catch (e) { toast(e.message || '重置失败', 'err') }
}
function cancelShare(s) {
  askConfirm('取消分享?', `取消后「${s.name}」的分享链接立即失效,且不可恢复。`, async () => {
    try { await filehubApi.shareCancel(s.id); toast('已取消分享', 'ok'); loadShares() } catch (e) { toast(e.message || '操作失败', 'err') }
  })
}
async function showQr(s) {
  qr.name = s.name
  qr.url = s.url
  qr.show = true
  await new Promise(r => setTimeout(r, 50))
  const c = document.getElementById('fh-share-qr')
  if (c) QRCode.toCanvas(c, s.url, { width: 200, margin: 1 }, () => {})
}

// ── 排序 ──
function onSort(k) {
  if (sortKey.value === k) sortOrder.value = sortOrder.value === 'asc' ? 'desc' : 'asc'
  else { sortKey.value = k; sortOrder.value = 'asc' }
  query.searching ? doSearch() : load()
}

// ── keep-alive 钩子:回前台立即刷新;切走停轮询(§8.3) ──
onActivated(() => { store.onActivated(); if (view.value === 'files') load(); loadSpaces() })
onDeactivated(() => { store.onDeactivated(); closeCtx() })
onMounted(() => {
  filehubApi._syncMe(meSpace.value, (session.user && session.user.nickname) || '')
  loadSpaces()
  load()
  store.onChange(() => { if (view.value === 'files') load() })
})
</script>

<template>
  <div>
    <div class="page-head">
      <h1>文件中心</h1>
      <p class="page-sub">双空间文件浏览与传输 · 把文件拖到列表即可上传</p>
    </div>

    <!-- 无权限 -->
    <div v-if="denied" class="card work-card">
      <div class="empty">
        <div class="empty-icon">⛔</div>
        <div class="empty-title">无权限访问</div>
        <div class="empty-sub">您没有文件中心模块的访问权限,请联系管理员授权</div>
      </div>
    </div>

    <template v-else>
      <div class="fh-body">
        <!-- 侧栏 -->
        <aside class="fh-side">
          <div v-for="s in spaces" :key="s.space" class="card hover-lift sp-card" :class="{ active: view === 'files' && space === s.space }"
               @click="switchSpace(s.space)">
            <div class="sp-name">
              <svg v-if="Number(s.space) === 0" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14.5 14.5 0 0 1 0 18 14.5 14.5 0 0 1 0-18"/></svg>
              <svg v-else width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
              {{ s.name }}
            </div>
            <div class="sp-meta">
              <span class="num">{{ s.used_items }} 项{{ Number(s.space) !== 0 ? ` · ${fmtSize(s.used_size)}` : '' }}</span>
              <span>{{ Number(s.quota) ? `限额 ${fmtSize(s.quota)}` : (Number(s.space) === 0 ? '共享资源' : '不限额') }}</span>
            </div>
            <div v-if="Number(s.space) !== 0 && Number(s.quota)" class="quota"><i :style="{ width: Math.min(100, s.used_size / s.quota * 100) + '%' }"></i></div>
          </div>
          <div class="card" style="padding:6px">
            <button type="button" class="side-entry" :class="{ active: view === 'files' }" @click="view = 'files'">
              <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
              文件浏览
            </button>
            <button type="button" class="side-entry" :class="{ active: view === 'shares' }" @click="view = 'shares'">
              <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 13.5 6.8 4M15.4 6.5l-6.8 4"/></svg>
              我的分享
              <span class="cnt num">{{ shares.length }}</span>
            </button>
            <button type="button" class="side-entry" :class="{ active: view === 'trash' }" @click="view = 'trash'">
              <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 10h6M10 3h4v3h-4z"/></svg>
              回收站
              <span v-if="curSpace && curSpace.my_trash_items" class="cnt num">{{ curSpace.my_trash_items }}</span>
            </button>
          </div>
        </aside>

        <!-- 主区 -->
        <div class="fh-main">
          <!-- 文件浏览 -->
          <template v-if="view === 'files'">
            <!-- 工具条 / 批量条 -->
            <div v-if="!selected.size" class="card fh-toolbar">
              <div class="seg" style="width:196px">
                <button v-for="s in spaces" :key="s.space" type="button" class="seg-item"
                        :class="{ active: space === s.space }" @click="switchSpace(s.space)">{{ s.name }}</button>
              </div>
              <div class="input-wrap fh-search grow">
                <span class="input-prefix" aria-hidden="true">
                  <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></svg>
                </span>
                <input v-model.trim="searchInput" class="input" style="height:38px" placeholder="搜索当前目录及子目录…" @keyup.enter="doSearch" />
              </div>
              <button class="btn btn-grad" type="button" @click="pickFiles">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 16V4m0 0-5 5m5-5 5 5"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>
                上传
                <span style="display:inline-flex;flex-direction:column;margin-left:2px" @click.stop="pickFolder" title="上传文件夹">
                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
                </span>
              </button>
              <button class="btn btn-secondary" type="button" @click="openMkdir">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M12 10v6M9 13h6"/></svg>
                新建
              </button>
              <div class="vseg">
                <button type="button" class="on" title="列表视图" aria-label="列表视图">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>
                </button>
                <button type="button" title="网格视图" aria-label="网格视图" @click="toast('网格视图:在列表工具条右上切换(开发提示:视图状态当前由 FileList 属性驱动)', 'info')">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></svg>
                </button>
              </div>
              <button class="icon-btn" type="button" title="刷新" aria-label="刷新" @click="query.searching ? doSearch() : load()">
                <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 3v6h-6"/></svg>
              </button>
            </div>
            <div v-else class="card batch-bar">
              <button class="icon-btn" type="button" aria-label="取消选择" @click="clearSel">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
              </button>
              <span class="sel-num num">已选 {{ selected.size }} 项</span>
              <span class="cap num" style="color:var(--color-text-3)">{{ fmtSize(selBytes) }}</span>
              <span style="flex:1"></span>
              <button class="btn btn-secondary btn-sm" type="button" @click="doPack(selTargets)">打包下载</button>
              <button class="btn btn-secondary btn-sm" type="button" @click="openMove(selTargets)">移动</button>
              <button class="btn btn-secondary btn-sm" type="button" @click="openCopy(selTargets)">复制</button>
              <button class="btn btn-danger-soft btn-sm" type="button" @click="doDelete(selTargets)">删除</button>
            </div>

            <!-- 搜索提示条 -->
            <div v-if="query.searching" class="card fh-toolbar" style="padding:8px 16px">
              <span class="badge badge-warn" v-if="query.truncated">结果过多,已截断至 500 条,请缩小范围</span>
              <span class="cap" style="color:var(--color-text-2)">搜索"{{ searchInput }}" · 当前目录及全部子目录 · 共 <b class="num">{{ total }}</b> 条</span>
              <span style="flex:1"></span>
              <button class="btn btn-ghost btn-sm" type="button" @click="exitSearch">退出搜索</button>
            </div>

            <!-- 列表 -->
            <div class="card" style="padding:0;overflow:hidden;position:relative">
              <div v-if="!query.searching" class="crumbs">
                <button type="button" @click="dirId = 0; selected = new Set(); load()">
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
                  {{ curSpace ? curSpace.name : '空间' }}
                </button>
                <template v-for="b in breadcrumb" :key="b.id">
                  <svg class="sep" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="m9 6 6 6-6 6"/></svg>
                  <button type="button" :class="{ here: b.id === dirId }" @click="dirId = b.id; selected = new Set(); load()">{{ b.name }}</button>
                </template>
                <span style="flex:1"></span>
                <span class="cap num" style="color:var(--color-text-3)">共 {{ total }} 项</span>
              </div>
              <FileList :items="items" :selected="selected" :keyword="query.searching ? searchInput : ''"
                        :show-path="query.searching" :sort="sortKey" :order="sortOrder"
                        :loading="loading" :has-more="items.length < total"
                        @toggle="toggleSel" @open="openNode" @ctx="openCtx" @sort="onSort"
                        @drag-to="onDragTo" @files="onListFiles" @load-more="loadMore" />
            </div>
          </template>

          <!-- 回收站 -->
          <TrashView v-else-if="view === 'trash'" :space="space" :me-space="meSpace" @changed="loadSpaces" />

          <!-- 我的分享 -->
          <template v-else-if="view === 'shares'">
            <div class="card" style="padding:0;overflow:hidden">
              <div style="overflow-x:auto">
                <table class="ftbl">
                  <thead>
                    <tr>
                      <th style="min-width:170px">名称</th>
                      <th style="width:70px">类型</th>
                      <th style="width:190px">链接</th>
                      <th style="width:84px">提取码</th>
                      <th style="width:110px">有效期</th>
                      <th style="width:66px">浏览</th>
                      <th style="width:80px">下载</th>
                      <th style="width:80px">状态</th>
                      <th style="width:190px">操作</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-if="sharesLoading">
                      <td :colspan="9" style="padding:16px">
                        <div v-for="i in 4" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
                      </td>
                    </tr>
                    <template v-else>
                      <tr v-for="s in shares" :key="s.id" :style="Number(s.status) !== 1 ? 'opacity:.62' : ''">
                        <td><span class="fname" style="font-weight:600">{{ s.name }}</span></td>
                        <td><span class="ftype">{{ Number(s.node_type) === 1 ? '文件夹' : '文件' }}</span></td>
                        <td><span class="share-url-cell">{{ s.url.replace(/^https?:\/\//, '') }}</span></td>
                        <td><span class="badge badge-dim">{{ s.has_pwd ? '已设置' : '未设置' }}</span></td>
                        <td><span class="num">{{ s.expire_time ? fmtTime(s.expire_time) : '永久' }}</span></td>
                        <td><span class="num">{{ s.view_count }}</span></td>
                        <td><span class="num">{{ s.download_count }}{{ s.max_downloads ? '/' + s.max_downloads : '' }}</span></td>
                        <td>
                          <span class="badge" :class="{ 'badge-ok': Number(s.status) === 1, 'badge-err': Number(s.status) === 3, 'badge-dim': Number(s.status) === 2 }">
                            <span class="dot" aria-hidden="true"></span>{{ s.status_name }}
                          </span>
                        </td>
                        <td>
                          <span class="row-ops">
                            <button class="btn btn-ghost btn-sm" type="button" @click="copyText(s.url, '链接已复制')">复制</button>
                            <button class="btn btn-ghost btn-sm" type="button" @click="showQr(s)">二维码</button>
                            <button v-if="s.has_pwd && Number(s.status) === 1" class="btn btn-ghost btn-sm" type="button" @click="resetPwd(s)">重置提取码</button>
                            <button v-if="Number(s.status) === 1" class="btn btn-danger-soft btn-sm" type="button" @click="cancelShare(s)">取消</button>
                          </span>
                        </td>
                      </tr>
                      <tr v-if="!shares.length">
                        <td :colspan="9">
                          <div class="empty">
                            <div class="empty-icon">🔗</div>
                            <div class="empty-title">暂无分享</div>
                            <div class="empty-sub">在文件上右键选择"分享"即可创建</div>
                          </div>
                        </td>
                      </tr>
                    </template>
                  </tbody>
                </table>
              </div>
            </div>
          </template>
        </div>
      </div>
    </template>

    <!-- 右键菜单 -->
    <Teleport to="body">
      <div v-if="ctxMenu.show" style="position:fixed;inset:0;z-index:890" @click="closeCtx" @contextmenu.prevent="closeCtx"></div>
      <div v-if="ctxMenu.show" class="menu" style="position:fixed;z-index:891;min-width:200px" :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
        <button class="menu-item" type="button" @click="ctxAct('download')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 4v12m0 0 4-4m-4 4-4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>下载</button>
        <button class="menu-item" type="button" @click="ctxAct('pack')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/></svg>打包下载</button>
        <div class="menu-sep"></div>
        <button class="menu-item" type="button" @click="ctxAct('rename')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/></svg>重命名</button>
        <button class="menu-item" type="button" @click="ctxAct('move')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M5 12h14m0 0-5-5m5 5-5 5"/></svg>移动</button>
        <button class="menu-item" type="button" @click="ctxAct('copy')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>复制</button>
        <button class="menu-item" type="button" @click="ctxAct('share')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 13.5 6.8 4M15.4 6.5l-6.8 4"/></svg>分享</button>
        <div class="menu-sep"></div>
        <button class="menu-item danger" type="button" @click="ctxAct('delete')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>删除</button>
        <div class="menu-sep"></div>
        <button class="menu-item" type="button" @click="ctxAct('detail')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>详情</button>
      </div>
    </Teleport>

    <!-- 上传入口(隐藏) -->
    <input ref="fileInput" type="file" multiple style="display:none" @change="onFilesPicked($event, 0)" />
    <input ref="dirInput" type="file" webkitdirectory style="display:none" @change="onFilesPicked($event, 0)" />

    <!-- 新建 / 重命名 -->
    <Modal :show="nameDlg.show" :title="nameDlg.kind === 'mkdir' ? '新建文件夹' : '重命名'" @close="nameDlg.show = false">
      <div class="form-item" style="margin-bottom:0">
        <label class="form-label">名称</label>
        <input v-model.trim="nameDlg.name" class="input" :class="{ err: nameDlg.err }" maxlength="255"
               placeholder="不超过 255 字节,不含 <>:&quot;/\|?*" @keyup.enter="submitName" />
        <span v-if="nameDlg.err" class="field-error">{{ nameDlg.err }}</span>
        <span v-if="nameDlg.kind === 'rename' && nameDlg.extChanged" class="form-hint" style="color:var(--color-warn)">
          ⚠ 你正在修改扩展名,文件可能无法被原程序打开
        </span>
      </div>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="nameDlg.show = false">取消</button>
        <button class="btn btn-primary" type="button" :disabled="nameDlg.busy || !nameDlg.name.trim()" @click="submitName">
          {{ nameDlg.kind === 'mkdir' ? '创建' : '重命名' }}
        </button>
      </template>
    </Modal>

    <!-- 移动 / 复制 -->
    <MoveCopyDialog :show="mcDlg.show" :mode="mcDlg.mode" :targets="mcDlg.targets" :me-space="meSpace"
                    @close="mcDlg.show = false" @done="onMcDone" />

    <!-- 分享 -->
    <ShareDialog :show="shareDlg.show" :node="shareDlg.node" :is-public="!shareDlg.node || Number(shareDlg.node.space) === 0"
                 @close="shareDlg.show = false" @created="loadShares" />

    <!-- 详情 -->
    <Modal :show="detail.show" title="条目详情" @close="detail.show = false">
      <table v-if="detail.node" class="table" style="font-size:var(--fs-cap)">
        <tbody>
          <tr><td style="color:var(--color-text-3);width:80px">名称</td><td><b>{{ detail.node.name }}</b></td></tr>
          <tr><td style="color:var(--color-text-3)">类型</td><td>{{ FILE_KINDS[kindOf(detail.node)].label }}</td></tr>
          <tr><td style="color:var(--color-text-3)">大小</td><td class="num">{{ Number(detail.node.type) === 1 ? `${detail.node.items} 项` : fmtSize(detail.node.size) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">路径</td><td>{{ detail.node.path || '(空间根)' }}</td></tr>
          <tr><td style="color:var(--color-text-3)">创建者</td><td>{{ detail.node.owner_name }}</td></tr>
          <tr><td style="color:var(--color-text-3)">创建时间</td><td class="num">{{ fmtTime(detail.node.create_time) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">修改时间</td><td class="num">{{ fmtTime(detail.node.update_time) }}</td></tr>
        </tbody>
      </table>
    </Modal>

    <!-- 删除/打包等确认 -->
    <Modal :show="confirmBox.show" :title="confirmBox.title" @close="confirmBox.show = false">
      <p style="line-height:24px;margin:0" v-html="confirmBox.html"></p>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="confirmBox.show = false">取消</button>
        <button class="btn btn-primary" type="button" @click="confirmBox.show = false; confirmBox.fn && confirmBox.fn()">确认</button>
      </template>
    </Modal>

    <!-- 分享二维码 -->
    <Modal :show="qr.show" :title="`扫码访问「${qr.name}」`" @close="qr.show = false">
      <div style="display:flex;flex-direction:column;align-items:center;gap:12px">
        <div class="qr-box" style="width:216px;height:216px;padding:8px"><canvas id="fh-share-qr" width="200" height="200"></canvas></div>
        <span class="share-url" style="max-width:280px">{{ qr.url }}</span>
        <button class="btn btn-secondary btn-sm" type="button" @click="copyText(qr.url, '链接已复制')">复制链接</button>
      </div>
    </Modal>

    <!-- 任务面板(右下角) -->
    <TaskPanel />
  </div>
</template>

<style scoped>
.page-sub{color:var(--color-text-2);font-size:var(--fs-body);margin:6px 0 0}
</style>
