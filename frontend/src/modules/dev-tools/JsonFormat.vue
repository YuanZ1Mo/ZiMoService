<script setup>
// 工具一:JSON 格式化(设计 §4)
// 用标准 JSON API 解析/序列化(不用 eval,可循环安全);错误按多格式定位到行/列,
// 原因优先给中文文案,未命中映射时原样展示引擎消息;>2MB 关闭自动重算改按钮触发。
import { ref, computed, watch, nextTick, onBeforeUnmount, inject } from 'vue'
import Modal from '../../components/Modal.vue'
import ToolShell from './ToolShell.vue'
import { copyText } from './toolbox'
import { countStats } from './stats'

const toast = inject('toast')

const SAMPLE = `{
  "name": "ZiMo 小工具",
  "version": "1.0",
  "tags": ["json", "format", "本地处理"],
  "enabled": true,
  "nested": { "level": 2, "note": "支持中文与 \\u4e2d 转义" },
  "empty": null,
  "list": [{ "id": 1, "ok": false }, { "id": 2, "ok": true }]
}`

const BIG_CHARS = 2 * 1024 * 1024
const INDENT_MAP = { 2: '  ', 4: '    ', tab: '\t' }

const src = ref('')
const out = ref('')
const err = ref(null)          // { line, col, msg };line=0 表示引擎未给位置
const indent = ref('2')
const mode = ref('pretty')     // pretty | compact
const big = ref(false)
const dropping = ref(false)
const askSample = ref(false)

const srcTa = ref(null)
const outTa = ref(null)

const inStats = computed(() => countStats(src.value))
const outStats = computed(() => countStats(out.value))
const errWhere = computed(() =>
  err.value && err.value.line ? `第 ${err.value.line} 行第 ${err.value.col} 列:` : '')
/// ToolShell 错误条文案:错误条与输入区同处一屏,不再塞在输出面板内部
const errText = computed(() => (err.value ? `${errWhere.value}${err.value.msg}` : ''))
const indentLabel = computed(() =>
  mode.value === 'compact' ? '压缩(单行)' : `格式化(缩进 ${indent.value === 'tab' ? 'Tab' : indent.value + ' 空格'})`)

// ── 重算:输入防抖 300ms;大文件模式不自动算 ──
// 大文件判定按**字符数**(与 JSON.parse 的成本同口径);不再每次按键全量 encode 拷贝
let timer = null
watch(src, () => {
  const isBig = src.value.length > BIG_CHARS
  if (isBig !== big.value)
    big.value = isBig
  if (isBig)
    return   // 大文件:暂停自动重算,避免每次停顿全量 parse + stringify 卡住输入
  if (timer)
    clearTimeout(timer)
  timer = setTimeout(() => compute(false), 300)
})
onBeforeUnmount(() => {
  if (timer)
    clearTimeout(timer)
})

/**
 * 解析并产出
 * @param manual true = 用户手动触发(此时才把光标定位到出错处)
 */
function compute(manual) {
  if (timer) {
    clearTimeout(timer)
    timer = null
  }
  const s = src.value
  if (!s.trim()) {
    out.value = ''
    err.value = null
    return
  }
  try {
    const value = JSON.parse(s)
    out.value = mode.value === 'compact'
      ? JSON.stringify(value)
      : JSON.stringify(value, null, INDENT_MAP[indent.value] || '  ')
    err.value = null
  } catch (e) {
    out.value = ''                     // 清空:不保留上一次成功输出,不渲染半成品
    err.value = toError(e, s)
    if (manual && err.value.line)
      focusError(err.value)
  }
}

// ── 错误定位与原因 ──
/// 常见引擎消息 → 中文说明;未命中返回原文(不臆造原因)
const REASONS = [
  [/unexpected end of (json )?input|unexpected end of data/i, '内容意外结束(括号或引号未闭合)'],
  [/unexpected non-whitespace character after json/i, 'JSON 结束后还有多余内容'],
  [/expected double-quoted property name/i, '属性名(键)必须用双引号包裹'],
  [/expected property name or '\}'/i, '此处应为属性名(键)或 },常见原因是多余的逗号'],
  [/expected ',' or '\}'/i, '此处应为 , 或 }'],
  [/expected ',' or '\]'/i, '此处应为 , 或 ]'],
  [/expected ':' after property name/i, '属性名之后缺少 :'],
  [/unterminated string/i, '字符串未闭合(缺少结尾的双引号)'],
  [/bad escaped character/i, '非法的转义字符'],
  [/bad control character/i, '字符串里出现未转义的控制字符'],
  [/invalid (unicode|character)/i, '出现了非法字符'],
  [/unexpected token/i, '出现了意外的字符(常见原因:多余的逗号、缺少引号或括号不匹配)']
]

function reasonOf(msg) {
  for (const [re, text] of REASONS) {
    if (re.test(msg))
      return text
  }
  return msg
}

function posToLineCol(s, pos) {
  const p = Math.max(0, Math.min(pos, s.length))
  const head = s.slice(0, p)
  const nl = head.lastIndexOf('\n')
  return { line: head.split('\n').length, col: p - nl }
}

function offsetOf(s, line, col) {
  const lines = s.split('\n')
  let off = 0
  for (let i = 0; i < line - 1 && i < lines.length; i++)
    off += lines[i].length + 1
  return off + Math.max(0, col - 1)
}

function toError(e, s) {
  const msg = String((e && e.message) || e || '')
  if (e instanceof RangeError || /call stack/i.test(msg))
    return { line: 0, col: 0, msg: '内容过大或嵌套过深' }
  // 1) 引擎直接给出行列(Chrome/V8 新版、Node)
  let m = msg.match(/\(line (\d+) column (\d+)\)/i)
  if (m)
    return { line: Number(m[1]), col: Number(m[2]), msg: reasonOf(msg) }
  // 2) 只给 position 的旧版/精简消息:回溯输入串换算
  m = msg.match(/at position (\d+)/i)
  if (m) {
    const p = posToLineCol(s, Number(m[1]))
    return { line: p.line, col: p.col, msg: reasonOf(msg) }
  }
  // 3) 都没有(Firefox/Safari 文案不同):不定位,只展示原因
  return { line: 0, col: 0, msg: reasonOf(msg) }
}

function focusError(info) {
  const ta = srcTa.value
  if (!ta)
    return
  const off = offsetOf(src.value, info.line, info.col)
  nextTick(() => {
    ta.focus()
    ta.setSelectionRange(off, Math.min(off + 1, src.value.length))
  })
}

// ── 操作 ──
function doPretty() {
  mode.value = 'pretty'
  compute(true)
}
function doMinify() {
  mode.value = 'compact'
  compute(true)
}
function setIndent(v) {
  indent.value = v
  if (mode.value === 'pretty')
    compute(true)
}
function loadSample() {
  if (src.value.trim()) {
    askSample.value = true
    return
  }
  applySample()
}
function applySample() {
  askSample.value = false
  src.value = SAMPLE
  mode.value = 'pretty'
  big.value = false
  compute(true)
}

async function copyOut() {
  const r = await copyText(out.value, outTa.value)
  if (r === 'ok')
    toast('已复制到剪贴板', 'ok')
  else if (r === 'fail')
    toast('没有可复制的内容', 'warn')
  else
    toast('已选中,请手动复制(Ctrl+C)', 'warn')
}

function fillBack() {
  if (!out.value) {
    toast('没有可回填的内容', 'warn')
    return
  }
  src.value = out.value
  compute(true)
}

function onDrop(e) {
  dropping.value = false
  const dt = e.dataTransfer
  // 拖入文件夹时浏览器只给一个 0 字节"文件":直接拒绝,避免把当前输入清空
  const entry = dt && dt.items && dt.items[0] && dt.items[0].webkitGetAsEntry
    ? dt.items[0].webkitGetAsEntry()
    : null
  if (entry && entry.isDirectory) {
    toast('不支持拖入文件夹,请拖入单个文件或直接粘贴内容', 'warn')
    return
  }
  const f = dt && dt.files && dt.files[0]
  if (!f) {
    toast('未识别到文件,可直接粘贴内容', 'warn')
    return
  }
  const reader = new FileReader()
  reader.onload = () => {
    src.value = String(reader.result || '')
    compute(true)
  }
  reader.onerror = () => toast('文件读取失败', 'err')
  reader.readAsText(f)
}
</script>

<template>
  <ToolShell :error="errText">
    <template #bar>
      <div class="seg dt-seg" role="group" aria-label="缩进方式">
        <button type="button" class="seg-item" :class="{ active: indent === '2' }"
                :aria-pressed="indent === '2'" @click="setIndent('2')">2 空格</button>
        <button type="button" class="seg-item" :class="{ active: indent === '4' }"
                :aria-pressed="indent === '4'" @click="setIndent('4')">4 空格</button>
        <button type="button" class="seg-item" :class="{ active: indent === 'tab' }"
                :aria-pressed="indent === 'tab'" @click="setIndent('tab')">Tab</button>
      </div>
      <button type="button" class="btn btn-primary btn-sm" @click="doPretty">格式化</button>
      <button type="button" class="btn btn-secondary btn-sm" @click="doMinify">压缩</button>
      <span class="sp"></span>
      <button type="button" class="btn btn-ghost btn-sm" @click="loadSample">载入示例</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="copyOut">复制结果</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="fillBack">回填到输入</button>
    </template>

    <template #notice>
      <div v-if="big" class="dt-warn">
        <span aria-hidden="true">⚠</span>
        <span>内容较大(超过 200 万字符),已暂停自动重算 —— 请点「格式化」或「压缩」手动执行</span>
      </div>
    </template>

    <div class="dt-cols">
      <div class="dt-pane" :class="{ 'dt-drop': dropping }"
           @dragover.prevent="dropping = true" @dragleave="dropping = false" @drop.prevent="onDrop">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输入</span>
          <span class="dt-cap">直接粘贴,或把文件拖进来</span>
        </div>
        <textarea ref="srcTa" v-model="src" class="dt-ta" spellcheck="false"
                  placeholder="在此粘贴 JSON…"></textarea>
      </div>

      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输出</span>
          <span class="dt-cap">{{ indentLabel }}</span>
        </div>
        <textarea ref="outTa" class="dt-ta wrap" readonly :value="out"
                  :placeholder="err ? '语法错误:已在输入区标出位置' : '格式化 / 压缩结果会显示在这里'"></textarea>
      </div>
    </div>

    <template #status>
      <span>输入:<b>{{ inStats.chars }}</b> 字符 · <b>{{ inStats.lines }}</b> 行</span>
      <span>输出:<b>{{ outStats.chars }}</b> 字符 · <b>{{ outStats.lines }}</b> 行</span>
    </template>

    <Modal :show="askSample" title="载入示例" @close="askSample = false">
      <p class="dlg-tip" style="margin:0">载入示例会覆盖当前输入内容,确定继续?</p>
      <template #foot>
        <button type="button" class="btn btn-ghost" @click="askSample = false">取消</button>
        <button type="button" class="btn btn-primary" @click="applySample">覆盖并载入</button>
      </template>
    </Modal>
  </ToolShell>
</template>
