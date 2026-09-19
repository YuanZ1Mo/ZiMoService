<script setup>
// 回收站视图(需求 §3.7.2/§7.4):只读 + 恢复/彻底删除/清空;仅显示"我删除的"(del_owner_uid=我)
//
// 与空间文件区共用同一套交互(工具条/批量条/FileList/右键菜单/选中语义),
// 三处刻意的差异 ——
//   1) 文件夹不下钻、双击不触发任何动作(回收站是平的,条目也不能打开或下载)
//   2) 不接拖拽:内部不能移动,也不能从系统拖入上传
//   3) 空白处右键不弹菜单:空间那边弹的是"返回上一级目录",回收站没有层级
import { ref, reactive, computed, onMounted, onBeforeUnmount, onActivated, onDeactivated, watch, inject } from 'vue'
import Modal from '../../components/Modal.vue'
import FileList from './FileList.vue'
import { filehubApi, fmtSize, fmtTime, fmtNodeSize, nodeBytes, nextSelection } from '../../api/filehub'
import { useFilehubStore } from '../../stores/filehub'

const props = defineProps({
  space: { type: Number, required: true },
  meSpace: { type: Number, default: 0 },
  spaces: { type: Array, default: () => [] }   ///< 空间列表(父组件传,与空间文件区同一份)
})
const emit = defineEmits(['changed', 'space'])
const toast = inject('toast')
const store = useFilehubStore()

const SEARCH_KW_MAX = 64        // 服务端关键词上限(超出会静默返回 0 条,故前端先拦并说明)
const SEARCH_DEBOUNCE_MS = 400  // 键入停止多久后自动搜索
const PAGE_SIZE = 200           // 回收站接口单页上限

const rows = ref([])
const total = ref(0)
const used = reactive({ size: 0, items: 0 })
const retainDays = ref(30)
const loading = ref(false)
const layout = ref('list')
const sortKey = ref('delete_time')
const sortOrder = ref('desc')
const page = ref(1)
const epoch = ref(0)       // 整表替换标记:FileList 据此复位滚动(否则刷新后停在空白处)
const kwInput = ref('')
const query = reactive({ keyword: '', searching: false })
const selected = ref(new Set())
let lastIdx = -1
let searchTimer = 0
let loadSeq = 0   // 请求序号:只认最后一次发出的请求,防止慢响应把新结果盖回旧的

const confirmBox = reactive({ show: false, title: '', html: '', fn: null })
const detail = reactive({ show: false, node: null })
const batchDetail = reactive({ show: false, count: 0, dirs: 0, bytes: 0 })
const ctxMenu = reactive({ show: false, x: 0, y: 0, node: null, multi: false })

/**
 * 取回收站列表
 *
 * @param append true = 追加下一页(无限滚动);false = 从第 1 页整表替换
 */
async function load(append = false) {
  const seq = ++loadSeq
  loading.value = true
  try {
    if (!append) page.value = 1
    const d = await filehubApi.trash({
      space: props.space, page: page.value, size: PAGE_SIZE,
      sort: sortKey.value, order: sortOrder.value,
      keyword: query.searching ? query.keyword : ''
    })
    if (seq !== loadSeq) return   // 已发出更新的请求,丢弃这次的响应
    const list = d.list || []
    rows.value = append ? [...rows.value, ...list] : list
    total.value = d.total || 0
    used.size = d.used_size || 0
    used.items = d.used_items || 0
    retainDays.value = d.retain_days || 30
    if (!append) { selected.value = new Set(); lastIdx = -1; epoch.value++ }
    // 恢复/彻底删除后页码可能越过末页,回退到末页重取,否则停在空白页
    const pages = Math.max(1, Math.ceil(total.value / PAGE_SIZE))
    if (!append && page.value > pages) { page.value = pages; return load() }
  } catch (e) {
    if (seq === loadSeq) toast(e.message || '加载回收站失败', 'err')
  } finally {
    if (seq === loadSeq) loading.value = false
  }
}
onMounted(load)
// 切空间:回收站内容与占用都换了,退出搜索并按新空间从头取
watch(() => props.space, () => {
  clearTimeout(searchTimer)
  query.searching = false; query.keyword = ''; kwInput.value = ''
  load()
})

function loadMore() {
  if (rows.value.length < total.value && !loading.value) { page.value++; load(true) }
}
function reload() { emit('changed'); load() }

// ── 搜索(与空间同口径:去首尾空格、超长先拦、键入停止即搜) ──
/**
 * 键入停止后自动搜索(防抖)
 *
 * 输入框不配"搜索"按钮,靠这个间隔把连续敲键合并成一次请求。
 * 清空关键词即时退出搜索、不等防抖 —— 否则列表会先空一下再回来。
 */
function scheduleSearch() {
  clearTimeout(searchTimer)
  if (!kwInput.value.trim()) { doSearch(); return }
  searchTimer = setTimeout(doSearch, SEARCH_DEBOUNCE_MS)
}
/// 立即搜索(回车):取消挂起的防抖,不必再等那 400ms
function flushSearch() {
  clearTimeout(searchTimer)
  doSearch()
}
/// 一键清空并退出搜索
function clearSearch() {
  kwInput.value = ''
  exitSearch()
}
function doSearch() {
  clearTimeout(searchTimer)
  const kw = kwInput.value.trim()
  if (!kw) { exitSearch(); return }
  if (kw.length > SEARCH_KW_MAX) {
    toast(`关键词最多 ${SEARCH_KW_MAX} 个字符(当前 ${kw.length} 个)`, 'warn')
    return
  }
  query.searching = true
  query.keyword = kw
  load()
}
function exitSearch() {
  clearTimeout(searchTimer)
  query.searching = false; query.keyword = ''; kwInput.value = ''
  load()
}

// ── 排序(四档都由服务端支持;剩余/原位置不参与排序) ──
function onSort(k) {
  if (sortKey.value === k) sortOrder.value = sortOrder.value === 'asc' ? 'desc' : 'asc'
  else { sortKey.value = k; sortOrder.value = 'asc' }
  load()
}

// ── 选中:与空间同一套语义(复选框纯增删 / Ctrl 增删 / Shift 连选 / 单击单选) ──
function toggleSel(n, e, alwaysToggle) {
  const r = nextSelection(rows.value.map(x => x.id), selected.value, n.id, {
    ctrl: !!(e && (e.ctrlKey || e.metaKey)),
    shift: !!(e && e.shiftKey),
    alwaysToggle: !!alwaysToggle,
    lastIdx
  })
  selected.value = r.selected
  lastIdx = r.lastIdx
}
function selectAll() { selected.value = new Set(rows.value.map(x => x.id)) }
function clearSel() { selected.value = new Set(); lastIdx = -1 }
function onToggleAll(checked) { checked ? selectAll() : clearSel() }

const selTargets = computed(() => rows.value.filter(x => selected.value.has(x.id)))
const selBytes = computed(() => selTargets.value.reduce((a, x) => a + nodeBytes(x), 0))
const selDirCount = computed(() => selTargets.value.filter(x => Number(x.type) === 1).length)
// 批量条上的构成说明:总体积 + 文件夹个数(目录在库里 size 恒为 0,只累加会显示假体积)
const selSummary = computed(() => {
  const parts = [fmtSize(selBytes.value)]
  if (selDirCount.value) parts.push(`${selDirCount.value} 个文件夹`)
  return parts.join(' · ')
})

// ── 右键菜单(结构同空间:首项主操作、危险项单独分隔、末项详情) ──
/**
 * 打开右键菜单
 *
 * @param e      触发事件
 * @param node   命中的行(行尾 ⋯ 与行右键都传,空白处不传)
 * @param source row = 右键某行;dots = 行尾 ⋯
 */
function openCtx(e, node, source) {
  if (source === 'area') {
    // 空白处右键:有选中项 → 照样弹菜单、作用于选中集(与空间一致);
    // 没选中 → 空间那边弹的是"返回上一级目录",回收站没有层级,不弹
    if (!selected.value.size) return
  } else if (source === 'dots' || !selected.value.has(node.id)) {
    // 行尾 ⋯ = 只操作该行;右键未选中行 = 把它设为唯一选中项
    // (右键总能出菜单,否则用户会以为右键失灵)
    selected.value = new Set([node.id]); lastIdx = -1
  }
  ctxMenu.node = source === 'area' ? null : node
  ctxMenu.multi = selected.value.size > 1
  ctxMenu.show = true
  // 菜单定位:贴到视口底部时改为向上弹,否则底部会被窗口裁掉
  const mh = 140
  ctxMenu.x = Math.min(e.clientX, window.innerWidth - 208)
  ctxMenu.y = (e.clientY + mh + 8 > window.innerHeight) ? Math.max(8, e.clientY - mh) : e.clientY
}
function closeCtx() { ctxMenu.show = false }
/// 菜单当前作用的对象集:多选 = 整个选中集;单选 = 命中的那一行,
/// 空白处右键时不带行(菜单作用于选中集),此时回落到选中集里的唯一项
const ctxTargets = computed(() => {
  if (ctxMenu.multi) return selTargets.value
  const one = ctxMenu.node || selTargets.value[0] || null
  return one ? [one] : []
})
function ctxAct(act) {
  const targets = ctxTargets.value
  closeCtx()
  if (act === 'restore') restore(targets)
  else if (act === 'purge') purge(targets)
  else if (act === 'detail') targets.length > 1 ? openBatchDetail(targets) : openDetail(targets[0])
}

function onKeydown(e) {
  const t = e.target
  // 输入框里不劫持快捷键:否则 Ctrl+A 无法全选输入内容
  const typing = !!t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)
  if (e.key === 'Escape') { clearSel(); closeCtx() }
  // 搜索态不给 Ctrl+A:选中的是搜索结果而不是"回收站里的全部",与空间同口径
  if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && !query.searching) {
    e.preventDefault(); selectAll()
  }
}
onMounted(() => document.addEventListener('keydown', onKeydown))
// keep-alive 下父组件失活不会卸载子组件:监听必须随激活状态成对挂摘,
// 否则停在回收站时切去别的页面,那边按 Ctrl+A 会选到看不见的回收站条目
// (同一个函数重复 addEventListener 无副作用,两条路径可共存)
onActivated(() => document.addEventListener('keydown', onKeydown))
onDeactivated(() => document.removeEventListener('keydown', onKeydown))
onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeydown)
  clearTimeout(searchTimer)   // 卸载后防抖回调不该再发请求
})

// ── 动作 ──
function openDetail(n) {
  if (!n) return
  detail.node = n
  detail.show = true
}
/// 多选详情:名称/原位置/删除时间这些都是逐条属性,对整批无意义 —— 只汇总条目数与总体积
/// (与空间文件区的"批量详情"同一口径;文件夹按子树字节计入)
function openBatchDetail(targets) {
  batchDetail.count = targets.length
  batchDetail.dirs = targets.filter(t => Number(t.type) === 1).length
  batchDetail.bytes = targets.reduce((a, t) => a + nodeBytes(t), 0)
  batchDetail.show = true
}
function askConfirm(title, html, fn) {
  confirmBox.title = title; confirmBox.html = html; confirmBox.fn = fn; confirmBox.show = true
}
function restore(items) {
  if (!items.length) return
  askConfirm('恢复所选条目?', `将恢复 <b>${items.length}</b> 项到原位置;原位置被占用或原目录已删除时,恢复到空间根并自动重命名。`, async () => {
    try {
      const r = await filehubApi.trashRestore(items.map(x => x.id))
      const okN = (r.restored || []).length
      const failN = (r.failed || []).length
      // 是否被自动重命名:按服务端返回的最终名与原名比对
      // (原判定用 / \(\d+\)$/ 要求序号在末尾,而实际是插在扩展名之前,对带扩展名的文件永不成立)
      const origin = new Map(items.map(x => [x.id, x.name]))
      const renamed = (r.restored || []).some(x => x.name && x.name !== origin.get(x.id))
      // 部分失败要说清数量,否则用户以为全都恢复了
      toast(`已恢复 ${okN} 项${renamed ? '(部分自动重命名)' : ''}` +
            (failN ? `,${failN} 项失败(可能已被彻底删除)` : ''), failN ? 'warn' : 'ok')
      reload()
    } catch (e) { toast(e.message || '恢复失败', 'err') }
  })
}
function purge(items) {
  if (!items.length) return
  const bytes = items.reduce((a, x) => a + nodeBytes(x), 0)
  askConfirm(`彻底删除 ${items.length} 项?`,
    `将连同全部子项<b style="color:var(--color-err)">物理删除</b>,共 ${fmtSize(bytes)},<b style="color:var(--color-err)">不可撤销</b>。`,
    async () => {
      try {
        const r = await filehubApi.trashPurge(items.map(x => x.id))
        const okN = (r.success || []).length
        const failN = (r.failed || []).length
        toast(`已彻底删除 ${okN} 项` + (failN ? `,${failN} 项失败` : ''), failN ? 'warn' : 'ok')
        reload()
      } catch (e) { toast(e.message || '删除失败', 'err') }
    })
}
function clearAll() {
  askConfirm('清空回收站?',
    `将<b style="color:var(--color-err)">永久删除</b>此空间中你删除的全部 <b>${used.items}</b> 个条目,共 <b>${fmtSize(used.size)}</b>,不可撤销;操作在后台执行,进度见任务面板。`,
    async () => {
      try {
        await filehubApi.trashClear(props.space)
        toast('已创建清空任务,进度见任务面板', 'ok')
        store.togglePanel(true)   // 打开任务面板并起轮询(清空是后台任务,列表此时还没变)
        reload()
      } catch (e) { toast(e.message || '操作失败', 'err') }
    })
}
function remainDays(n) {
  return Math.max(0, retainDays.value - Math.floor((Date.now() / 1000 - (n.delete_time || 0)) / 86400))
}
</script>

<template>
  <!-- 工具条 / 批量条:与空间文件区同款(.fh-toolbar),选中 ≥2 项时批量条替换工具条 -->
  <div v-if="selected.size < 2" class="card fh-toolbar">
    <div class="seg" style="width:196px">
      <button v-for="s in spaces" :key="s.space" type="button" class="seg-item"
              :class="{ active: Number(space) === Number(s.space) }"
              @click="emit('space', Number(s.space))">{{ s.name }}</button>
    </div>
    <div class="input-wrap fh-search grow">
      <input v-model.trim="kwInput" class="input" style="height:38px" :maxlength="SEARCH_KW_MAX"
             placeholder="在回收站内搜索…" @input="scheduleSearch" @keyup.enter="flushSearch" />
      <!-- 键入停止即自动搜索,故不需要"搜索"按钮;留个清空按钮,与空间文件区一致 -->
      <span v-if="kwInput" class="input-suffix">
        <button class="icon-btn" type="button" aria-label="清空搜索" title="清空" @click="clearSearch">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
        </button>
      </span>
    </div>
    <div class="vseg">
      <button type="button" :class="{ on: layout === 'list' }" title="列表视图" aria-label="列表视图" @click="layout = 'list'">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>
      </button>
      <button type="button" :class="{ on: layout === 'grid' }" title="网格视图" aria-label="网格视图" @click="layout = 'grid'">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="4" width="7" height="7" rx="1.5"/><rect x="13" y="4" width="7" height="7" rx="1.5"/><rect x="4" y="13" width="7" height="7" rx="1.5"/><rect x="13" y="13" width="7" height="7" rx="1.5"/></svg>
      </button>
    </div>
    <button class="icon-btn" type="button" title="刷新" aria-label="刷新" @click="load()">
      <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 3v6h-6"/></svg>
    </button>
    <button class="btn btn-danger" type="button" :disabled="!used.items" @click="clearAll">清空回收站</button>
  </div>
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
    <span class="cap" style="color:var(--color-text-2)">
      搜索"{{ query.keyword }}" · 回收站内全部条目 · 共 <b class="num">{{ total }}</b> 条
    </span>
    <span style="flex:1"></span>
    <button class="btn btn-ghost btn-sm" type="button" @click="exitSearch">退出搜索</button>
  </div>

  <div class="card fh-listcard">
    <!-- 列表头:空间那边是面包屑 + "共 N 项",回收站没有层级,换成保留期与占用说明 -->
    <div class="crumbs">
      <span>
        共 <b class="num">{{ total }}</b> 项 · 删除 <b class="num">{{ retainDays }}</b> 天后自动清除 · 占用 <b class="num">{{ fmtSize(used.size) }}</b>
      </span>
    </div>
    <FileList variant="trash" :items="rows" :selected="selected" :view="layout"
              :keyword="query.searching ? query.keyword : ''"
              :sort="sortKey" :order="sortOrder" :loading="loading"
              :has-more="rows.length < total" :epoch="epoch"
              :space="space" :retain-days="retainDays"
              @toggle="toggleSel" @ctx="openCtx" @sort="onSort" @toggle-all="onToggleAll"
              @load-more="loadMore" />
  </div>

  <Teleport to="body">
    <div v-if="ctxMenu.show" style="position:fixed;inset:0;z-index:890" @click="closeCtx" @contextmenu.prevent="closeCtx"></div>
    <div v-if="ctxMenu.show" class="menu" style="position:fixed;z-index:891;min-width:200px"
         :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }">
      <!-- 首项 = 主操作(对齐空间:那边单选文件是"下载",这里回收站里能做的主操作就是"恢复") -->
      <button class="menu-item" type="button" @click="ctxAct('restore')">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/></svg>{{ ctxMenu.multi ? `恢复 ${ctxTargets.length} 项` : '恢复' }}</button>
      <button class="menu-item danger" type="button" @click="ctxAct('purge')">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M8 6V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2m3 0v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>{{ ctxMenu.multi ? `彻底删除 ${ctxTargets.length} 项` : '彻底删除' }}</button>
      <div class="menu-sep"></div>
      <button class="menu-item" type="button" @click="ctxAct('detail')">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/></svg>详情</button>
    </div>
  </Teleport>

  <Modal :show="confirmBox.show" :title="confirmBox.title" @close="confirmBox.show = false">
    <p class="dlg-tip" style="margin:0" v-html="confirmBox.html"></p>
    <template #foot>
      <button class="btn btn-ghost" type="button" @click="confirmBox.show = false">取消</button>
      <button class="btn btn-danger" type="button" @click="confirmBox.show = false; confirmBox.fn && confirmBox.fn()">确认执行</button>
    </template>
  </Modal>

  <Modal :show="batchDetail.show" title="批量详情" @close="batchDetail.show = false">
    <table class="table" style="font-size:var(--fs-cap)">
      <tbody>
        <tr><td style="color:var(--color-text-3);width:80px">条目数</td>
            <td><b class="num">{{ batchDetail.count }}</b> 项<span v-if="batchDetail.dirs">(含 {{ batchDetail.dirs }} 个文件夹)</span></td></tr>
        <tr><td style="color:var(--color-text-3)">总大小</td><td class="num">{{ fmtSize(batchDetail.bytes) }}</td></tr>
        <tr><td style="color:var(--color-text-3)">名称</td><td>—</td></tr>
        <tr><td style="color:var(--color-text-3)">类型</td><td>—</td></tr>
        <tr><td style="color:var(--color-text-3)">原位置</td><td>—</td></tr>
        <tr><td style="color:var(--color-text-3)">删除时间</td><td>—</td></tr>
        <tr><td style="color:var(--color-text-3)">剩余</td><td>—</td></tr>
      </tbody>
    </table>
    <p class="form-hint" style="margin-top:10px">多选时无法逐条展示名称、原位置、时间等字段,这里只汇总条目数与总体积(文件夹按子树字节计入)。</p>
    <template #foot>
      <button class="btn btn-primary" type="button" @click="batchDetail.show = false">关闭</button>
    </template>
  </Modal>

  <Modal :show="detail.show" title="条目详情" @close="detail.show = false">
    <div v-if="detail.node" class="dlg-tip" style="margin:0">
      <div class="kv"><span class="k">名称</span><span class="v">{{ detail.node.name }}</span></div>
      <div class="kv"><span class="k">类型</span><span class="v">{{ Number(detail.node.type) === 1 ? '文件夹' : '文件' }}</span></div>
      <div class="kv"><span class="k">大小</span><span class="v num">{{ fmtNodeSize(detail.node) }}</span></div>
      <div class="kv"><span class="k">原位置</span>
        <span class="v" :style="!detail.node.path && Number(detail.node.origin_parent_id) ? 'color:var(--color-warn)' : ''">
          {{ detail.node.path || (Number(detail.node.origin_parent_id) ? '原目录已删除(恢复到空间根)' : '空间根') }}
        </span></div>
      <div class="kv"><span class="k">删除时间</span><span class="v num">{{ fmtTime(detail.node.delete_time) }}</span></div>
      <div class="kv"><span class="k">剩余</span><span class="v num">剩 {{ remainDays(detail.node) }} 天</span></div>
    </div>
    <!-- 与空间文件区的条目详情一致:纯展示,只给关闭 —— 恢复走列表/右键,
         详情里再放一个动作按钮会让两个模块的同名弹窗长得不一样 -->
    <template #foot>
      <button class="btn btn-primary" type="button" @click="detail.show = false">关闭</button>
    </template>
  </Modal>
</template>

<style scoped>
.kv{display:flex;gap:12px;padding:5px 0;font-size:var(--fs-body)}
.kv .k{width:72px;flex:none;color:var(--color-text-3)}
.kv .v{flex:1;min-width:0;word-break:break-all}
</style>
