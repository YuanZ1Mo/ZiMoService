<script setup>
// 免登录分享页 /s/:token(meta.public,不进门户壳,§7.5)
// 状态:加载 → 提取码校验 / 需登录 → 浏览(只读)/ 失效
// 浏览区与空间文件区共用 FileList:同一套列表/网格双视图、虚拟滚动、选中手势、行尾 ⋯ 与右键菜单。
// 差别只在"能做什么" —— 这里整页只读,菜单里只有打开 / 下载 / 打包下载 / 详情。
// 打包下载:服务端返回 {task_no} 表示压缩中,前端每 2s 重试同一请求直至返回 {url}
// (免登录无任务面板;上限约 60s,超时提示失败)
import { ref, reactive, computed, onMounted, onUnmounted, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { filehubApi, downloadByUrl, fmtSize, fmtNodeSize, fmtTime, kindLabel, nodeBytes, nextSelection } from '../api/filehub'
import FileList from '../modules/filehub/FileList.vue'
import Modal from '../components/Modal.vue'
import ThemeSwitch from '../components/ThemeSwitch.vue'
import '../modules/filehub/filehub.css'

const route = useRoute()
const token = computed(() => String(route.params.token || ''))

const state = ref('loading')   // loading | pwd | browse | need_login | expired | unavailable | notfound
const info = reactive({ name: '', node_type: 1, multi: false, node_count: 0, hidden_count: 0, nodes: [], owner_name: '', expire_time: 0, max_downloads: 0, download_count: 0, node_id: 0 })
const pwd = ref('')
const pwdErr = ref('')
const pwdBusy = ref(false)

async function loadInfo() {
  state.value = 'loading'
  // 位置来自 URL:刷新、或从浏览器历史回退到本页时,停在原来那一层而不是弹回顶层
  dirId.value = queryDir()
  try {
    const d = await filehubApi.shareInfo(token.value)
    Object.assign(info, d)
    if (d.need_pwd) { state.value = 'pwd'; return }
    state.value = 'browse'
    // 目录分享(单目录 / 多条目)要拉当前目录;单文件分享就一行,由 singleItems 直接给出
    if (isDirShare.value) load()
  } catch (e) {
    if (e.status === 401 || e.code === 'NEED_LOGIN') { state.value = 'need_login'; return }
    if (e.code === 'SHARE_EXPIRED') { state.value = 'expired'; return }
    if (e.code === 'SHARE_UNAVAILABLE') { state.value = 'unavailable'; return }
    state.value = 'notfound'
  }
}
async function verify() {
  if (!pwd.value.trim()) return
  pwdBusy.value = true; pwdErr.value = ''
  try {
    const r = await filehubApi.shareVerify(token.value, { pwd: pwd.value })
    if (r.pass) { state.value = 'browse'; if (isDirShare.value) load() }
    else pwdErr.value = '提取码不正确,请重试'
  } catch (e) {
    if (e.code === 'SHARE_LOCKED') pwdErr.value = '错误次数过多,请 10 分钟后再试'
    else pwdErr.value = e.message || '校验失败'
  } finally { pwdBusy.value = false }
}
function goLogin() {
  location.href = `/login?redirect=${encodeURIComponent('/s/' + token.value)}`
}

/// 目录分享(单目录 / 多条目)才有列表可浏览
const isDirShare = computed(() => !!info.multi || Number(info.node_type) === 1)

// ── 列表状态(与空间文件区同一套) ──
const pageSize = 200
const SEARCH_KW_MAX = 64
const SEARCH_DEBOUNCE_MS = 400   // 键入停止多久后自动搜索(与空间页同一个手感)
const dirId = ref(0)
const breadcrumb = ref([])
const items = ref([])
const total = ref(0)
const listEpoch = ref(0)     // 整表被替换时自增,列表据此复位滚动位置
const loadingList = ref(false)
const layout = ref('list')   // list | grid
const sortKey = ref('name')
const sortOrder = ref('asc')
const selected = ref(new Set())
let lastIdx = -1
let loadSeq = 0              // 列表请求序号(丢弃过期响应)
const query = reactive({ keyword: '', searching: false, truncated: false })
const hasMore = computed(() => isDirShare.value && items.value.length < total.value)
/**
 * 单文件分享:把分享目标当成列表里的唯一一行
 *
 * 这样详情、行尾 ⋯、右键菜单与目录分享共用同一套,不必再为它单独写一种样式。
 */
const singleItems = computed(() => {
  if (isDirShare.value) return []
  const n = (info.nodes || [])[0]
  if (n) return [n]
  return info.node_id ? [{ id: info.node_id, type: 2, name: info.name, size: 0 }] : []
})
/// 列表当下显示的内容:目录分享 = 当前目录条目;单文件分享 = 它自己一行
const listItems = computed(() => (isDirShare.value ? items.value : singleItems.value))

async function load(append = false) {
  if (!isDirShare.value) return
  // 换目录/搜索/刷新会并发发请求,回来晚的旧响应必须丢弃,否则会把上一批行拼进当前列表
  const seq = ++loadSeq
  loadingList.value = true
  try {
    const d = await filehubApi.shareListDir(token.value, {
      dir_id: dirId.value,
      keyword: query.searching ? query.keyword : '',
      sort: sortKey.value, order: sortOrder.value,
      page: append ? Math.ceil(items.value.length / pageSize) + 1 : 1, size: pageSize
    })
    if (seq !== loadSeq) return
    items.value = append ? items.value.concat(d.list || []) : (d.list || [])
    if (!append) {
      listEpoch.value++
      // 顺带清理选中集:旧条目可能已不在本页,留着会出现"已选 N 项、实际操作 0 项"的错位
      if (selected.value.size)
        selected.value = new Set([...selected.value].filter(id => items.value.some(x => x.id === id)))
      lastIdx = -1
    }
    total.value = d.total || 0
    query.truncated = !!d.truncated
    breadcrumb.value = d.breadcrumb || []
  } catch (e) {
    if (seq !== loadSeq) return
    // 分享在读的过程中失效/不可用:整页切态,而不是继续显示一张空表
    if (e.code === 'SHARE_UNAVAILABLE') state.value = 'unavailable'
    else if (e.code === 'SHARE_EXPIRED') state.value = 'expired'
    // 凭证在浏览途中过期(2 小时):打回提取码那一步,让人重新输入
    else if (e.code === 'NEED_PWD') state.value = 'pwd'
    // 会话在浏览途中失效(而这条分享是"仅登录可见"):回到"去登录"那一步
    else if (e.code === 'NEED_LOGIN' || e.status === 401) state.value = 'need_login'
    // URL 里带的那一层已经没了(旧链接、或目录被删):退回分享顶层,而不是停在一张空表上
    else if (e.code === 'NODE_NOT_FOUND' && dirId.value) {
      toastErr('这个位置已不存在,已回到分享顶层')
      goDir(0)
    }
    else toastErr(e.message || '加载分享内容失败')
  } finally { if (seq === loadSeq) loadingList.value = false }
}
function loadMore() { if (hasMore.value && !loadingList.value) load(true) }
function refresh() { query.searching ? doSearch() : load() }

// ── 目录导航(前进/后退交给 vue-router 的历史栈) ──
// 位置写进 URL 查询串(/s/<token>?dir=<id>),换目录就是一次真正的路由跳转 —— 鼠标侧键、
// Alt+←/→、浏览器返回键全部由 vue-router 处理,本页不自己记账。附带好处:分享内的位置
// 可分享、刷新后仍停在原处。
const router = useRouter()
/// @return 当前 URL 查询串所代表的目录位置
function queryDir() { return Number(route.query.dir || 0) }
/**
 * 把当前目录同步进 URL 查询串
 *
 * 已在位时直接返回 —— 不比较的话,本页自己发起的那次跳转回来还会再触发一次回放。
 */
function syncQuery() {
  if (queryDir() === dirId.value) return
  const q = { ...route.query }
  if (dirId.value) q.dir = String(dirId.value)
  else delete q.dir
  router.push({ query: q })
}
// 浏览器前进/后退(含鼠标侧键、Alt+方向键)落回本页时,按 URL 回放目录位置
watch(() => route.query.dir, () => {
  // 已经离开本页时路由也会变,别在别的页面上重载分享内容
  if (!route.path.startsWith('/s/')) return
  const p = queryDir()
  if (p === dirId.value) return   // 本页自己发起的那次
  // 还没进浏览态(提取码/需登录/失效):先记下位置,等真正列内容时再用
  if (state.value !== 'browse') { dirId.value = p; return }
  // 目录要变了,搜索态与选中集都不再适用;清搜索态但不单独发请求,下面统一 load 一次
  clearTimeout(searchTimer)
  query.searching = false; query.keyword = ''; query.truncated = false; searchInput.value = ''
  dirId.value = p
  clearSel()
  load()
})
/// @param id 目标目录;0 = 分享顶层(多条目分享的虚拟根 / 单条目分享的分享根)
function goDir(id) {
  const target = Number(id) || 0
  if (target === dirId.value && !query.searching) return
  // 搜索结果是跨子目录的,换目录后关键词与结果都不再适用,先退出搜索
  if (query.searching) { clearTimeout(searchTimer); query.searching = false; query.keyword = ''; query.truncated = false; searchInput.value = '' }
  dirId.value = target
  clearSel()
  syncQuery()
  load()
}
/// 返回上一级目录:面包屑 = 祖先链 + 当前目录,倒数第二级即父目录;已在顶层时无上级
function goParent() {
  closeCtx()
  if (!dirId.value) return
  const bc = breadcrumb.value
  goDir(bc.length >= 2 ? Number(bc[bc.length - 2].id) : 0)
}

// ── 搜索(搜当前目录及子目录,与空间页同一套) ──
const searchInput = ref('')
let searchTimer = 0
let searchComposing = false   // 输入法组字中:拼音阶段每个字母都会触发 input,此时不该发请求
/**
 * 键入停止后自动搜索(防抖)
 *
 * 输入框不配"搜索"按钮,靠这个间隔把连续敲键合并成一次请求。
 * 清空关键词即时退出搜索、不等防抖 —— 否则列表会先空一下再回来。
 */
function scheduleSearch() {
  if (searchComposing) return   // 组字未结束,等 compositionend 再搜
  clearTimeout(searchTimer)
  if (!searchInput.value.trim()) { exitSearch(); return }
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
    toastErr(`关键词最多 ${SEARCH_KW_MAX} 个字符(当前 ${kw.length} 个)`)
    return
  }
  query.searching = true
  query.keyword = kw
  clearSel()
  await load()
}
function exitSearch() {
  clearTimeout(searchTimer)
  if (!query.searching && !searchInput.value) return
  query.searching = false; query.keyword = ''; query.truncated = false; searchInput.value = ''
  clearSel()
  load()
}
function onSort(k) {
  if (sortKey.value === k) sortOrder.value = sortOrder.value === 'asc' ? 'desc' : 'asc'
  else { sortKey.value = k; sortOrder.value = 'asc' }
  load()   // 搜索态下排序对结果同样生效
}

// ── 选中(单击单选 / Ctrl 加选 / Shift 连选 / 复选框只增删自己) ──
function toggleSel(n, e, alwaysToggle) {
  const r = nextSelection(listItems.value.map(x => x.id), selected.value, n.id, {
    ctrl: !!(e && (e.ctrlKey || e.metaKey)),
    shift: !!(e && e.shiftKey),
    alwaysToggle: !!alwaysToggle,
    lastIdx
  })
  selected.value = r.selected
  lastIdx = r.lastIdx
}
function clearSel() { selected.value = new Set(); lastIdx = -1 }
/// 表头全选框(FileList 抛出):勾选=全选当前已加载条目,取消=清空
function onToggleAll(checked) { checked ? (selected.value = new Set(listItems.value.map(x => x.id))) : clearSel() }
const selTargets = computed(() => listItems.value.filter(x => selected.value.has(x.id)))
// 选中项体积:文件取 size、目录取服务端下发的子树字节(bytes),两者统一口径
const selSummary = computed(() => {
  const bytes = selTargets.value.reduce((a, x) => a + nodeBytes(x), 0)
  const dirs = selTargets.value.filter(x => Number(x.type) === 1).length
  return dirs ? `${fmtSize(bytes)} · ${dirs} 个文件夹` : fmtSize(bytes)
})
function onKeydown(e) {
  const t = e.target
  // 输入框/可编辑区里不劫持快捷键:否则 Ctrl+A 无法全选输入内容
  const typing = !!t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)
  if (e.key === 'Escape') { clearSel(); closeCtx(); closeDownMenu() }
  if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && !query.searching && listItems.value.length) {
    e.preventDefault()
    onToggleAll(true)
  }
}

// ── 打开(双击):目录往下走,文件直接下载(与空间页一致) ──
function openNode(n) {
  if (Number(n.type) === 1) { goDir(n.id); return }
  downloadOne(n)
}

// ── 下载 ──
/**
 * 下载单个文件
 *
 * 多选或目录走打包(见 startPack);这里只处理文件,服务端直接换下载直链。
 *
 * @param n 条目
 */
function downloadOne(n) {
  const id = Number(n.id) || 0
  if (!id) { toastErr('分享目标无效,无法下载'); return }
  filehubApi.shareDownload(token.value, { ids: [id] })
    .then(r => { if (r && r.url) downloadByUrl(r.url); else toastErr('下载地址获取失败,请重试') })
    .catch(e => toastErr(e.message || '下载失败'))
}

const packing = ref(false)   // 打包中(按钮文案与禁用)
const PACK_RETRY_MAX = 30    // 2s × 30 ≈ 60s 上限,超时提示失败
let packTimer = null
let packTries = 0
let packIds = []             // 本轮打包的条目(服务端按这组 id 幂等去重,重试必须复用同一批)
let packHint = ''
/** 停掉打包轮询定时器(就绪、失败、超时、离开页面都要停,否则离开后仍在后台反复请求) */
function stopPackRetry() {
  if (packTimer) { clearTimeout(packTimer); packTimer = null }
}
/**
 * 打包下载一组条目
 *
 * @param ids  待打包的条目 id(目录按整棵子树打包)
 * @param hint 失败提示里的动作名,如"下载当前目录"
 */
function startPack(ids, hint) {
  if (!ids.length) { toastErr('这里没有可下载的内容'); return }
  packIds = ids
  packHint = hint
  packTries = 0
  packing.value = true
  packAttempt()
}
async function packAttempt() {
  try {
    const r = await filehubApi.shareDownload(token.value, { ids: packIds })
    if (r && r.url) {
      packing.value = false; stopPackRetry()
      downloadByUrl(r.url)
      return
    }
    // {task_no} = 压缩中:免登录没有任务面板,2s 后幂等重试直至就绪
    if (++packTries >= PACK_RETRY_MAX) {
      packing.value = false; stopPackRetry()
      toastErr(`${packHint}打包超时,请稍后重试`)
      return
    }
    packTimer = setTimeout(packAttempt, 2000)
  } catch (e) { packing.value = false; stopPackRetry(); toastErr(e.message || '打包失败') }
}
/// 分享顶层对应的条目 id:单条目分享是那一个分享根,多条目分享是各分享根
const rootIds = computed(() => (info.nodes || []).map(n => Number(n.id)).filter(Boolean))
/// 当前目录对应的条目 id(顶层时就是各分享根)
const curDirIds = computed(() => (dirId.value ? [dirId.value] : rootIds.value))
/**
 * 顶部"下载全部"与下拉首项:打包当前目录
 *
 * 顶层取分享根、子目录取该目录 id —— 由服务端按子树递归打包,不受列表分页影响。
 * 搜索态下"当前目录"没有意义,改为打包搜出来的这批条目。
 */
function downloadAll() {
  if (query.searching) { startPack(listItems.value.map(x => x.id), '下载搜索结果'); return }
  startPack(curDirIds.value, '下载当前目录')
}
/// 下拉第二项:整个分享(等价于从分享根整棵打包一次)
function downloadWhole() { startPack(rootIds.value, '下载整个分享') }

// ── 右键 / 行尾 ⋯ 菜单 ──
// 触发源(source)决定"作用对象":row=右键行、dots=行尾 ⋯、area=列表空白区
const ctxMenu = reactive({ show: false, x: 0, y: 0, node: null, multi: false, blank: false })
/// 单选态的作用对象:row/dots 用 ctxMenu.node;area 触发时取选中集里的唯一项
const ctxTarget = computed(() => ctxMenu.node || selTargets.value[0] || null)
const ctxIsDir = computed(() => !!ctxTarget.value && Number(ctxTarget.value.type) === 1)
/**
 * 菜单定位:贴到视口底部时改为向上弹
 *
 * @param e  触发事件(取 clientX / clientY)
 * @param mh 菜单估算高度
 */
function placeMenu(e, mh) {
  const mw = 200
  ctxMenu.x = Math.min(e.clientX, window.innerWidth - mw - 8)
  ctxMenu.y = (e.clientY + mh + 8 > window.innerHeight) ? Math.max(8, e.clientY - mh) : e.clientY
}
function openCtx(e, node, source) {
  ctxMenu.blank = false
  if (source === 'area') {
    // 空白处右键:有选中项 → 操作选中集;没有 → 空白处菜单(只有"返回上一级目录")
    if (!selected.value.size) { openBlankCtx(e); return }
  } else if (source === 'dots' || !selected.value.has(node.id)) {
    // 行尾 ⋯ = 只操作该行;右键未选中行 = 把它设为唯一选中项
    selected.value = new Set([node.id]); lastIdx = -1
  }
  ctxMenu.node = source === 'area' ? null : node
  ctxMenu.multi = selected.value.size > 1
  ctxMenu.show = true
  placeMenu(e, 170)   // 最多 4 项
}
function openBlankCtx(e) {
  ctxMenu.node = null; ctxMenu.multi = false; ctxMenu.blank = true; ctxMenu.show = true
  placeMenu(e, 60)
}
function closeCtx() { ctxMenu.show = false }
/// 菜单动作:作用对象由"是否多选"决定(多选 = 选中集,单选 = 当前行)
function ctxAct(kind) {
  const targets = ctxMenu.multi ? selTargets.value : (ctxTarget.value ? [ctxTarget.value] : [])
  closeCtx()
  if (!targets.length) return
  if (kind === 'open') { goDir(targets[0].id); return }
  if (kind === 'download') { downloadOne(targets[0]); return }
  // 多选或目录:打包;单个文件不必绕一层 zip,已在上面直接下载
  if (kind === 'pack') { startPack(targets.map(x => x.id), ctxMenu.multi ? '下载所选' : '打包下载'); return }
  if (kind === 'detail') openDetail(targets)
}

// ── 详情(新增):字段表与空间页对齐,去掉"创建者"、路径改分享内位置 ──
const detail = reactive({ show: false, node: null })
const batchDetail = reactive({ show: false, count: 0, dirs: 0, bytes: 0 })
/**
 * 详情里的"位置"
 *
 * 给的是**所在目录**而不是含文件名的完整路径(与列表"位置"列同一口径):
 * 目录浏览时就是当前目录链,搜索态用服务端算好的分享内相对路径。
 */
const detailPath = computed(() => {
  if (!detail.node) return ''
  if (query.searching) return detail.node.path || '(分享根)'
  const parts = breadcrumb.value.map(b => b.name)
  return parts.length ? parts.join(' / ') : '(分享根)'
})
function openDetail(targets) {
  if (targets.length > 1) {
    batchDetail.count = targets.length
    batchDetail.dirs = targets.filter(x => Number(x.type) === 1).length
    batchDetail.bytes = targets.reduce((a, x) => a + nodeBytes(x), 0)
    batchDetail.show = true
    return
  }
  detail.node = targets[0]
  detail.show = true
}

// ── 顶部"下载全部"下拉 ──
const downMenu = reactive({ show: false, x: 0, y: 0 })
function toggleDownMenu(e) {
  downMenu.show = !downMenu.show
  if (!downMenu.show) return
  // 贴按钮右缘弹出,超出视口则回退到视口内
  const r = e.currentTarget.getBoundingClientRect()
  const w = 230
  downMenu.x = Math.max(8, Math.min(r.right - w, window.innerWidth - w - 8))
  downMenu.y = r.bottom + 6
}
function closeDownMenu() { downMenu.show = false }
/// 下拉首项的文案:搜索态下打包的是搜索结果,不是目录
const downloadAllLabel = computed(() => (query.searching ? `下载搜索结果(${items.value.length} 项)`
                                                         : `下载当前目录(${total.value} 项)`))

/**
 * 顶部角标:告诉访客这条分享的访问规则
 *
 * 用词与"创建/修改分享"里的开关同名(仅登录可见),免得同一个设置在两个页面叫两种名字。
 * 信息还没拿到、或这条分享已经不可看时不表态 —— 先亮一句"免登录访问"再翻成"仅登录可见"
 * 会说反,不如不说。
 */
const accessBadge = computed(() => {
  if (state.value === 'loading') return ''
  if (state.value === 'expired' || state.value === 'unavailable' || state.value === 'notfound') return ''
  return Number(info.login_only) === 1 ? '仅登录可见' : '免登录访问'
})

const remain = computed(() => {
  if (!info.expire_time) return '永久有效'
  return `剩 ${Math.max(0, Math.ceil((info.expire_time * 1000 - Date.now()) / 86400000))} 天`
})
/**
 * 分享内容的一句话描述
 *
 * 多条目只有数量(具体是哪些条目,进列表就看得到);单条目给"类型 + 名字",
 * 名字用「」括起来与前面的动词分开,读起来不会连成"分享了文件夹产品发布方案"。
 */
const shareWhat = computed(() => {
  if (info.multi) return ` ${info.node_count} 项内容`
  return `${Number(info.node_type) === 1 ? '文件夹' : '文件'}「${info.name}」`
})

function toastErr(msg) { toastMsg.value = msg; setTimeout(() => { toastMsg.value = '' }, 3000) }
const toastMsg = ref('')

onMounted(() => {
  loadInfo()
  document.addEventListener('keydown', onKeydown)
})
onUnmounted(() => {
  stopPackRetry()
  clearTimeout(searchTimer)   // 卸载后防抖回调不该再发请求
  document.removeEventListener('keydown', onKeydown)
})
// 选中 ≥2 项时工具条整条换成批量条,下拉随之消失,先关掉免得浮层留在屏幕上
watch(() => selected.value.size, n => { if (n >= 2) closeDownMenu() })
watch(token, loadInfo)
</script>

<template>
  <div class="share-page">
    <header class="share-top">
      <div class="portal-brand"><span class="portal-logo">Z</span>ZiMo 文件中心</div>
      <span style="flex:1"></span>
      <span v-if="accessBadge" class="badge badge-dim">{{ accessBadge }}</span>
      <!-- 主题切换:门户壳里由它统一落 data-theme,分享页不在壳内得自己带一个 ——
           缺了它,系统为暗色时页面令牌仍是亮色、而原生控件(复选框)已按暗色渲染,两边对不上 -->
      <ThemeSwitch />
    </header>

    <main class="share-mid" :class="{ 'share-mid-list': state === 'browse' }">
      <!-- 加载中 -->
      <div v-if="state === 'loading'" class="card share-card" style="padding:32px">
        <div v-for="i in 3" :key="i" class="skeleton" style="height:22px;margin-bottom:14px"></div>
      </div>

      <!-- 提取码 -->
      <div v-else-if="state === 'pwd'" class="card share-card" style="padding:36px;text-align:center">
        <div class="share-owner" style="justify-content:center">
          <span class="avatar">{{ (info.owner_name || '?').charAt(0) }}</span>
          <div style="text-align:left">
            <b>{{ info.owner_name }}</b> 分享了{{ shareWhat }}
            <div class="cap" style="color:var(--color-text-3)">来自 ZiMo 文件中心</div>
          </div>
        </div>
        <!-- 标题就是展示名本身:绑了几项上面那行已经写了("分享了 N 项"),
             往名字后面缀"等 N 项"会把分享者填的自定义名改样 -->
        <b style="font-size:19px;display:block;margin-top:20px">{{ info.name }}</b>
        <div class="form-item" style="max-width:280px;margin:24px auto 0">
          <input v-model.trim="pwd" class="input" style="height:46px;text-align:center;letter-spacing:4px;font-weight:700"
                 maxlength="8" placeholder="输入提取码" @keyup.enter="verify" />
          <span v-if="pwdErr" class="field-error" style="justify-content:center">{{ pwdErr }}</span>
          <button class="btn btn-grad btn-lg btn-block" type="button" :disabled="pwdBusy || !pwd.trim()" @click="verify">提取文件</button>
        </div>
      </div>

      <!-- 需登录 -->
      <div v-else-if="state === 'need_login'" class="card share-card" style="padding:36px;text-align:center">
        <div style="width:64px;height:64px;border-radius:20px;background:var(--color-primary-soft);color:var(--color-primary);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
        </div>
        <b style="font-size:var(--fs-h2)">此分享仅登录用户可见</b>
        <p class="cap" style="color:var(--color-text-2);margin-top:8px">登录后将回到本页面</p>
        <button class="btn btn-grad btn-lg" type="button" style="margin-top:16px;min-width:160px" @click="goLogin">去登录</button>
      </div>

      <!-- 失效 / 不可用 / 不存在 -->
      <div v-else-if="state !== 'browse'" class="card share-card" style="padding:36px;text-align:center">
        <div v-if="state === 'expired'" style="width:64px;height:64px;border-radius:20px;background:var(--color-err-bg);color:var(--color-err);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="m9 9 6 6M15 9l-6 6"/></svg>
        </div>
        <div v-else-if="state === 'unavailable'" style="width:64px;height:64px;border-radius:20px;background:var(--color-warn-bg);color:var(--color-warn);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 10h6"/></svg>
        </div>
        <div v-else style="width:64px;height:64px;border-radius:20px;background:var(--color-border-soft);color:var(--color-text-3);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5M8 11h6"/></svg>
        </div>
        <b style="font-size:var(--fs-h2)">
          {{ state === 'expired' ? '分享已失效' : state === 'unavailable' ? '分享内容暂不可用' : '分享不存在或链接有误' }}
        </b>
        <p class="cap" style="color:var(--color-text-2);margin-top:8px">
          {{ state === 'expired' ? '链接已过期、被取消或下载次数已达上限' : state === 'unavailable' ? '该文件正在回收站中;恢复后本链接将自动恢复可用' : '请向分享者确认链接是否正确' }}
        </p>
      </div>

      <!-- 浏览 -->
      <div v-else class="sheet" :class="{ 'sheet-fill': isDirShare }">
        <!-- 分享信息卡:只留身份与有效期,下载动作在工具条上 -->
        <div class="card" style="padding:16px 20px">
          <div class="share-owner">
            <span class="avatar">{{ (info.owner_name || '?').charAt(0) }}</span>
            <div style="flex:1;min-width:0">
              <div><b>{{ info.owner_name }}</b> 分享了{{ shareWhat }}</div>
              <div class="cap" style="color:var(--color-text-3)">
                {{ remain }}<template v-if="info.max_downloads"> · 已下载 {{ info.download_count }}/{{ info.max_downloads }} 次</template>
                <template v-if="info.hidden_count > 0"> · 另有 {{ info.hidden_count }} 项暂不可用</template>
              </div>
            </div>
          </div>
        </div>

        <!-- 单文件分享:没有可搜可打包的内容,只留一个显眼的下载入口
             (行尾 ⋯ 要悬停才出现,唯一动作藏进去等于没有入口) -->
        <div v-if="!isDirShare" class="card fh-toolbar">
          <span style="flex:1"></span>
          <button class="btn btn-grad" type="button" :disabled="packing || !listItems.length"
                  @click="listItems.length && downloadOne(listItems[0])">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 4v12m0 0 4-4m-4 4-4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>
            下载
          </button>
        </div>

        <!-- 工具条 / 批量条(≥2 项选中时整条替换) -->
        <div v-else-if="selected.size < 2" class="card fh-toolbar">
          <div class="input-wrap fh-search grow">
            <input v-model.trim="searchInput" class="input" style="height:38px" :maxlength="SEARCH_KW_MAX"
                   placeholder="搜索当前目录及子目录…" aria-label="搜索分享内容"
                   @compositionstart="searchComposing = true" @compositionend="searchComposing = false; scheduleSearch()"
                   @input="scheduleSearch" @keyup.enter="flushSearch" />
            <span v-if="searchInput" class="input-suffix">
              <button class="icon-btn" type="button" aria-label="清空搜索" title="清空" @click="clearSearch">
                <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
              </button>
            </span>
          </div>
          <button class="btn btn-grad" type="button" :disabled="packing || !listItems.length"
                  aria-haspopup="menu" :aria-expanded="downMenu.show" @click="toggleDownMenu">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 4v12m0 0 4-4m-4 4-4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>
            {{ packing ? '打包中…' : '下载全部' }}
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" style="margin-left:3px"><path d="m6 9 6 6 6-6"/></svg>
          </button>
          <div class="vseg">
            <button type="button" :class="{ on: layout === 'list' }" title="列表视图" aria-label="列表视图" @click="layout = 'list'">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>
            </button>
            <button type="button" :class="{ on: layout === 'grid' }" title="网格视图" aria-label="网格视图" @click="layout = 'grid'">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></svg>
            </button>
          </div>
          <button class="icon-btn" type="button" title="刷新" aria-label="刷新" @click="refresh">
            <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 3v6h-6"/></svg>
          </button>
        </div>
        <!-- 批量条:只做汇总与取消,操作统一走右键菜单(与空间页一致) -->
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
          <span class="badge badge-warn" v-if="query.truncated">结果过多,已截断,请缩小范围</span>
          <span class="cap" style="color:var(--color-text-2)">搜索"{{ searchInput }}" · 当前目录及全部子目录 · 共 <b class="num">{{ total }}</b> 条</span>
          <span style="flex:1"></span>
          <button class="btn btn-ghost btn-sm" type="button" @click="exitSearch">退出搜索</button>
        </div>

        <!-- 列表:与空间文件区同一份组件 -->
        <div class="card fh-listcard" style="padding:0;overflow:hidden;position:relative">
          <div v-if="!query.searching && breadcrumb.length" class="crumbs">
            <template v-for="(b, i) in breadcrumb" :key="b.id">
              <svg v-if="i" class="sep" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="m9 6 6 6-6 6"/></svg>
              <button type="button" :class="{ here: i === breadcrumb.length - 1 }" @click="goDir(b.id)">
                <svg v-if="!i" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
                {{ b.name }}
              </button>
            </template>
            <span style="flex:1"></span>
            <span class="cap num" style="color:var(--color-text-3)">共 {{ total }} 项</span>
          </div>
          <FileList :items="listItems" :selected="selected" :keyword="query.searching ? query.keyword : ''"
                    :show-path="query.searching" :sort="sortKey" :order="sortOrder" :view="layout"
                    :loading="loadingList" :has-more="hasMore" :epoch="listEpoch" variant="share"
                    @toggle="toggleSel" @open="openNode" @ctx="openCtx" @sort="onSort"
                    @load-more="loadMore" @toggle-all="onToggleAll" />
        </div>
      </div>
    </main>

    <footer class="share-foot">由 ZiMo 文件中心提供 · 仅可浏览与下载</footer>

    <!-- 悬浮层:传送到 body,免被列表卡片裁剪 -->
    <Teleport to="body">
      <!-- 下载全部下拉:全屏透明层负责"点外面关闭" -->
      <div v-if="downMenu.show" style="position:fixed;inset:0;z-index:880" @click="closeDownMenu" @contextmenu.prevent="closeDownMenu"></div>
      <div v-if="downMenu.show" class="menu" style="position:fixed;z-index:881;min-width:230px"
           :style="{ left: downMenu.x + 'px', top: downMenu.y + 'px' }" role="menu">
        <button class="menu-item" type="button" role="menuitem" :disabled="packing" @click="closeDownMenu(); downloadAll()">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>{{ downloadAllLabel }}</button>
        <button class="menu-item" type="button" role="menuitem" :disabled="packing || !rootIds.length" @click="closeDownMenu(); downloadWhole()">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/></svg>下载整个分享</button>
      </div>

      <div v-if="ctxMenu.show" style="position:fixed;inset:0;z-index:890" @click="closeCtx" @contextmenu.prevent="closeCtx"></div>
      <!-- 空白处菜单(无选中项时右键列表空白区):作用于"当前位置" -->
      <div v-if="ctxMenu.show && ctxMenu.blank" class="menu" style="position:fixed;z-index:891;min-width:200px" :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
        <button class="menu-item" type="button" :disabled="!dirId" @click="goParent">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 19V5m0 0-6 6m6-6 6 6"/></svg>返回上一级目录</button>
      </div>
      <div v-if="ctxMenu.show && !ctxMenu.blank" class="menu" style="position:fixed;z-index:891;min-width:200px" :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
        <!-- 首项随作用对象变字:单选目录=打开、单选文件=下载、多选=下载所选 -->
        <button v-if="!ctxMenu.multi && ctxIsDir" class="menu-item" type="button" @click="ctxAct('open')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>打开</button>
        <button v-if="!ctxMenu.multi && !ctxIsDir" class="menu-item" type="button" @click="ctxAct('download')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 4v12m0 0 4-4m-4 4-4-4"/><path d="M4 17v1a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-1"/></svg>下载</button>
        <!-- 打包下载只给"多选"和"单选文件夹":单文件直接下载更直接,不必绕一层 zip -->
        <button v-if="ctxMenu.multi || ctxIsDir" class="menu-item" type="button" :disabled="packing" @click="ctxAct('pack')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/></svg>{{ ctxMenu.multi ? `下载所选(${selected.size})` : '打包下载' }}</button>
        <div class="menu-sep"></div>
        <button class="menu-item" type="button" @click="ctxAct('detail')">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>详情</button>
      </div>
    </Teleport>

    <!-- 详情:字段与空间页对齐,去掉"创建者"(免登录页不该暴露文件是谁上传的) -->
    <Modal :show="detail.show" title="条目详情" @close="detail.show = false">
      <table v-if="detail.node" class="table" style="font-size:var(--fs-cap)">
        <tbody>
          <tr><td style="color:var(--color-text-3);width:80px">名称</td><td><b>{{ detail.node.name }}</b></td></tr>
          <tr><td style="color:var(--color-text-3)">类型</td><td>{{ kindLabel(detail.node) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">大小</td><td class="num">{{ fmtNodeSize(detail.node) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">位置</td><td>{{ detailPath }}</td></tr>
          <tr><td style="color:var(--color-text-3)">创建时间</td><td class="num">{{ fmtTime(detail.node.create_time) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">修改时间</td><td class="num">{{ fmtTime(detail.node.update_time) }}</td></tr>
        </tbody>
      </table>
      <template #foot>
        <button class="btn btn-primary" type="button" @click="detail.show = false">关闭</button>
      </template>
    </Modal>

    <!-- 批量详情:逐条字段没有公共值,只汇总条目数与总体积(与空间页同款) -->
    <Modal :show="batchDetail.show" title="批量详情" @close="batchDetail.show = false">
      <table class="table" style="font-size:var(--fs-cap)">
        <tbody>
          <tr><td style="color:var(--color-text-3);width:80px">条目数</td>
              <td><b class="num">{{ batchDetail.count }}</b> 项<span v-if="batchDetail.dirs">(含 {{ batchDetail.dirs }} 个文件夹)</span></td></tr>
          <tr><td style="color:var(--color-text-3)">总大小</td><td class="num">{{ fmtSize(batchDetail.bytes) }}</td></tr>
          <tr><td style="color:var(--color-text-3)">名称</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">类型</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">位置</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">创建时间</td><td>—</td></tr>
          <tr><td style="color:var(--color-text-3)">修改时间</td><td>—</td></tr>
        </tbody>
      </table>
      <p class="form-hint" style="margin-top:10px">多选时无法逐条展示名称、时间等字段,这里只汇总条目数与总体积(文件夹按子树字节计入)。</p>
      <template #foot>
        <button class="btn btn-primary" type="button" @click="batchDetail.show = false">关闭</button>
      </template>
    </Modal>

    <Teleport to="body">
      <Transition name="toast">
        <div v-if="toastMsg" class="toast toast-err"><span aria-hidden="true">✕</span><span>{{ toastMsg }}</span></div>
      </Transition>
    </Teleport>
  </div>
</template>

<style scoped>
.share-page{min-height:100vh;display:flex;flex-direction:column;
  background:var(--grad-brand-soft)}
[data-theme="dark"] .share-page{background:radial-gradient(1200px 500px at 70% -10%,rgba(56,189,248,.14),transparent 60%),radial-gradient(900px 420px at 10% 110%,rgba(236,72,153,.12),transparent 55%),var(--color-bg)}
.share-top{display:flex;align-items:center;gap:10px;padding:16px 24px}
.portal-brand{display:flex;align-items:center;gap:10px;font-weight:800;font-size:16px}
.portal-logo{width:30px;height:30px;border-radius:10px;background:var(--grad-brand);color:#fff;display:flex;align-items:center;justify-content:center;font-weight:700;box-shadow:var(--shadow-pop)}
.share-mid{flex:1;min-height:0;display:flex;align-items:center;justify-content:center;padding:24px 16px 40px}
/* 浏览态:内容撑满剩余高度,列表在卡片内滚动(与空间文件区同一套布局思路) */
.share-mid-list{align-items:stretch}
.share-card{width:640px;max-width:100%}
/* 列表卡片比居中卡片宽:名称列要截断长名,搜索结果还多一列"位置" */
.sheet{width:1080px;max-width:100%;min-height:0;display:flex;flex-direction:column;gap:12px;align-self:flex-start}
/* 目录分享才撑满高度:列表要吃掉剩余高度并在卡片内滚;单文件分享按内容高度即可 */
.sheet.sheet-fill{align-self:stretch}
.share-owner{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.share-foot{text-align:center;padding:16px;color:var(--color-text-3);font-size:var(--fs-cap)}
</style>
