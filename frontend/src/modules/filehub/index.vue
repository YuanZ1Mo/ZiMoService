<script setup>
// 文件中心主页面 /portal/filehub(模块 code=filehub,index=2)
// 工具条 + 侧栏(空间树/我的分享/回收站)+ 面包屑 + 列表(虚拟滚动)+ 右键菜单 + 任务面板
// 选中:单击/Ctrl 加选/Shift 连选/Ctrl+A 全选/Esc 取消;批量条替换工具条(§7.2)
import { ref, reactive, computed, watch, nextTick, onMounted, onBeforeUnmount, onActivated, onDeactivated, inject } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useSessionStore } from '../../stores/session'
import { useFilehubStore } from '../../stores/filehub'
import { filehubApi, fmtSize, fmtNodeSize, fmtTime, kindOf, FILE_KINDS, nextSelection, nodeBytes } from '../../api/filehub'
import FileList from './FileList.vue'
import ZmSelect from '../../components/ZmSelect.vue'
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
// 权限判定:按会话权限码(权限点 filehub 由服务端启动时登记);API 403 亦可兜底
const denied = computed(() => denied403.value || !hasPerm('filehub'))

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
/**
 * 回收站工具条切空间
 *
 * 只换空间,回收站自己按新空间重取 —— 不能走 switchSpace:它会强制把视图切回"文件浏览"
 *
 * @param s 目标空间号
 */
/// 每个空间上次停留的目录(会话内记忆):切走再切回来应回到原处,而不是重置到根
const spaceDirs = reactive({})
/**
 * 换空间:先记下当前空间停在哪,再落到目标空间上次停留的目录
 *
 * 只改 space/dirId 两个状态,不动视图与列表 —— 工具条切空间还要做搜索态收尾、
 * 回收站切空间不该跳到文件浏览,两边的收尾各在外面做。
 *
 * @param s 目标空间号
 */
function applySpace(s) {
  if (space.value === s) return
  spaceDirs[space.value] = dirId.value
  space.value = s
  dirId.value = Number(spaceDirs[s] || 0)
}
function switchTrashSpace(s) {
  if (space.value === s) return
  applySpace(s)
  selected.value = new Set()
}
function switchSpace(s) {
  if (view.value !== 'files') view.value = 'files'
  if (space.value === s) return
  applySpace(s)
  selected.value = new Set()
  syncQuery()              // 切空间也是一次位置变化,后退能退回去
  // 搜索态切空间:关键词与结果都属于旧空间,先退出搜索(它内部会按新空间重载)
  if (query.searching) { exitSearch(); return }
  load()
}

// ── 目录前进后退(交给 vue-router 的历史栈) ──
// 位置写进 URL 查询串(space/dir),换目录就是一次真正的路由跳转 —— 于是鼠标侧键、
// Alt+←/→、浏览器返回键全都是 vue-router 自己处理的历史导航,本模块一行历史代码都不用写。
// 早期的做法是自己 history.pushState 挂状态、URL 保持不变:那会和 vue-router 的历史
// 记账相互干扰(生产上表现为点侧栏「用户主页」跳到 https://<域名>null/),已废弃。
// 附带好处:目录位置可分享、刷新后仍停在原处。
const route  = useRoute()
const router = useRouter()

/// @return 当前 URL 查询串所代表的目录位置
function queryPos() {
  return { space: Number(route.query.space || 0), dirId: Number(route.query.dir || 0) }
}
/**
 * 把当前位置同步进 URL 查询串
 *
 * 已在位时直接返回 —— 不比较的话,本模块自己发起的那次跳转回来还会再触发一次回放。
 *
 * @param replace true = 替换当前记录(不单独占一步后退),用于切空间这类
 */
function syncQuery(replace = false) {
  const p = queryPos()
  if (p.space === space.value && p.dirId === dirId.value) return
  const q = { ...route.query, space: String(space.value), dir: String(dirId.value) }
  replace ? router.replace({ query: q }) : router.push({ query: q })
}
/// 浏览器前进/后退(含鼠标侧键、Alt+方向键)落到本页时,按 URL 回放目录位置
watch(
  () => [route.query.space, route.query.dir],
  () => {
    // 已经离开本模块时路由也会变,别在别的页面上重载文件列表
    if (!route.path.startsWith('/portal/filehub')) return
    const p = queryPos()
    if (p.space === space.value && p.dirId === dirId.value) return   // 本模块自己发起的那次
    view.value = 'files'
    // 目录要变了,搜索态与选中集都不再适用;清搜索态但不单独发请求,下面统一 load 一次
    clearTimeout(searchTimer)
    searchSeq++
    query.searching = false; query.truncated = false; searchInput.value = ''
    applySpace(p.space)   // 先记住离开前的目录,再落到 URL 指定的位置
    dirId.value = p.dirId
    selected.value = new Set()
    lastIdx = -1
    load()
  }
)
/**
 * 面包屑跳级(回某一级祖先 / 回空间根)
 *
 * @param id 目标目录 id;0 = 空间根
 */
function goDir(id) {
  const target = Number(id)
  if (target === dirId.value) return   // 点当前这一级不产生新记录
  dirId.value = target
  selected.value = new Set()
  lastIdx = -1
  syncQuery()
  load()
}

// ── 列表状态 ──
const dirId = ref(0)
const breadcrumb = ref([])
const items = ref([])
const total = ref(0)
const listEpoch = ref(0)     // 整表被替换时自增,列表据此复位滚动位置
let loadSeq = 0              // 列表请求序号(丢弃过期响应)
const loading = ref(false)
const sortKey = ref('name')
const sortOrder = ref('asc')
const layout = ref('list')    // 列表/网格视图(list | grid),持久化在会话内
const pageSize = 200
const SEARCH_KW_MAX = 64     // 服务端关键词上限(超出直接返回空结果,故前端先拦)
const UPLOAD_MAX_FILES = 5000   // 文件夹上传单批上限:文件数(需求 §3.8.6)
const UPLOAD_MAX_DIRS = 1000    // 文件夹上传单批上限:目录数(同上)
const query = reactive({ keyword: '', searching: false, truncated: false })

async function load(append = false) {
  // 请求序号:切目录/搜索/刷新会并发发请求,回来晚的旧响应必须丢弃,
  // 否则会把上一个目录的行拼进当前列表
  const seq = ++loadSeq
  loading.value = true
  try {
    const d = await filehubApi.list({
      space: space.value, dir_id: dirId.value,
      sort: sortKey.value, order: sortOrder.value,
      // 追加时按已加载行数翻页;刷新(含上传完成/切回前台)一律回到第 1 页
      page: append ? Math.ceil(items.value.length / pageSize) + 1 : 1, size: pageSize
    })
    if (seq !== loadSeq) return
    items.value = append ? items.value.concat(d.list || []) : (d.list || [])
    if (!append) {
      listEpoch.value++   // 整表替换:通知列表把滚动位置复位
      // 顺带清理选中集与连选基准:旧条目可能已不在本页,留着会出现
      // "批量条显示已选 N 项、实际操作 0 项"的错位
      if (selected.value.size)
        selected.value = new Set([...selected.value].filter(id => items.value.some(x => x.id === id)))
      lastIdx = -1
    }
    total.value = d.total || 0
    breadcrumb.value = d.breadcrumb || []
  } catch (e) {
    if (seq !== loadSeq) return
    if (e.status === 404) toast('目录不存在,可能已被删除,建议触发一致性同步', 'warn')
    else toast(e.message || '加载目录失败', 'err')
  } finally { if (seq === loadSeq) loading.value = false }
}
function loadMore() { if (items.value.length < total.value) load(true) }

/**
 * 去抖刷新列表与空间用量
 *
 * 批量上传时每个文件完成都会触发一次(store 的 onDone 与 onChange 各一次),
 * 逐次刷新会打出数百个列表请求,并把用户反复打回第 1 页 —— 合并成一次。
 */
function scheduleRefresh() {
  if (refreshTimer) clearTimeout(refreshTimer)
  refreshTimer = setTimeout(() => { refreshTimer = null; load(); loadSpaces() }, 500)
}
let refreshTimer = null

// ── 搜索(当前目录及子目录,§3.2) ──
const searchInput = ref('')
const SEARCH_DEBOUNCE_MS = 400   // 键入停止多久后自动搜索
// 搜索态面包屑会被清空,"返回上一级目录"仍需要它:进入搜索时留一份快照
let bcBeforeSearch = []
let searchTimer = 0
let searchComposing = false   // 输入法组字中:拼音阶段每个字母都会触发 input,此时不该发请求
let searchSeq = 0
/**
 * 键入停止后自动搜索(防抖)
 *
 * 输入框不配"搜索"按钮,靠这个间隔把连续敲键合并成一次请求。
 * 清空关键词即时退出搜索、不等防抖 —— 否则列表会先空一下再回来。
 */
function scheduleSearch() {
  if (searchComposing) return   // 组字未结束,等 compositionend 再搜
  clearTimeout(searchTimer)
  if (!searchInput.value.trim()) { doSearch(); return }
  searchTimer = setTimeout(doSearch, SEARCH_DEBOUNCE_MS)
}
/// 立即搜索(回车):取消挂起的防抖,不必再等那 400ms
function flushSearch() {
  clearTimeout(searchTimer)
  doSearch()
}
/// 一键清空并退出搜索
function clearSearch() {
  searchInput.value = ''
  exitSearch()
}
async function doSearch() {
  clearTimeout(searchTimer)
  const kw = searchInput.value.trim()
  if (!kw) { exitSearch(); return }
  // 服务端关键词上限 64 字符:超了会静默返回 0 条,这里先拦下并说明原因
  if (kw.length > SEARCH_KW_MAX) {
    toast(`关键词最多 ${SEARCH_KW_MAX} 个字符(当前 ${kw.length} 个)`, 'warn')
    return
  }
  query.searching = true
  bcBeforeSearch = [...breadcrumb.value]   // 快照当前目录链,供"返回上一级目录"使用
  loading.value = true
  // 自动搜索下连续敲键会有两个请求在飞:只认最后一次发出的那个响应,
  // 否则慢的那个后到会把新结果盖回旧的
  const seq = ++searchSeq
  try {
    const d = await filehubApi.search({ space: space.value, dir_id: dirId.value, keyword: kw, page: 1, size: 500 })
    if (seq !== searchSeq) return
    items.value = d.list || []
    total.value = d.total || 0
    query.truncated = !!d.truncated
    breadcrumb.value = []
  } catch (e) {
    if (seq === searchSeq) toast(e.message || '搜索失败', 'err')
  } finally {
    if (seq === searchSeq) loading.value = false
  }
}
function exitSearch() {
  clearTimeout(searchTimer)
  searchSeq++   // 退出搜索后,在飞的响应不许再改列表
  query.searching = false; query.truncated = false; searchInput.value = ''
  items.value = []; load()
}
watch(() => query.searching, () => { selected.value = new Set() })

// ── 选中管理 ──
const selected = ref(new Set())
let lastIdx = -1
/**
 * 选中/取消选中一个条目
 *
 * 语义按点击方式区分:复选框(alwaysToggle)= 纯增删;Ctrl/Cmd 点击 = 增删;
 * Shift 点击 = 从上次落点连选;其余(单击行)= 单选并清空其它。
 *
 * @param n            条目
 * @param e            鼠标事件(带修饰键;缺省按单击处理)
 * @param alwaysToggle true = 只增删自身,不动其它选中项
 */
function toggleSel(n, e, alwaysToggle) {
  const r = nextSelection(items.value.map(x => x.id), selected.value, n.id, {
    ctrl: !!(e && (e.ctrlKey || e.metaKey)),
    shift: !!(e && e.shiftKey),
    alwaysToggle: !!alwaysToggle,
    lastIdx
  })
  selected.value = r.selected
  lastIdx = r.lastIdx
}
function selectAll() { selected.value = new Set(items.value.map(x => x.id)) }
function clearSel() { selected.value = new Set(); lastIdx = -1 }
// 表头全选框(FileList 抛出):勾选=全选当前已加载条目,取消=清空
function onToggleAll(checked) { checked ? selectAll() : clearSel() }
const selTargets = computed(() => items.value.filter(x => selected.value.has(x.id)))
// 选中项体积:文件取 size、目录取服务端下发的子树字节(bytes),两者统一口径
const selBytes = computed(() => selTargets.value.reduce((a, x) => a + nodeBytes(x), 0))
const selDirCount = computed(() => selTargets.value.filter(x => Number(x.type) === 1).length)
// 批量条上的构成说明:总体积 + 文件夹个数(个数用于提示"其中几个是文件夹")
const selSummary = computed(() => {
  const parts = [fmtSize(selBytes.value)]
  if (selDirCount.value) parts.push(`${selDirCount.value} 个文件夹`)
  return parts.join(' · ')
})
function onKeydown(e) {
  const t = e.target
  // 输入框/可编辑区里不劫持快捷键:否则 Ctrl+A 无法全选输入内容
  const typing = !!t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)
  if (e.key === 'Escape') { clearSel(); ctxMenu.show = false; closeShareCtx(); closeUpMenu() }
  if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && view.value === 'files' && !query.searching) { e.preventDefault(); selectAll() }
}
// keep-alive 下组件不卸载、只失活:监听须随激活状态挂摘,否则隐藏页仍会吃掉 Ctrl+A
onMounted(() => document.addEventListener('keydown', onKeydown))
onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeydown)
  clearTimeout(searchTimer)   // 卸载后防抖回调不该再发请求
  if (offChange) { offChange(); offChange = null }
})

// ── 打开(双击):目录进入,文件直接下载(本期无预览) ──
function openNode(n) {
  if (Number(n.type) === 1) {
    // 只重置搜索态(不能调 exitSearch:它内部会按旧目录先 load 一次,白跑一个请求)
    if (query.searching) {
      clearTimeout(searchTimer)   // 连带取消挂起的自动搜索,否则进目录后还会再补一次请求
      query.searching = false; query.truncated = false; searchInput.value = ''
    }
    dirId.value = n.id
    selected.value = new Set()
    lastIdx = -1
    syncQuery()
    load()
  } else {
    // 下载失败要说一声(文件可能已被他人删除 / 令牌过期)
    store.download([n.id]).catch(e => toast(e.message || '下载失败', 'err'))
  }
}

// ── 右键 / 行尾菜单 ──
// 触发源(source)决定"作用对象":row=右键行、dots=行尾 ⋯、area=列表空白区
// blank=true 表示空白处菜单(与条目菜单互斥)
const ctxMenu = reactive({ show: false, x: 0, y: 0, node: null, multi: false, blank: false })
/// 单选态的作用对象:row/dots 用 ctxMenu.node;area 触发时取选中集里的唯一项
const ctxTarget = computed(() => ctxMenu.node || selTargets.value[0] || null)
const ctxIsDir = computed(() => !!ctxTarget.value && Number(ctxTarget.value.type) === 1)
/**
 * 菜单定位:贴到视口底部时改为向上弹
 *
 * 原先只做了 x 方向钳制,菜单底部会被窗口裁掉。
 *
 * @param e   触发事件(取 clientX / clientY)
 * @param mh  菜单估算高度
 */
function placeMenu(e, mh) {
  const mw = 200
  ctxMenu.x = Math.min(e.clientX, window.innerWidth - mw - 8)
  ctxMenu.y = (e.clientY + mh + 8 > window.innerHeight) ? Math.max(8, e.clientY - mh)
                                                       : e.clientY
}
function openCtx(e, node, source) {
  ctxMenu.blank = false
  if (source === 'area') {
    // 空白处右键:有选中项 → 操作选中集;没有 → 空白处菜单(只有"返回上一级目录")
    if (!selected.value.size) { openBlankCtx(e); return }
  } else if (source === 'dots' || !selected.value.has(node.id)) {
    // 行尾 ⋯ = 只操作该行;右键未选中行 = 把它设为唯一选中项
    // (右键总能出菜单,否则用户会以为右键失灵)
    selected.value = new Set([node.id]); lastIdx = -1
  }
  ctxMenu.node = source === 'area' ? null : node
  ctxMenu.multi = selected.value.size > 1
  ctxMenu.show = true
  placeMenu(e, 350)   // 最多 8 项×38px + 2 条分隔 + 内边距
}
/// 空白处菜单:作用于"当前位置"而非某个条目
function openBlankCtx(e) {
  ctxMenu.node = null; ctxMenu.multi = false; ctxMenu.blank = true; ctxMenu.show = true
  placeMenu(e, 60)
}
/**
 * 返回上一级目录(空白处菜单)
 *
 * 面包屑 = 祖先链 + 当前目录(服务端下发),倒数第二级即父目录;
 * 当前目录直属空间根时没有倒数第二级,父目录取 0(空间根)。
 * 搜索态下面包屑被清空(结果跨子目录,显示当前路径会误导),改用进入搜索时的快照。
 * 已在空间根(dirId=0)时无上级,菜单项禁用。
 */
function goParent() {
  closeCtx()
  if (!dirId.value) return
  const bc = query.searching ? bcBeforeSearch : breadcrumb.value
  const parent = bc.length >= 2 ? Number(bc[bc.length - 2].id) : 0
  if (query.searching) {
    clearTimeout(searchTimer)
    query.searching = false; query.truncated = false; searchInput.value = ''
  }
  dirId.value = parent
  selected.value = new Set()
  lastIdx = -1
  syncQuery()
  load()
}
function closeCtx() { ctxMenu.show = false }
/// 菜单当前作用的对象集:多选 = 整个选中集,单选 = 那一条
const ctxTargets = computed(() => ctxMenu.multi ? selTargets.value
                                                : (ctxTarget.value ? [ctxTarget.value] : []))
function ctxAct(act) {
  const n = ctxTarget.value
  const targets = ctxTargets.value.slice()
  closeCtx()
  if (!targets.length) return
  if (act === 'open') openNode(n)
  else if (act === 'download') store.download(targets.map(x => x.id)).catch(e => toast(e.message || '下载失败', 'err'))
  else if (act === 'pack') doPack(targets)
  else if (act === 'rename') targets.length > 1 ? openBatchRename(targets) : openRename(n)
  else if (act === 'move') openMove(targets)
  else if (act === 'copy') openCopy(targets)
  else if (act === 'share') openShare(targets)
  else if (act === 'delete') doDelete(targets)
  else if (act === 'detail') targets.length > 1 ? openBatchDetail(targets) : openDetail(n)
}

// ── 操作:新建/重命名/删除/打包 ──
const nameDlg = reactive({
  show: false, kind: 'mkdir', id: 0, name: '', busy: false, err: '',
  isDir: false, extChanged: false, oldExt: ''
})
function openMkdir() { nameDlg.kind = 'mkdir'; nameDlg.name = ''; nameDlg.err = ''; nameDlg.show = true }
function openRename(n) {
  nameDlg.kind = 'rename'; nameDlg.id = n.id; nameDlg.extChanged = false
  nameDlg.isDir = Number(n.type) === 1        // 目录没有扩展名概念,不参与该项警告
  nameDlg.oldExt = (n.ext || '').toLowerCase(); nameDlg.name = n.name
  nameDlg.err = ''; nameDlg.show = true
}
watch(() => nameDlg.name, (v) => {
  if (nameDlg.kind !== 'rename' || nameDlg.isDir) return
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

// ── 批量重命名(多选):逐行改名,左列原名只读、右列可编辑 ──
const batchRename = reactive({ show: false, rows: [], busy: false })
function openBatchRename(targets) {
  batchRename.rows = targets.map(t => ({
    id: t.id, old: t.name, name: t.name,
    isDir: Number(t.type) === 1, oldExt: (t.ext || '').toLowerCase()
  }))
  batchRename.busy = false
  batchRename.show = true
}
/**
 * 该行是否改了扩展名(仅提示用,不拦截)
 * @param row  批量重命名的一行
 */
function extWarn(row) {
  if (row.isDir) return false
  const i = row.name.lastIndexOf('.')
  return i > 0 && row.name.slice(i + 1).toLowerCase() !== row.oldExt
}
async function submitBatchRename() {
  const rows = batchRename.rows.filter(r => r.name.trim() && r.name.trim() !== r.old)
  if (!rows.length) { toast('没有需要修改的名称', 'warn'); return }
  batchRename.busy = true
  let ok = 0
  const fails = []
  // 逐条串行:并发改名会让"部分失败"的定位变难,且同名冲突的判定依赖服务端逐条返回
  for (const r of rows) {
    try { await filehubApi.rename(r.id, r.name.trim()); ok++ }
    catch (e) { fails.push(`${r.old}(${e.message || '失败'})`) }
  }
  batchRename.busy = false
  batchRename.show = false
  if (fails.length) {
    toast(`重命名完成:${ok} 项成功,${fails.length} 项失败 —— ${fails.slice(0, 3).join(';')}${fails.length > 3 ? ' 等' : ''}`, 'warn')
  } else toast(`已重命名 ${ok} 项`, 'ok')
  clearSel(); load()
}

// ── 批量详情(多选):只汇总条目数与总体积,逐条字段没有公共值,统一显示 - ──
const batchDetail = reactive({ show: false, count: 0, dirs: 0, bytes: 0 })
function openBatchDetail(targets) {
  batchDetail.count = targets.length
  batchDetail.dirs = targets.filter(t => Number(t.type) === 1).length
  batchDetail.bytes = targets.reduce((a, t) => a + nodeBytes(t), 0)
  batchDetail.show = true
}
const confirmBox = reactive({ show: false, title: '', html: '', fn: null })
function askConfirm(title, html, fn) { confirmBox.title = title; confirmBox.html = html; confirmBox.fn = fn; confirmBox.show = true }
function doDelete(targets) {
  if (!targets.length) return
  const dirCount = targets.filter(t => Number(t.type) === 1).length
  // 体积按统一口径累加(目录用子树字节);文件夹另注明"连同子项删除",避免只看到总数不知影响面
  const bytes = targets.reduce((a, t) => a + nodeBytes(t), 0)
  const body = `将移入回收站并保留 30 天`
    + (dirCount ? `,其中 ${dirCount} 个文件夹将连同其全部子项一并删除` : '')
    + `;共 ${fmtSize(bytes)}。`
  askConfirm(`删除 ${targets.length} 项?`, body,
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
  // 打包前预估:体积按统一口径累加(目录用服务端下发的子树字节);
  // 条目数是粗估(目录只按直接子项数展开),真正上限由服务端 400 PACK_TOO_LARGE 兜底(§3.10.1)
  let items = 0, bytes = 0
  for (const t of targets) {
    items += Number(t.type) === 1 ? 1 + Number(t.items || 0) : 1
    bytes += nodeBytes(t)
  }
  askConfirm('打包下载?',
    `将打包 <b>${targets.length}</b> 个条目(约 ${items} 项,共 ${fmtSize(bytes)})为 zip 并转入任务面板,完成后可下载。`,
    async () => {
      try {
        const r = await filehubApi.pack(space.value, targets.map(t => t.id))
        if (r && r.task_no) store.queuePackDownload(r.task_no)   // 打包完成自动触发下载
        else store.togglePanel(true)                             // 兜底:进任务面板手动取
      } catch (e) { toast(e.message || '创建打包任务失败', 'err') }
    })
}

// ── 移动 / 复制 ──
const mcDlg = reactive({ show: false, mode: 'move', targets: [] })
function openMove(targets) { if (!targets.length) return; mcDlg.mode = 'move'; mcDlg.targets = targets; mcDlg.show = true }
function openCopy(targets) { if (!targets.length) return; mcDlg.mode = 'copy'; mcDlg.targets = targets; mcDlg.show = true }
function onMcDone(r) {
  mcDlg.show = false
  const what = mcDlg.mode === 'move' ? '移动' : '复制'
  if (r && r.__error) { toast(r.__error.message || '操作失败', 'err'); return }
  // 超阈值时服务端转异步任务,响应里只有 task_no:此时报"0 项成功"会误导用户
  if (r && r.task_no) {
    toast(`${what}已转后台任务,进度见任务面板`, 'ok')
    store.togglePanel(true)
  } else {
    const ok = (r.moved || r.copied || []).length
    const skip = (r.skipped || []).length
    toast(`${what}完成:${ok} 项成功${skip ? `,${skip} 项跳过` : ''}`, 'ok')
  }
  clearSel(); load(); loadSpaces()
}

// ── 详情 ──
const detail = reactive({ show: false, node: null })
async function openDetail(n) {
  try { detail.node = await filehubApi.node(n.id); detail.show = true }
  catch (e) { toast(e.message || '加载详情失败', 'err') }
}

// ── 分享(单选/多选共用;一条分享可绑定多个条目) ──
const shareDlg = reactive({ show: false, nodes: [] })
function openShare(targets) {
  if (!targets.length) return
  shareDlg.nodes = targets
  shareDlg.show = true
}

// ── 上传 ──
const fileInput = ref(null)
const dirInput = ref(null)
function pickFiles() { fileInput.value && fileInput.value.click() }
function pickFolder() { dirInput.value && dirInput.value.click() }

// 上传下拉:「上传」一键两用会让用户猜哪里点哪个,改成下拉由用户明确选(§7.1)
const upMenu = reactive({ show: false, x: 0, y: 0 })
/// 展开/收起上传下拉(锚在按钮左下角)
function toggleUpMenu(e) {
  const r = e.currentTarget.getBoundingClientRect()
  upMenu.x = r.left
  upMenu.y = r.bottom + 6
  upMenu.show = !upMenu.show
}
function closeUpMenu() { upMenu.show = false }
/**
 * 选定上传方式:关菜单并打开对应的系统选择器
 * @param kind  'file' = 上传文件;'dir' = 上传文件夹
 */
function chooseUpload(kind) {
  closeUpMenu()
  if (kind === 'dir') pickFolder()
  else pickFiles()
}
// 上传入队(文件夹/拖入共用):先按相对路径批量建目录(幂等),再按映射把文件派到各自目录
// rels[i] = files[i] 所在目录的相对路径(含顶层文件夹名);dirs = 途中遇到的全部目录(空目录也要建)
async function enqueueWithDirs(files, base, { rels = [], dirs = [] } = {}) {
  const list = [...files]
  if (!list.length && !dirs.length) return
  if (list.length > UPLOAD_MAX_FILES) {
    toast(`单批最多 ${UPLOAD_MAX_FILES} 个文件,请分批上传`, 'warn'); return
  }
  const relOf = (f, i) => rels[i] !== undefined ? rels[i] : (f.webkitRelativePath || '')
  const dirPaths = [...new Set([
    ...dirs,
    ...list.map((f, i) => { const r = relOf(f, i); const k = r.lastIndexOf('/'); return k > 0 ? r.slice(0, k) : '' })
  ].filter(Boolean))]
  // 目录数也要单批校验:服务端 ensure_batch 上限是 2000 条路径,超出会整批 400,
  // 前面的文件一个都传不上去(需求 §3.8.6 单批 ≤1000 个目录)
  if (dirPaths.length > UPLOAD_MAX_DIRS) {
    toast(`单个文件夹最多 ${UPLOAD_MAX_DIRS} 个目录,请分批上传`, 'warn'); return
  }
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
    onDone: () => scheduleRefresh()
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
const qrCanvas = ref(null)   // 二维码画布(模板引用,替代按 id 取 DOM)
// 重置提取码结果弹窗:明文仅此一次,展示 + 一键复制(并自动写入剪贴板)
const pwdBox = reactive({ show: false, name: '', pwd: '' })
// 状态筛选:0=全部 1=有效 2=已取消 3=已失效(服务端按 status 过滤,≤0 不过滤)
// 默认只看"有效":取消/失效的记录会离开列表,避免历史记录把列表撑满
const shareFilter = ref(1)
const SHARE_FILTERS = [[1, '有效'], [2, '已取消'], [3, '已失效'], [0, '全部']]
const shareFilterOptions = SHARE_FILTERS.map(([v, n]) => ({ value: v, label: n }))
// 修改属性里的有效期档位(与创建分享保持一致)
const EDIT_EXPIRES = [{ value: 1, label: '1 天' }, { value: 7, label: '7 天' },
                      { value: 30, label: '30 天' }, { value: 0, label: '永久' }]
// 空间分类:与空间/回收站同款 seg。切空间只重取列表,不动状态筛选
const shareSpace = ref(0)
const shareSel = ref(new Set())
const shareKwInput = ref('')
const shareKeyword = ref('')   // 已生效的关键词(防抖到点才落到这里)
let shareTimer = 0
let shareComposing = false   // 输入法组字中:拼音阶段每个字母都会触发 input,此时不该发请求
let shareSeq = 0               // 请求序号:防抖连打时只认最后一次发出的响应
async function loadShares() {
  const seq = ++shareSeq
  sharesLoading.value = true
  try {
    const d = await filehubApi.shareList({
      status: shareFilter.value || undefined,
      space: shareSpace.value,
      keyword: shareKeyword.value || undefined,
      page: 1, size: 200
    })
    if (seq !== shareSeq) return
    shares.value = d.list || []
    shareSel.value = new Set()   // 整表换了,旧选中不再对应任何行
  } catch (e) {
    if (seq === shareSeq) toast(e.message || '加载分享失败', 'err')
  } finally {
    if (seq === shareSeq) sharesLoading.value = false
  }
}
/// 切空间:只看该空间的分享;状态筛选不动
function switchShareSpace(v) {
  if (Number(shareSpace.value) === Number(v)) return
  shareSpace.value = Number(v)
  loadShares()
}
/// 键入停止后自动搜索(与空间/回收站/传输历史同口径)
function scheduleShareSearch() {
  if (shareComposing) return   // 组字未结束,等 compositionend 再搜
  clearTimeout(shareTimer)
  if (!shareKwInput.value.trim()) { shareKeyword.value = ''; loadShares(); return }
  shareTimer = setTimeout(applyShareSearch, SEARCH_DEBOUNCE_MS)
}
function flushShareSearch() {
  clearTimeout(shareTimer)
  applyShareSearch()
}
function applyShareSearch() {
  clearTimeout(shareTimer)
  const kw = shareKwInput.value.trim()
  if (kw.length > SEARCH_KW_MAX) {
    toast(`关键词最多 ${SEARCH_KW_MAX} 个字符(当前 ${kw.length} 个)`, 'warn')
    return
  }
  shareKeyword.value = kw
  loadShares()
}
function clearShareSearch() {
  clearTimeout(shareTimer)
  shareKwInput.value = ''
  shareKeyword.value = ''
  loadShares()
}
// ── 我的分享:选中与右键(与空间同一套语义) ──
function toggleShareSel(s, e, alwaysToggle) {
  const r = nextSelection(shares.value.map(x => x.id), shareSel.value, s.id, {
    ctrl: !!(e && (e.ctrlKey || e.metaKey)),
    shift: !!(e && e.shiftKey),
    alwaysToggle: !!alwaysToggle,
    lastIdx: shareLastIdx
  })
  shareSel.value = r.selected
  shareLastIdx = r.lastIdx
}
let shareLastIdx = -1
function toggleAllShares(checked) {
  shareSel.value = checked ? new Set(shares.value.map(s => s.id)) : new Set()
  shareLastIdx = -1
}
const shareTargets = computed(() => shares.value.filter(s => shareSel.value.has(s.id)))
// 表头全选态:全部已加载行都在选中集里=全选;部分选中=半选
const allSharesChecked  = computed(() => shares.value.length > 0 && shares.value.every(s => shareSel.value.has(s.id)))
const someSharesChecked = computed(() => !allSharesChecked.value && shares.value.some(s => shareSel.value.has(s.id)))
const shareCtx = reactive({ show: false, x: 0, y: 0, share: null, multi: false })
/**
 * 打开分享右键菜单
 *
 * @param e      触发事件
 * @param s      命中的行(行尾 ⋯ 与行右键都传,空白处不传)
 * @param source row = 右键某行;dots = 行尾 ⋯;area = 列表空白区
 */
function openShareCtx(e, s, source) {
  if (source === 'area' && !shareSel.value.size) return   // 空白处且无选中:没有可操作对象
  if (source !== 'area' && (source === 'dots' || !shareSel.value.has(s.id))) {
    // 行尾 ⋯ = 只操作该行;右键未选中行 = 把它设为唯一选中项
    shareSel.value = new Set([s.id])
    shareLastIdx = -1
  }
  shareCtx.share = source === 'area' ? null : s
  shareCtx.multi = shareSel.value.size > 1
  shareCtx.show = true
  const mh = 220
  shareCtx.x = Math.min(e.clientX, window.innerWidth - 208)
  shareCtx.y = (e.clientY + mh + 8 > window.innerHeight) ? Math.max(8, e.clientY - mh) : e.clientY
}
function closeShareCtx() { shareCtx.show = false }
/// 菜单作用对象:多选 = 选中集;单选 = 命中行,空白处右键时回落到选中集里唯一项
const shareCtxTargets = computed(() => {
  if (shareCtx.multi) return shareTargets.value
  const one = shareCtx.share || shareTargets.value[0] || null
  return one ? [one] : []
})
/// 菜单项可用性:某个动作是否对当前选中集有意义(混选时按"至少有一条可用"显示)
const shareCanCancel = computed(() => shareCtxTargets.value.some(s => Number(s.status) === 1))
const shareCanResume = computed(() => shareCtxTargets.value.some(s => Number(s.status) === 2))
/**
 * 批量取消:只处理"有效"的那些,其余跳过并说明 ——
 * 已取消/已失效的本来就无法取消,混选时不该整批失败
 */
function cancelShares(list) {
  const hit = list.filter(s => Number(s.status) === 1)
  const skip = list.length - hit.length
  if (!hit.length) { toast('选中项里没有可取消的分享', 'warn'); return }
  askConfirm(`取消 ${hit.length} 条分享?`,
    '取消后链接立即失效,可随时恢复(原链接与提取码不变)。'
    + (skip ? `<br>另外 <b>${skip}</b> 条不是有效状态,将跳过。` : ''),
    async () => {
      let ok = 0
      for (const s of hit) {
        try { await filehubApi.shareCancel(s.id); ok++ } catch { /* 单条失败不打断整批 */ }
      }
      toast(`已取消 ${ok} 条` + (ok < hit.length ? `,${hit.length - ok} 条失败` : ''), ok < hit.length ? 'warn' : 'ok')
      loadShares(); loadSpaces()
    })
}
/**
 * 批量删除:任意状态都可删(有效分享删除即链接失效且不可恢复)
 */
/**
 * 批量恢复:只处理"已取消"的那些,其余跳过 ——
 * 有效的本来就有效、已失效的过期/达上限恢复不了
 */
function resumeShares(list) {
  const hit = list.filter(s => Number(s.status) === 2)
  const skip = list.length - hit.length
  if (!hit.length) { toast('选中项里没有可恢复的分享', 'warn'); return }
  askConfirm(`恢复 ${hit.length} 条分享?`,
    '恢复后原链接与提取码继续可用;已过期或已达下载上限的无法恢复。'
    + (skip ? `<br>另外 <b>${skip}</b> 条不是已取消状态,将跳过。` : ''),
    async () => {
      let ok = 0
      for (const s of hit) {
        try { await filehubApi.shareResume(s.id); ok++ } catch { /* 单条失败不打断整批 */ }
      }
      toast(`已恢复 ${ok} 条` + (ok < hit.length ? `,${hit.length - ok} 条失败` : ''), ok < hit.length ? 'warn' : 'ok')
      loadShares(); loadSpaces()
    })
}
/**
 * 批量删除:任意状态都可删(有效分享删除即链接失效且不可恢复)。
 * 走 shares/purge 批量接口,不必逐条发
 */
function purgeShares(list) {
  const live = list.filter(s => Number(s.status) === 1).length
  askConfirm(`删除 ${list.length} 条分享记录?`,
    (live ? `其中 <b>${live}</b> 条<b style="color:var(--color-err)">正在生效</b>,删除后链接立即失效且不可恢复。<br>` : '')
    + '删除的是分享记录,不影响文件本身。只想临时停用请改用「取消」——链接失效但可随时恢复。',
    async () => {
      try {
        const r = await filehubApi.sharePurge({ ids: list.map(s => s.id) })
        toast(`已删除 ${(r && r.purged) || 0} 条记录`, 'ok')
        loadShares(); loadSpaces()
      } catch (e) { toast(e.message || '删除失败', 'err') }
    })
}
// ── 修改分享属性(创建时的那些:名称/有效期/次数/仅登录可见/提取码) ──
const shareEdit = reactive({ show: false, id: 0, name: '', expire_days: 7, max_downloads: 0,
                             login_only: false, pwd_enabled: false, pwd: '',
                             had_pwd: false, busy: false })
function openShareEdit(s) {
  shareEdit.id = s.id
  shareEdit.name = s.name
  // 有效期是"从现在起重新算 N 天",接口收的就是这个语义,所以不回显原到期时间
  shareEdit.expire_days = 7
  shareEdit.max_downloads = Number(s.max_downloads) || 0
  shareEdit.login_only = !!Number(s.login_only)
  shareEdit.pwd_enabled = !!s.has_pwd
  shareEdit.had_pwd = !!s.has_pwd
  shareEdit.pwd = ''
  shareEdit.busy = false
  shareEdit.show = true
}
async function submitShareEdit() {
  if (shareEdit.busy) return
  shareEdit.busy = true
  try {
    const body = {
      name: shareEdit.name.trim(),
      expire_days: Number(shareEdit.expire_days) || 0,
      max_downloads: Number(shareEdit.max_downloads) || 0,
      login_only: shareEdit.login_only ? 1 : 0
    }
    // 提取码:开关关掉 → 清空;开着时手填优先;原本没有、这次也没填的则随机生成一个
    // (已有提取码时留空 = 保持不变;要换新码请用右键菜单的"重置提取码")
    body.pwd_enabled = shareEdit.pwd_enabled ? 1 : 0
    if (shareEdit.pwd_enabled)
    {
      const custom = shareEdit.pwd.trim()
      if (custom) body.pwd = custom
      else if (!shareEdit.had_pwd) body.reset_pwd = true
    }
    const r = await filehubApi.sharePatch(shareEdit.id, body)
    shareEdit.show = false
    if (r && r.pwd) {
      pwdBox.name = shareEdit.name
      pwdBox.pwd = r.pwd
      pwdBox.show = true
      copyText(r.pwd, '新提取码已复制到剪贴板')   // 与单条重置同一体验:顺手复制
    } else {
      toast('已保存分享属性', 'ok')
    }
    loadShares()
  } catch (e) {
    toast(e.message || '保存失败', 'err')
  } finally {
    shareEdit.busy = false
  }
}
// 恢复被取消的分享:重新生效,原链接与提取码继续可用
function resumeShare(s) {
  askConfirm('恢复这条分享?',
    `「${s.name}」将重新变为有效,原链接与提取码继续可用。若它已过期或达下载上限,则无法恢复。`,
    async () => {
      try {
        await filehubApi.shareResume(s.id)
        toast('已恢复分享', 'ok'); loadShares()
      } catch (e) { toast(e.message || '恢复失败', 'err') }
    })
}
// 彻底删除记录(任意状态均可;有效分享删除即链接失效)
function purgeInactive() {
  askConfirm('清空非有效分享记录?',
    '将删除全部「已取消」「已失效」的分享记录,不可恢复(访问日志仍保留)。',
    async () => {
      try {
        const r = await filehubApi.sharePurge({ inactive: true })
        const n = (r && r.purged) || 0
        toast(n ? `已清空 ${n} 条记录` : '没有可清理的记录', n ? 'ok' : 'warn')
        loadShares()
      } catch (e) { toast(e.message || '清空失败', 'err') }
    })
}
// 视图切换:进入"我的分享"拉列表;回到"文件浏览"时重取当前目录与空间计数
// (回收站里的恢复/彻底删除会改变可见列表与配额,不重取会看到过期列表)
watch(view, (v) => {
  if (v === 'shares') loadShares()
  else if (v === 'files') { load(); loadSpaces() }
})
// 回收站内任一写操作完成后的回调:同步刷新空间卡片计数与文件列表
function onTrashChanged() { loadSpaces(); load() }
async function copyText(t, msg) {
  try { await navigator.clipboard.writeText(t); toast(msg || '已复制', 'ok') } catch { /* 剪贴板不可用 */ }
}
async function resetPwd(s) {
  try {
    const r = await filehubApi.sharePatch(s.id, { reset_pwd: true })
    if (!r || !r.pwd) { toast('重置失败:服务端未返回新提取码', 'err'); return }
    pwdBox.name = s.name
    pwdBox.pwd = r.pwd
    pwdBox.show = true
    copyText(r.pwd, '新提取码已复制到剪贴板')   // 顺手复制,省一次手动操作
    loadShares()
  } catch (e) { toast(e.message || '重置失败', 'err') }
}
async function showQr(s) {
  qr.name = s.name
  qr.url = s.url
  qr.show = true
  // 等弹窗渲染完再取画布(用 nextTick,别靠固定 50ms 猜);画不出来要说一声,不要静默
  await nextTick()
  if (!qrCanvas.value) return
  QRCode.toCanvas(qrCanvas.value, s.url, { width: 200, margin: 1 }, (err) => {
    if (err) toast('二维码生成失败,可直接复制链接', 'warn')
  })
}

// ── 排序 ──
function onSort(k) {
  if (sortKey.value === k) sortOrder.value = sortOrder.value === 'asc' ? 'desc' : 'asc'
  else { sortKey.value = k; sortOrder.value = 'asc' }
  query.searching ? doSearch() : load()
}

// ── keep-alive 钩子:回前台立即刷新;切走停轮询(§8.3) ──
let firstActivate = true   // 首访由 onMounted 拉数据,避免同一份数据拉两次
let offChange = null       // onChange 的注销函数
onActivated(() => {
  store.onActivated()
  document.addEventListener('keydown', onKeydown)   // 与 onDeactivated 成对(同函数重复注册无副作用)
  if (firstActivate) { firstActivate = false; return }
  if (view.value === 'files') load()
  loadSpaces()
})
onDeactivated(() => {
  store.onDeactivated()
  closeCtx()
  closeShareCtx()
  closeUpMenu()   // 悬浮菜单须随页面失活关闭(它们被传送到 body,不会自己消失)
  document.removeEventListener('keydown', onKeydown)
  if (refreshTimer) { clearTimeout(refreshTimer); refreshTimer = null }
})
onMounted(() => {
  // 深链:URL 里带着目录位置就先落位,再拉列表(顺序反了会按根目录白拉一次)
  const p0 = queryPos()
  space.value = p0.space
  dirId.value = p0.dirId
  loadSpaces()
  load()
  loadShares()   // 侧栏「我的分享」的计数:不进该标签页也要有值
  offChange = store.onChange(() => { if (view.value === 'files') scheduleRefresh() })
})
</script>

<template>
  <div class="fh-page">
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
            <!-- 传输任务入口:原先做成右下角悬浮球,会盖住列表最后一行右侧的 ⋯ 操作;
                 移进侧栏后不再与任何行重叠,进行中/失败数用徽标常驻可见。
                 它单独占一栏(见下方卡片):上面三项是"切换浏览视图",它是"查看进行中的传输",
                 不是一类东西,挤在同一栏里容易被当成第四个视图 -->
            <button type="button" class="side-entry" :class="{ active: view === 'trash' }" @click="view = 'trash'">
              <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 10h6M10 3h4v3h-4z"/></svg>
              回收站
            </button>
          </div>
          <div class="card" style="padding:6px">
            <button type="button" class="side-entry" :class="{ active: store.panelOpen }"
                    @click="store.togglePanel(!store.panelOpen)">
              <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 3v12m0 0-4-4m4 4 4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>
              传输任务
              <span v-if="store.activeCount" class="cnt num">{{ store.activeCount }}</span>
              <span v-else-if="store.failedCount" class="cnt warn num">{{ store.failedCount }}</span>
            </button>
          </div>
        </aside>

        <!-- 主区 -->
        <!-- 回收站/我的分享没有自己的滚动容器,交给主区滚;文件浏览则把高度让给列表 -->
        <div class="fh-main" :class="{ 'fh-main-scroll': view !== 'files' }">
          <!-- 文件浏览 -->
          <template v-if="view === 'files'">
            <!-- 工具条 / 批量统计条 -->
            <div v-if="selected.size < 2" class="card fh-toolbar">
              <div class="seg" style="width:196px">
                <button v-for="s in spaces" :key="s.space" type="button" class="seg-item"
                        :class="{ active: space === s.space }" @click="switchSpace(s.space)">{{ s.name }}</button>
              </div>
              <div class="input-wrap fh-search grow">
                <input v-model.trim="searchInput" class="input" style="height:38px" :maxlength="SEARCH_KW_MAX"
                       placeholder="搜索当前目录及子目录…" @compositionstart="searchComposing = true" @compositionend="searchComposing = false; scheduleSearch()" @input="scheduleSearch" @keyup.enter="flushSearch" />
                <!-- 键入停止即自动搜索,故不需要"搜索"按钮;留个清空按钮,一键退出搜索 -->
                <span v-if="searchInput" class="input-suffix">
                  <button class="icon-btn" type="button" aria-label="清空搜索" title="清空" @click="clearSearch">
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
                  </button>
                </span>
              </div>
              <button class="btn btn-grad" type="button" @click="toggleUpMenu"
                      aria-haspopup="menu" :aria-expanded="upMenu.show">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 16V4m0 0-5 5m5-5 5 5"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>
                上传
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" style="margin-left:3px"><path d="m6 9 6 6 6-6"/></svg>
              </button>
              <button class="btn btn-secondary" type="button" @click="openMkdir">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M12 10v6M9 13h6"/></svg>
                新建
              </button>
              <div class="vseg">
                <button type="button" :class="{ on: layout === 'list' }" title="列表视图" aria-label="列表视图" @click="layout = 'list'">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>
                </button>
                <button type="button" :class="{ on: layout === 'grid' }" title="网格视图" aria-label="网格视图" @click="layout = 'grid'">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></svg>
                </button>
              </div>
              <button class="icon-btn" type="button" title="刷新" aria-label="刷新" @click="query.searching ? doSearch() : load()">
                <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 3v6h-6"/></svg>
              </button>
            </div>
            <!-- 批量统计条:仅多选(≥2 项)时替换工具条;操作统一走右键菜单,这里只做汇总与取消
                 复用 .fh-toolbar 的最小高度,避免替换工具条时下方列表跳动 -->
            <div v-else class="card fh-toolbar batch-bar">
              <button class="icon-btn" type="button" aria-label="取消选择" @click="clearSel">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
              </button>
              <span class="sel-num num">已选 {{ selected.size }} 项</span>
              <span v-if="selSummary" class="cap num" style="color:var(--color-text-3)">{{ selSummary }}</span>
              <span style="flex:1"></span>
              <span class="cap" style="color:var(--color-text-3)">在列表内右键,可对选中项执行操作</span>
            </div>

            <!-- 搜索提示条 -->
            <div v-if="query.searching" class="card fh-toolbar" style="padding:8px 16px">
              <span class="badge badge-warn" v-if="query.truncated">结果过多,已截断至 500 条,请缩小范围</span>
              <span class="cap" style="color:var(--color-text-2)">搜索"{{ searchInput }}" · 当前目录及全部子目录 · 共 <b class="num">{{ total }}</b> 条</span>
              <span style="flex:1"></span>
              <button class="btn btn-ghost btn-sm" type="button" @click="exitSearch">退出搜索</button>
            </div>

            <!-- 列表 -->
            <div class="card fh-listcard" style="padding:0;overflow:hidden;position:relative">
              <div v-if="!query.searching" class="crumbs">
                <button type="button" @click="goDir(0)">
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
                  {{ curSpace ? curSpace.name : '空间' }}
                </button>
                <template v-for="b in breadcrumb" :key="b.id">
                  <svg class="sep" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="m9 6 6 6-6 6"/></svg>
                  <button type="button" :class="{ here: b.id === dirId }" @click="goDir(b.id)">{{ b.name }}</button>
                </template>
                <span style="flex:1"></span>
                <span class="cap num" style="color:var(--color-text-3)">共 {{ total }} 项</span>
              </div>
              <FileList :items="items" :selected="selected" :keyword="query.searching ? searchInput : ''"
                        :show-path="query.searching" :sort="sortKey" :order="sortOrder" :view="layout"
                        :loading="loading" :has-more="items.length < total" :epoch="listEpoch"
                        :dir-id="dirId" :space="space"
                        @toggle="toggleSel" @open="openNode" @ctx="openCtx" @sort="onSort"
                        @drag-to="onDragTo" @files="onListFiles" @load-more="loadMore"
                        @toggle-all="onToggleAll" />
            </div>
          </template>

          <!-- 回收站 -->
          <TrashView v-else-if="view === 'trash'" :space="space" :me-space="meSpace" :spaces="spaces"
                     @changed="onTrashChanged" @space="switchTrashSpace" />

          <!-- 我的分享 -->
          <template v-else-if="view === 'shares'">
            <!-- 工具条 / 批量条:与空间、回收站同款。顺序按需求:空间分类 → 状态 → 搜索 → 清空非有效 -->
            <div v-if="shareSel.size < 2" class="card fh-toolbar">
              <div class="seg" style="width:196px">
                <button v-for="sp in spaces" :key="sp.space" type="button" class="seg-item"
                        :class="{ active: Number(shareSpace) === Number(sp.space) }"
                        @click="switchShareSpace(sp.space)">{{ sp.name }}</button>
              </div>
              <!-- 前缀做进控件内:整块是一个可点区域,文案随选项变(状态:有效 / 状态:全部) -->
              <ZmSelect v-model="shareFilter" prefix="状态:" :options="shareFilterOptions"
                        style="min-width:132px" @change="loadShares()" />
              <div class="input-wrap fh-search grow">
                <input v-model.trim="shareKwInput" class="input" style="height:38px" :maxlength="SEARCH_KW_MAX"
                       placeholder="搜索分享名称…" @compositionstart="shareComposing = true" @compositionend="shareComposing = false; scheduleShareSearch()" @input="scheduleShareSearch" @keyup.enter="flushShareSearch" />
                <span v-if="shareKwInput" class="input-suffix">
                  <button class="icon-btn" type="button" aria-label="清空搜索" title="清空" @click="clearShareSearch">
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
                  </button>
                </span>
              </div>
              <button class="btn btn-secondary" type="button" @click="purgeInactive">清空非有效记录</button>
            </div>
            <div v-else class="card fh-toolbar batch-bar">
              <button class="icon-btn" type="button" aria-label="取消选择" @click="shareSel = new Set(); shareLastIdx = -1">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
              </button>
              <span class="sel-num num">已选 {{ shareSel.size }} 项</span>
              <span style="flex:1"></span>
              <span class="cap" style="color:var(--color-text-3)">在列表内右键,可对选中项执行操作</span>
            </div>

            <div class="card" style="padding:0;overflow:hidden">
              <div style="overflow-x:auto">
                <table class="ftbl">
                  <thead>
                    <tr>
                      <th class="col-cb">
                        <input type="checkbox" class="fcheck" :checked="allSharesChecked"
                               :indeterminate="someSharesChecked" aria-label="全选"
                               @change="toggleAllShares($event.target.checked)" />
                      </th>
                      <th style="width:200px">名称</th>
                      <th style="width:70px">类型</th>
                      <th style="width:190px">链接</th>
                      <th style="width:84px">提取码</th>
                      <th style="width:110px">有效期</th>
                      <th style="width:66px">浏览</th>
                      <th style="width:80px">下载</th>
                      <th style="width:80px">状态</th>
                      <th style="width:60px"></th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-if="sharesLoading">
                      <td :colspan="10" style="padding:16px">
                        <div v-for="i in 4" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
                      </td>
                    </tr>
                    <template v-else>
                      <!-- 行操作全走右键/行尾 ⋯(与空间、回收站一致),状态用整行降透明区分 -->
                      <tr v-for="s in shares" :key="s.id" :class="{ sel: shareSel.has(s.id) }"
                          :style="Number(s.status) !== 1 ? 'opacity:.62' : ''"
                          @click="toggleShareSel(s, $event)"
                          @contextmenu.prevent.stop="openShareCtx($event, s, 'row')">
                        <td class="col-cb" @click.stop @dblclick.stop>
                          <input type="checkbox" class="fcheck" :checked="shareSel.has(s.id)"
                                 @click.stop="toggleShareSel(s, $event, true)" :aria-label="`选择 ${s.name}`" />
                        </td>
                        <!-- 名称列只显示展示名本身:绑了几项在"类型"列已有(「N 项」),
                             再往名字后面缀一句会把自定义展示名改样 —— 用户填什么就显示什么 -->
                        <td><span class="fname" style="font-weight:600">{{ s.name }}</span></td>
                        <td>
                          <!-- 多选分享按条目数显示「N 项」(单条仍按类型) -->
                          <span class="ftype">{{ Number(s.node_count) > 1 ? `${s.node_count} 项` : (Number(s.node_type) === 1 ? '文件夹' : '文件') }}</span>
                        </td>
                        <td><span class="share-url-cell">{{ s.url.replace(/^https?:\/\//, '') }}</span></td>
                        <td>
                          <span class="badge badge-dim"
                                :title="s.has_pwd ? '出于安全考虑,原提取码无法查看;可在右键菜单里重置' : '该分享无需提取码'">
                            {{ s.has_pwd ? '已设置' : '未设置' }}
                          </span>
                        </td>
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
                            <button class="dots-btn" type="button" aria-label="更多操作" @click.stop="openShareCtx($event, s, 'dots')">
                              <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="5" r="1.7"/><circle cx="12" cy="12" r="1.7"/><circle cx="12" cy="19" r="1.7"/></svg>
                            </button>
                          </span>
                        </td>
                      </tr>
                      <tr v-if="!shares.length">
                        <td :colspan="10">
                          <div class="empty">
                            <div class="empty-icon">🔗</div>
                            <div class="empty-title">{{ shareKeyword ? '没有匹配的分享' : (Number(shareFilter) === 1 ? '暂无有效分享' : '没有符合条件的记录') }}</div>
                            <div class="empty-sub">{{ shareKeyword ? '换个关键词试试' : (Number(shareFilter) === 1 ? '在文件上右键选择"分享"即可创建' : '换个状态或空间看看') }}</div>
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

    <!-- 悬浮菜单(上传下拉 / 右键菜单):传送到 body 以免被容器裁剪;
         两者都必须在离开本页时关闭(onDeactivated),否则会随 keep-alive 缓存残留到其它页面 -->
    <Teleport to="body">
      <!-- 上传下拉:全屏透明层负责"点外面关闭" -->
      <div v-if="upMenu.show" style="position:fixed;inset:0;z-index:880"
           @click="closeUpMenu" @contextmenu.prevent="closeUpMenu"></div>
      <div v-if="upMenu.show" class="menu" style="position:fixed;z-index:881;min-width:170px"
           :style="{ left: upMenu.x + 'px', top: upMenu.y + 'px' }" role="menu">
        <button class="menu-item" type="button" role="menuitem" @click="chooseUpload('file')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/></svg>
          上传文件
        </button>
        <button class="menu-item" type="button" role="menuitem" @click="chooseUpload('dir')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
          上传文件夹
        </button>
      </div>

      <div v-if="ctxMenu.show" style="position:fixed;inset:0;z-index:890" @click="closeCtx" @contextmenu.prevent="closeCtx"></div>
      <!-- 空白处菜单(无选中项时右键列表空白区):作用于"当前位置",当前只有"返回上一级目录" -->
      <div v-if="ctxMenu.show && ctxMenu.blank" class="menu" style="position:fixed;z-index:891;min-width:200px" :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
        <button class="menu-item" type="button" :disabled="!dirId" @click="goParent">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 19V5m0 0-6 6m6-6 6 6"/></svg>返回上一级目录</button>
      </div>
      <div v-if="ctxMenu.show && !ctxMenu.blank" class="menu" style="position:fixed;z-index:891;min-width:200px" :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
        <!-- 首项按状态区分:单选目录=打开、单选文件=下载、多选=打包下载 -->
        <button v-if="!ctxMenu.multi && ctxIsDir" class="menu-item" type="button" @click="ctxAct('open')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>打开</button>
        <button v-if="!ctxMenu.multi && !ctxIsDir" class="menu-item" type="button" @click="ctxAct('download')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 4v12m0 0 4-4m-4 4-4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>下载</button>
        <!-- 打包下载只给"多选"和"单选文件夹":单文件直接下载更直接,不必绕一层 zip -->
        <button v-if="ctxMenu.multi || ctxIsDir" class="menu-item" type="button" @click="ctxAct('pack')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/></svg>打包下载</button>
        <div class="menu-sep"></div>
        <button class="menu-item" type="button" @click="ctxAct('rename')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/></svg>{{ ctxMenu.multi ? '批量重命名' : '重命名' }}</button>
        <button class="menu-item" type="button" @click="ctxAct('move')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M5 12h14m0 0-5-5m5 5-5 5"/></svg>移动</button>
        <button class="menu-item" type="button" @click="ctxAct('copy')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>复制</button>
        <!-- 分享:单选一条 / 多选一条分享绑定全部选中项 -->
        <button class="menu-item" type="button" @click="ctxAct('share')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 13.5 6.8 4M15.4 6.5l-6.8 4"/></svg>分享</button>
        <div class="menu-sep"></div>
        <button class="menu-item danger" type="button" @click="ctxAct('delete')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>删除</button>
        <div class="menu-sep"></div>
        <button class="menu-item" type="button" @click="ctxAct('detail')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>详情</button>
      </div>

      <!-- 我的分享:右键菜单(单选 = 按状态给原有动作 + 修改属性;多选 = 取消 + 删除) -->
      <div v-if="shareCtx.show" style="position:fixed;inset:0;z-index:890"
           @click="closeShareCtx" @contextmenu.prevent="closeShareCtx"></div>
      <div v-if="shareCtx.show" class="menu" style="position:fixed;z-index:891;min-width:200px"
           :style="{ left: shareCtx.x + 'px', top: shareCtx.y + 'px' }">
        <template v-if="!shareCtx.multi && shareCtxTargets.length === 1">
          <template v-if="Number(shareCtxTargets[0].status) === 1">
            <button class="menu-item" type="button" @click="closeShareCtx(); copyText(shareCtxTargets[0].url, '链接已复制')">
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>复制链接</button>
            <button class="menu-item" type="button" @click="closeShareCtx(); showQr(shareCtxTargets[0])">
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="4" width="6" height="6" rx="1"/><rect x="14" y="4" width="6" height="6" rx="1"/><rect x="4" y="14" width="6" height="6" rx="1"/><path d="M14 14h3v3h-3zM20 20h-3"/></svg>二维码</button>
            <button v-if="shareCtxTargets[0].has_pwd" class="menu-item" type="button" @click="closeShareCtx(); resetPwd(shareCtxTargets[0])">
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/></svg>重置提取码</button>
            <div class="menu-sep"></div>
          </template>
          <button class="menu-item" type="button" @click="closeShareCtx(); openShareEdit(shareCtxTargets[0])">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/></svg>修改属性</button>
          <div class="menu-sep"></div>
          <button v-if="Number(shareCtxTargets[0].status) === 1" class="menu-item" type="button" @click="closeShareCtx(); cancelShares([shareCtxTargets[0]])">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M8 12h8"/></svg>取消</button>
          <button v-else-if="Number(shareCtxTargets[0].status) === 2" class="menu-item" type="button" @click="closeShareCtx(); resumeShare(shareCtxTargets[0])">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/></svg>恢复</button>
          <button class="menu-item danger" type="button" @click="closeShareCtx(); purgeShares([shareCtxTargets[0]])">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>删除</button>
        </template>
        <template v-else>
          <!-- 多选:只给批量有意义的两个。取消只作用于其中"有效"的,其余自动跳过 -->
          <button v-if="shareCanCancel" class="menu-item" type="button" @click="closeShareCtx(); cancelShares(shareCtxTargets)">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M8 12h8"/></svg>取消 {{ shareCtxTargets.length }} 条</button>
          <button v-else-if="shareCanResume" class="menu-item" type="button" @click="closeShareCtx(); resumeShares(shareCtxTargets)">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/></svg>恢复 {{ shareCtxTargets.length }} 条</button>
          <button class="menu-item danger" type="button" @click="closeShareCtx(); purgeShares(shareCtxTargets)">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>删除 {{ shareCtxTargets.length }} 条</button>
        </template>
      </div>

      <!-- 修改分享属性:字段、顺序、控件形态与创建分享(ShareDialog)对齐,只保留两处语义差异 ——
           有效期是"从现在起重新算 N 天"而非改原到期日,提取码留空是"保持不变"而非随机生成 -->
      <Modal :show="shareEdit.show" title="修改分享属性" @close="shareEdit.show = false">
        <div class="form-item">
          <label class="form-label">分享名称 <span class="opt">分享页上显示的标题</span></label>
          <input v-model.trim="shareEdit.name" class="input" maxlength="255" placeholder="留空则沿用当前名称" />
          <span class="form-hint">只影响分享页标题与列表,不改文件名</span>
        </div>
        <!-- 提取码开关常显(原本没有的能加上、原本有的能去掉)。
             用与创建分享同款的开关 + 固定文案:文案不随状态变字 ——
             写成"免提取码访问"会被读成"点了就不要密码",那描述的是点之后的结果,不是当前状态 -->
        <div class="form-item">
          <label class="form-label">提取码 <span class="opt">开启后访问需输入 4 位码</span></label>
          <div class="row between" style="background:var(--color-bg);border:1px solid var(--color-border-soft);border-radius:var(--r-md);padding:10px 14px">
            <span style="font-size:var(--fs-cap);color:var(--color-text-2)">需要提取码才能访问</span>
            <button type="button" class="switch" :class="{ on: shareEdit.pwd_enabled }" aria-label="提取码开关"
                    @click="shareEdit.pwd_enabled = !shareEdit.pwd_enabled"></button>
          </div>
          <span class="form-hint">要换新码用右键菜单的「重置提取码」</span>
        </div>
        <div v-if="shareEdit.pwd_enabled" class="form-item" style="margin-top:-6px">
          <label class="form-label">自定义提取码 <span class="opt">4 个字符;留空保持不变</span></label>
          <input v-model.trim="shareEdit.pwd" class="input" maxlength="4"
                 :placeholder="shareEdit.had_pwd ? '留空保持不变' : '留空则由服务端随机生成'" />
        </div>
        <!-- flex-start:右列没有提示文字,默认的 center 会把它整体压低,两列的标签就不在同一水平线 -->
        <div class="row" style="gap:12px;flex-wrap:wrap;align-items:flex-start">
          <div class="form-item" style="flex:1;min-width:170px">
            <label class="form-label">有效期</label>
            <ZmSelect v-model="shareEdit.expire_days" :options="EDIT_EXPIRES" />
            <span class="form-hint" style="color:var(--color-warn)">选完从现在起重新计时</span>
          </div>
          <div class="form-item" style="flex:1;min-width:170px">
            <label class="form-label">下载次数上限 <span class="opt">0 = 不限</span></label>
            <input v-model.number="shareEdit.max_downloads" class="input" type="number" min="0" step="1" />
          </div>
        </div>
        <div v-if="Number(shareSpace) !== 0" class="row between"
             style="background:var(--color-bg);border:1px solid var(--color-border-soft);border-radius:var(--r-md);padding:10px 14px">
          <div>
            <b style="font-size:var(--fs-body)">仅登录可见</b>
            <div class="form-hint">开启后未登录访客先跳登录,登录后回到分享页</div>
          </div>
          <button type="button" class="switch" :class="{ on: shareEdit.login_only }" aria-label="仅登录可见开关"
                  @click="shareEdit.login_only = !shareEdit.login_only"></button>
        </div>
        <template #foot>
          <button class="btn btn-ghost" type="button" @click="shareEdit.show = false">取消</button>
          <button class="btn btn-primary" type="button" :disabled="shareEdit.busy" @click="submitShareEdit">保存</button>
        </template>
      </Modal>
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

    <!-- 批量重命名(多选):左列原名只读、右列编辑目标名;扩展名变更逐行提示 -->
    <Modal :show="batchRename.show" title="批量重命名" @close="batchRename.show = false">
      <p class="form-hint" style="margin:0 0 10px">
        共 {{ batchRename.rows.length }} 项,仅提交被修改的名称;重名冲突的行会单独报告失败。
      </p>
      <div style="max-height:46vh;overflow-y:auto;display:flex;flex-direction:column;gap:10px;padding-right:4px">
        <div v-for="r in batchRename.rows" :key="r.id" class="row" style="gap:10px;align-items:flex-start">
          <div style="flex:1;min-width:0">
            <div class="form-label" style="margin:0">原名称</div>
            <div style="font-size:var(--fs-cap);word-break:break-all;line-height:34px;color:var(--color-text-2)">{{ r.old }}</div>
          </div>
          <div style="flex:1;min-width:0">
            <div class="form-label" style="margin:0">重命名为</div>
            <input v-model.trim="r.name" class="input" maxlength="255" />
            <span v-if="extWarn(r)" class="form-hint" style="color:var(--color-warn)">⚠ 扩展名已变更,文件可能无法被原程序打开</span>
          </div>
        </div>
      </div>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="batchRename.show = false">取消</button>
        <button class="btn btn-primary" type="button" :disabled="batchRename.busy" @click="submitBatchRename">
          {{ batchRename.busy ? '提交中…' : '重命名' }}
        </button>
      </template>
    </Modal>

    <!-- 批量详情(多选):逐条字段没有公共值,只汇总条目数与总体积 -->
    <Modal :show="batchDetail.show" title="批量详情" @close="batchDetail.show = false">
      <table class="table" style="font-size:var(--fs-cap)">
        <tbody>
          <tr><td style="color:var(--color-text-3);width:80px">条目数</td>
              <td><b class="num">{{ batchDetail.count }}</b> 项<span v-if="batchDetail.dirs">(含 {{ batchDetail.dirs }} 个文件夹)</span></td></tr>
          <tr><td style="color:var(--color-text-3)">总大小</td><td class="num">{{ fmtSize(batchDetail.bytes) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">名称</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">类型</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">路径</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">创建者</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">创建时间</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">修改时间</td><td>—</td></tr>
        </tbody>
      </table>
      <p class="form-hint" style="margin-top:10px">多选时无法逐条展示名称、时间等字段,这里只汇总条目数与总体积(文件夹按子树字节计入)。</p>
      <template #foot>
        <button class="btn btn-primary" type="button" @click="batchDetail.show = false">关闭</button>
      </template>
    </Modal>

    <!-- 移动 / 复制 -->
    <MoveCopyDialog :show="mcDlg.show" :mode="mcDlg.mode" :targets="mcDlg.targets" :me-space="meSpace"
                    :space="space"
                    @close="mcDlg.show = false" @done="onMcDone" />

    <!-- 分享 -->
    <ShareDialog :show="shareDlg.show" :nodes="shareDlg.nodes" :space="Number(space)"
                 @close="shareDlg.show = false" @created="loadShares" />

    <!-- 详情 -->
    <Modal :show="detail.show" title="条目详情" @close="detail.show = false">
      <table v-if="detail.node" class="table" style="font-size:var(--fs-cap)">
        <tbody>
          <tr><td style="color:var(--color-text-3);width:80px">名称</td><td><b>{{ detail.node.name }}</b></td></tr>
          <tr><td style="color:var(--color-text-3)">类型</td><td>{{ FILE_KINDS[kindOf(detail.node)].label }}</td></tr>
          <tr><td style="color:var(--color-text-3)">大小</td><td class="num">{{ fmtNodeSize(detail.node) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">路径</td><td>{{ detail.node.path || '(空间根)' }}</td></tr>
          <tr><td style="color:var(--color-text-3)">创建者</td><td>{{ detail.node.owner_name }}</td></tr>
          <tr><td style="color:var(--color-text-3)">创建时间</td><td class="num">{{ fmtTime(detail.node.create_time) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">修改时间</td><td class="num">{{ fmtTime(detail.node.update_time) }}</td></tr>
        </tbody>
      </table>
      <!-- 纯展示弹窗也给个关闭位:只靠右上角 × 的话,底部没有落点,手会不知道往哪放 -->
      <template #foot>
        <button class="btn btn-primary" type="button" @click="detail.show = false">关闭</button>
      </template>
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
        <div class="qr-box" style="width:216px;height:216px;padding:8px"><canvas ref="qrCanvas" width="200" height="200"></canvas></div>
        <span class="share-url" style="max-width:280px">{{ qr.url }}</span>
        <button class="btn btn-secondary btn-sm" type="button" @click="copyText(qr.url, '链接已复制')">复制链接</button>
      </div>
    </Modal>

    <!-- 重置提取码结果(明文只此一次):展示 + 复制 -->
    <Modal :show="pwdBox.show" title="提取码已重置" @close="pwdBox.show = false">
      <p style="margin:0 0 12px;line-height:22px;color:var(--color-text-2)">
        「{{ pwdBox.name }}」的新提取码如下,<b>旧提取码已立即失效</b>:
      </p>
      <div class="row" style="gap:10px;align-items:center">
        <span class="share-url num" style="flex:1;text-align:center;font-size:22px;letter-spacing:6px;font-weight:700;padding:12px">{{ pwdBox.pwd }}</span>
        <button class="btn btn-primary" type="button" @click="copyText(pwdBox.pwd, '提取码已复制')">复制</button>
      </div>
      <p class="form-hint" style="margin-top:10px">已自动复制到剪贴板。提取码仅本次展示,请及时保存,关闭后无法再次查看。</p>
      <template #foot>
        <button class="btn btn-grad" type="button" @click="pwdBox.show = false">完成</button>
      </template>
    </Modal>

    <!-- 任务面板(右下角) -->
    <TaskPanel />
  </div>
</template>

<style scoped>
.page-sub{color:var(--color-text-2);font-size:var(--fs-body);margin:6px 0 0}
</style>
