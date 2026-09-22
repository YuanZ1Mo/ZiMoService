<script setup>
// 工具二:Markdown 编辑器(设计 §5)
// 管线:源文 HTML 转义 → marked(gfm,breaks:false) → DOMPurify 清理 → v-html
// 草稿:localStorage 单键(多标签页共享、后写覆盖);写入异常必须提示,不静默丢弃
// 滚动联动单向(预览 → 编辑),仅分栏生效
import { ref, computed, reactive, watch, nextTick, onMounted, onBeforeUnmount, inject } from 'vue'
import { marked } from 'marked'
import DOMPurify from 'dompurify'
import Modal from '../../components/Modal.vue'
import { countStats } from './stats'

const toast = inject('toast')

const DRAFT_KEY = 'zimo-dev-tools:markdown-draft'
// 阈值口径按**字符数**(与统计、解析成本同口径;若按字节判,中文文档会晚约 3 倍才触发)
const IDLE_RENDER_CHARS = 100 * 1024    // 超过:渲染挪到空闲时段(带 timeout,不会无限等)
const PAUSE_PREVIEW_CHARS = 256 * 1024  // 超过:暂停自动预览,由「刷新预览」手动触发

// ── 渲染管线 ──
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]))
}
// 源文 HTML 一律按文本展示(需求 §4.2):marked 默认直通原始 HTML,在渲染器层转义
marked.use({
  gfm: true,
  breaks: false,
  renderer: {
    html(token) {
      const raw = typeof token === 'string' ? token : (token && (token.text ?? token.raw)) || ''
      return escapeHtml(raw)
    }
  }
})
// 预览中的链接/图片(设计 §5.1):链接新标签页打开(不丢门户现场),图片不带 referrer
DOMPurify.addHook('afterSanitizeAttributes', (node) => {
  if (node.tagName === 'A') {
    node.setAttribute('target', '_blank')
    node.setAttribute('rel', 'noopener noreferrer')
  } else if (node.tagName === 'IMG') {
    node.setAttribute('referrerpolicy', 'no-referrer')
    node.setAttribute('loading', 'lazy')
  }
})

// ── 状态 ──
const src = ref('')
const html = ref('')
const ta = ref(null)
const previewEl = ref(null)
const splitEl = ref(null)
const fileInput = ref(null)

const mode = ref('split')          // edit | preview | split
const ratio = ref(0.5)
const narrow = ref(false)
const effMode = computed(() => (narrow.value && mode.value === 'split' ? 'edit' : mode.value))
const statsSrc = ref('')
const stats = computed(() => countStats(statsSrc.value))
const previewPaused = computed(() => src.value.length > PAUSE_PREVIEW_CHARS)
const savedAt = ref('')
const saveFailed = ref(false)
const renderErr = ref('')

const dlg = reactive({ show: false, kind: '' })   // import | clear | export
const exportName = ref('')

// ── 渲染(防抖 300ms;大内容走空闲时段) ──
const idleCb = typeof window !== 'undefined' && window.requestIdleCallback
  ? window.requestIdleCallback.bind(window)
  : fn => setTimeout(fn, 0)
const idleCancel = typeof window !== 'undefined' && window.cancelIdleCallback
  ? window.cancelIdleCallback.bind(window)
  : h => clearTimeout(h)

let renderTimer = null
let idleHandle = null
let draftTimer = null
let statsTimer = null
let mqRef = null

function renderNow() {
  const s = src.value
  if (!s.trim()) {
    html.value = ''
    renderErr.value = ''
    return
  }
  try {
    html.value = DOMPurify.sanitize(marked.parse(s))
    renderErr.value = ''
  } catch (e) {
    // 畸形内容(如数千层嵌套引用/列表)会让解析器栈溢出:保留上一次成功结果并提示,
    // 不能让异常冒泡出去导致预览静默停更(源文不受影响,仍可编辑与导出)
    console.warn('[dev-tools] Markdown 渲染失败', e)
    renderErr.value = (e instanceof RangeError || /call stack/i.test(String((e && e.message) || e)))
      ? '内容嵌套过深,预览已暂停更新(源文仍可编辑与导出)'
      : '预览渲染失败(源文仍可编辑与导出)'
  }
}
function scheduleRender() {
  if (renderTimer)
    clearTimeout(renderTimer)
  if (src.value.length > PAUSE_PREVIEW_CHARS)
    return   // 暂停自动预览:等用户点「刷新预览」(否则每次停顿都要卡住主线程数秒)
  renderTimer = setTimeout(() => {
    renderTimer = null
    if (src.value.length > IDLE_RENDER_CHARS) {
      if (idleHandle)
        idleCancel(idleHandle)
      // 带 timeout:页面持续繁忙时也要在 500ms 内跑掉,不能无限等空闲
      idleHandle = idleCb(() => {
        idleHandle = null
        renderNow()
      }, { timeout: 500 })
    } else {
      renderNow()
    }
  }, 300)
}
/// 手动刷新预览(暂停态/空闲态共用):立即渲染一次
function refreshPreview() {
  if (renderTimer) {
    clearTimeout(renderTimer)
    renderTimer = null
  }
  if (idleHandle) {
    idleCancel(idleHandle)
    idleHandle = null
  }
  renderNow()
}
// 统计:小文档即时更新(单遍扫描已足够便宜);大文档按 300ms 节流,避免每次按键全量扫描
function scheduleStats() {
  const v = src.value
  if (v.length <= IDLE_RENDER_CHARS) {
    if (statsTimer) {
      clearTimeout(statsTimer)
      statsTimer = null
    }
    statsSrc.value = v
    return
  }
  if (!statsTimer)
    statsTimer = setTimeout(() => {
      statsTimer = null
      statsSrc.value = src.value
    }, 300)
}

// ── 草稿 ──
function hhmm() {
  const d = new Date()
  const p = n => String(n).padStart(2, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}`
}
function saveDraft(manual) {
  try {
    if (src.value === '') {
      // 内容为空即视为"无草稿":删除键,避免清空草稿后被 beforeunload 又写回空串
      localStorage.removeItem(DRAFT_KEY)
      savedAt.value = ''
    } else {
      localStorage.setItem(DRAFT_KEY, src.value)
      savedAt.value = hhmm()
    }
    saveFailed.value = false
  } catch (e) {
    // 隐私模式禁用存储 / 超出配额:必须让用户知道,不能显示"已保存"
    saveFailed.value = true
    console.warn('[dev-tools] 草稿保存失败', e)
    if (manual)
      toast('草稿保存失败(浏览器存储不可用或已满)', 'err')
  }
}
function scheduleDraft() {
  if (draftTimer)
    clearTimeout(draftTimer)
  draftTimer = setTimeout(() => saveDraft(false), 1000)
}
let justLoaded = false
function loadDraft() {
  try {
    const v = localStorage.getItem(DRAFT_KEY)
    if (v) {
      justLoaded = true   // 载入不算"用户编辑":不刷新"已保存"时间
      src.value = v
      if (v.length <= PAUSE_PREVIEW_CHARS)
        renderNow()   // 超大草稿不自动渲染:由提示条上的「刷新预览」触发
      else
        statsSrc.value = v
    }
  } catch { /* 存储不可用:按无草稿处理,不打断挂载 */ }
}
function onBeforeUnload() {
  saveDraft(false)
}

watch(src, () => {
  scheduleRender()
  scheduleStats()
  if (justLoaded)
    justLoaded = false
  else
    scheduleDraft()
})

// ── 工具条与快捷键(共用同一动作表) ──
const ACTIONS = {
  h1: { pre: '# ', post: '', ph: '标题' },
  h2: { pre: '## ', post: '', ph: '标题' },
  h3: { pre: '### ', post: '', ph: '标题' },
  bold: { pre: '**', post: '**', ph: '加粗文本' },
  italic: { pre: '*', post: '*', ph: '斜体文本' },
  strike: { pre: '~~', post: '~~', ph: '删除线文本' },
  code: { pre: '`', post: '`', ph: 'code' },
  codeblock: { pre: '```\n', post: '\n```', ph: 'code' },
  quote: { pre: '> ', post: '', ph: '引用内容' },
  ul: { pre: '- ', post: '', ph: '列表项' },
  ol: { pre: '1. ', post: '', ph: '列表项' },
  task: { pre: '- [ ] ', post: '', ph: '待办事项' },
  link: { pre: '[', post: '](https://)', ph: '链接文字' },
  image: { pre: '![', post: '](https://)', ph: '图片描述' },
  table: { insert: '\n| 列 1 | 列 2 |\n| --- | --- |\n| 单元格 | 单元格 |\n' },
  hr: { insert: '\n---\n' }
}

const TOOLS = [
  { key: 'h1', label: 'H1', title: '一级标题 (Ctrl/Cmd+Alt+1)' },
  { key: 'h2', label: 'H2', title: '二级标题 (Ctrl/Cmd+Alt+2)' },
  { key: 'h3', label: 'H3', title: '三级标题 (Ctrl/Cmd+Alt+3)' },
  { sep: true },
  { key: 'bold', label: 'B', title: '加粗 (Ctrl/Cmd+B)' },
  { key: 'italic', label: 'I', title: '斜体 (Ctrl/Cmd+I)' },
  { key: 'strike', label: 'S', title: '删除线 (Ctrl/Cmd+U)' },
  { key: 'code', label: '代码', title: '行内代码 (Ctrl/Cmd+E)' },
  { key: 'codeblock', label: '代码块', title: '代码块 (Ctrl/Cmd+`)' },
  { sep: true },
  { key: 'quote', label: '引用', title: '引用' },
  { key: 'ul', label: '无序', title: '无序列表' },
  { key: 'ol', label: '有序', title: '有序列表' },
  { key: 'task', label: '任务', title: '任务列表' },
  { sep: true },
  { key: 'link', label: '链接', title: '链接 (Ctrl/Cmd+K)' },
  { key: 'image', label: '图片', title: '图片 (Ctrl/Cmd+Shift+K)' },
  { key: 'table', label: '表格', title: '表格' },
  { key: 'hr', label: '分割线', title: '分割线' }
]

function apply(key) {
  const a = ACTIONS[key]
  const el = ta.value
  if (!a || !el)
    return
  const start = el.selectionStart
  const end = el.selectionEnd
  const sel = src.value.slice(start, end)
  let s1
  let s2
  if (a.insert !== undefined) {
    // 插入型(表格/分割线):在光标处整行插入,不包裹
    const needNl = start > 0 && src.value[start - 1] !== '\n'
    const text = (needNl ? '\n' : '') + a.insert
    src.value = src.value.slice(0, start) + text + src.value.slice(end)
    s1 = s2 = start + text.length
  } else {
    const body = sel || a.ph || ''
    const text = a.pre + body + a.post
    src.value = src.value.slice(0, start) + text + src.value.slice(end)
    s1 = start + a.pre.length
    s2 = s1 + body.length
  }
  nextTick(() => {
    el.focus()
    el.setSelectionRange(s1, s2)
  })
}

function onKeydown(e) {
  if (!(e.ctrlKey || e.metaKey))
    return
  const k = e.key
  let key = null
  if (k === 'b' || k === 'B')
    key = 'bold'
  else if (k === 'i' || k === 'I')
    key = 'italic'
  else if (k === 'e' || k === 'E')
    key = 'code'
  else if (k === 'u' || k === 'U')
    key = 'strike'
  else if (k === 'k' || k === 'K')
    key = e.shiftKey ? 'image' : 'link'
  else if (k === '`')
    key = 'codeblock'
  else if (e.altKey && (k === '1' || k === '2' || k === '3'
    || e.code === 'Digit1' || e.code === 'Digit2' || e.code === 'Digit3')) {
    // macOS 下 Option+数字 的 e.key 是 '¡'/'™' 之类的符号,故同时按 e.code 识别
    key = `h${k >= '1' && k <= '3' ? k : e.code.slice(-1)}`
  }
  else if (k === 's' || k === 'S') {
    e.preventDefault()
    saveDraft(true)
    if (!saveFailed.value)
      toast('草稿已保存', 'ok')
    return
  }
  if (!key)
    return
  // 已绑定的组合一律阻止浏览器默认行为(Ctrl+U 查看源码、Ctrl+Shift+K 控制台等)
  e.preventDefault()
  apply(key)
}

// ── 分栏拖宽与滚动联动 ──
let dragCleanup = null
function startDrag(e) {
  e.preventDefault()
  const box = splitEl.value
  if (!box)
    return
  const move = ev => {
    const r = (ev.clientX - box.getBoundingClientRect().left) / box.clientWidth
    ratio.value = Math.min(0.8, Math.max(0.2, r))
  }
  const up = () => {
    window.removeEventListener('mousemove', move)
    window.removeEventListener('mouseup', up)
    dragCleanup = null
  }
  window.addEventListener('mousemove', move)
  window.addEventListener('mouseup', up)
  dragCleanup = up   // 卸载兜底:拖动中被卸载也要摘掉监听
}

let rafPending = false
function onPreviewScroll() {
  if (effMode.value !== 'split' || rafPending)
    return
  rafPending = true
  requestAnimationFrame(() => {
    rafPending = false
    const p = previewEl.value
    const el = ta.value
    if (!p || !el)
      return
    const pMax = Math.max(1, p.scrollHeight - p.clientHeight)
    const tMax = Math.max(0, el.scrollHeight - el.clientHeight)
    el.scrollTop = (p.scrollTop / pMax) * tMax
  })
}

// ── 导入 / 导出 / 清空草稿 ──
function pickFile() {
  if (src.value.trim()) {
    dlg.kind = 'import'
    dlg.show = true
    return
  }
  doPickFile()
}
function doPickFile() {
  dlg.show = false
  nextTick(() => fileInput.value && fileInput.value.click())
}
function onFileChange(e) {
  const f = e.target.files && e.target.files[0]
  e.target.value = ''
  if (!f)
    return
  const reader = new FileReader()
  reader.onload = () => {
    const text = String(reader.result || '')
    src.value = text
    if (text.length <= PAUSE_PREVIEW_CHARS)
      renderNow()
    toast(`已导入 ${f.name}`, 'ok')
  }
  reader.onerror = () => toast('文件读取失败', 'err')
  reader.readAsText(f)
}

function defaultName() {
  const d = new Date()
  const p = n => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}-untitled.md`
}
function openExport() {
  exportName.value = defaultName()
  dlg.kind = 'export'
  dlg.show = true
}
function doExport() {
  let name = (exportName.value || '').trim() || defaultName()
  name = name.replace(/[\\/:*?"<>|]/g, '_')
  if (!/\.(md|markdown|txt)$/i.test(name))
    name += '.md'
  const url = URL.createObjectURL(new Blob([src.value], { type: 'text/markdown;charset=utf-8' }))
  const a = document.createElement('a')
  a.href = url
  a.download = name
  document.body.appendChild(a)
  a.click()
  a.remove()
  URL.revokeObjectURL(url)
  dlg.show = false
  toast(`已导出 ${name}`, 'ok')
}
function doClearDraft() {
  try {
    localStorage.removeItem(DRAFT_KEY)
  } catch { /* 存储不可用:仅清空编辑区 */ }
  src.value = ''
  html.value = ''
  savedAt.value = ''
  saveFailed.value = false
  dlg.show = false
  toast('草稿已清空', 'ok')
}

// ── 生命周期 ──
function onMqChange(e) {
  narrow.value = e.matches
  if (narrow.value && mode.value === 'split')
    mode.value = 'edit'
}

onMounted(() => {
  loadDraft()
  mqRef = window.matchMedia('(max-width:768px)')
  narrow.value = mqRef.matches
  if (narrow.value && mode.value === 'split')
    mode.value = 'edit'
  mqRef.addEventListener('change', onMqChange)
  window.addEventListener('beforeunload', onBeforeUnload)
})
onBeforeUnmount(() => {
  if (dragCleanup)
    dragCleanup()   // 拖动中被卸载:摘掉 window 上的 mousemove/mouseup
  // 卸载前把未落盘的编辑补写一次(权限被撤销导致卸载时,最后一次输入不能丢)
  if (draftTimer) {
    clearTimeout(draftTimer)
    draftTimer = null
    saveDraft(false)
  }
  window.removeEventListener('beforeunload', onBeforeUnload)
  if (mqRef)
    mqRef.removeEventListener('change', onMqChange)
  if (renderTimer)
    clearTimeout(renderTimer)
  if (statsTimer) {
    clearTimeout(statsTimer)
    statsTimer = null
  }
  if (idleHandle)
    idleCancel(idleHandle)
})
</script>

<template>
  <div class="md-wrap">
    <div class="dt-bar">
      <div class="seg dt-seg" role="group" aria-label="视图模式">
        <button type="button" class="seg-item" :class="{ active: mode === 'edit' }"
                @click="mode = 'edit'">仅编辑</button>
        <button type="button" class="seg-item" :class="{ active: mode === 'preview' }"
                @click="mode = 'preview'">仅预览</button>
        <button v-if="!narrow" type="button" class="seg-item" :class="{ active: mode === 'split' }"
                @click="mode = 'split'">分栏</button>
      </div>
      <span class="sp"></span>
      <span class="dt-cap">草稿保存在本机浏览器,不区分账号</span>
      <button type="button" class="btn btn-ghost btn-sm" @click="pickFile">导入</button>
      <button type="button" class="btn btn-ghost btn-sm" @click="openExport">导出</button>
      <button type="button" class="btn btn-ghost btn-sm" @click="dlg.kind = 'clear'; dlg.show = true">清空草稿</button>
    </div>

    <div class="md-toolbar">
      <template v-for="(t, i) in TOOLS" :key="t.key || `sep${i}`">
        <span v-if="t.sep" class="md-sep"></span>
        <button v-else type="button" class="md-tb" :title="t.title" :aria-label="t.title"
                @click="apply(t.key)">{{ t.label }}</button>
      </template>
    </div>

    <div ref="splitEl" class="md-split">
      <div v-if="effMode !== 'preview'" class="dt-pane md-pane"
           :style="effMode === 'split' ? { flex: ratio } : null">
        <textarea ref="ta" v-model="src" class="dt-ta" spellcheck="false"
                  placeholder="在此编写 Markdown…" @keydown="onKeydown"></textarea>
      </div>
      <div v-if="effMode === 'split'" class="md-divider" title="拖动调整栏宽" @mousedown="startDrag"></div>
      <div v-if="effMode !== 'edit'" class="dt-pane md-pane"
           :style="effMode === 'split' ? { flex: 1 - ratio } : null">
        <div v-if="previewPaused" class="dt-warn" style="margin:8px 10px 0">
          <span aria-hidden="true">⚠</span>
          <span>内容较大({{ stats.chars }} 字符),已暂停自动预览</span>
          <button type="button" class="btn btn-secondary btn-sm" style="margin-left:auto"
                  @click="refreshPreview">刷新预览</button>
        </div>
        <div v-if="renderErr" class="dt-warn" style="margin:8px 10px 0">
          <span aria-hidden="true">⚠</span><span>{{ renderErr }}</span>
        </div>
        <div ref="previewEl" class="md-preview" @scroll="onPreviewScroll">
          <div v-if="!src.trim()" class="md-empty">
            <div>预览区是空的</div>
            <div class="dt-cap">在编辑区输入 Markdown 内容即可实时预览</div>
          </div>
          <div v-else class="md-body" v-html="html"></div>
        </div>
      </div>
    </div>

    <div class="md-status">
      <span>字符 <b>{{ stats.chars }}</b></span>
      <span>行 <b>{{ stats.lines }}</b></span>
      <span>字数 <b>{{ stats.words }}</b></span>
      <span class="sp"></span>
      <span v-if="saveFailed" class="save-fail">草稿保存失败(浏览器存储不可用或已满)</span>
      <span v-else-if="savedAt">已保存 {{ savedAt }}</span>
    </div>

    <input ref="fileInput" type="file" accept=".md,.markdown,.txt" style="display:none"
           @change="onFileChange" />

    <Modal :show="dlg.show && dlg.kind === 'import'" title="导入文件" @close="dlg.show = false">
      <p class="dlg-tip" style="margin:0">导入会替换当前编辑区内容,确定继续?</p>
      <template #foot>
        <button type="button" class="btn btn-ghost" @click="dlg.show = false">取消</button>
        <button type="button" class="btn btn-primary" @click="doPickFile">选择文件</button>
      </template>
    </Modal>

    <Modal :show="dlg.show && dlg.kind === 'clear'" title="清空草稿" @close="dlg.show = false">
      <p class="dlg-tip" style="margin:0">将删除本机保存的草稿并清空编辑区,此操作不可撤销。</p>
      <template #foot>
        <button type="button" class="btn btn-ghost" @click="dlg.show = false">取消</button>
        <button type="button" class="btn btn-danger" @click="doClearDraft">清空</button>
      </template>
    </Modal>

    <Modal :show="dlg.show && dlg.kind === 'export'" title="导出为 Markdown" @close="dlg.show = false">
      <div class="form-item" style="margin-bottom:0">
        <label class="form-label" for="md-export-name">文件名</label>
        <input id="md-export-name" v-model="exportName" class="input" spellcheck="false" />
        <span class="form-hint">默认 YYYY-MM-DD-untitled.md;不带扩展名时自动补 .md</span>
      </div>
      <template #foot>
        <button type="button" class="btn btn-ghost" @click="dlg.show = false">取消</button>
        <button type="button" class="btn btn-primary" @click="doExport">导出</button>
      </template>
    </Modal>
  </div>
</template>
