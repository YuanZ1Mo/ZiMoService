<script setup>
// 编解码(二期 A):Base64 / URL 组件 / HTML 实体 / Unicode 转义 的编解码
// Base64 一律走 UTF-8 字节(先 TextEncoder → btoa),大字符串分块喂 fromCharCode 防栈溢出
import { ref, inject } from 'vue'
import ToolShell from './ToolShell.vue'
import { copyText, useHeavyInput, fmtCount, lineCount, isBlank } from './toolbox'

const toast = inject('toast')

const KIND = [
  { key: 'base64', name: 'Base64' },
  { key: 'url', name: 'URL 组件' },
  { key: 'html', name: 'HTML 实体' },
  { key: 'unicode', name: 'Unicode 转义' }
]
const kind = ref('base64')
const dir = ref('encode')     // encode | decode
const src = ref('')
const out = ref('')
const errMsg = ref('')

const { paused, schedule, runNow } = useHeavyInput(src)
const outEl = ref(null)

// ── Base64(UTF-8 安全) ──
function b64Encode(s) {
  const bytes = new TextEncoder().encode(s)
  const CHUNK = 0x8000
  let bin = ''
  for (let i = 0; i < bytes.length; i += CHUNK)
    bin += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK))
  return btoa(bin)
}
function b64Decode(s) {
  const clean = s.replace(/\s+/g, '')
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(clean))
    throw new Error('含非 Base64 字符(只允许 A-Z a-z 0-9 + / 与结尾 =)')
  const bin = (() => {
    try {
      return atob(clean)
    } catch {
      // atob 对长度非 4 倍数/非法补位会抛 DOMException,原文照抄给用户看不懂
      throw new Error('Base64 内容不完整或长度不合法(长度需为 4 的倍数,可用 = 补齐)')
    }
  })()
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++)
    bytes[i] = bin.charCodeAt(i)
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes)
  } catch {
    throw new Error('解码结果不是合法 UTF-8 文本(可能不是文本型内容)')
  }
}

// ── URL 组件 ──
function urlEncode(s) {
  return encodeURIComponent(s)
}
function urlDecode(s) {
  try {
    return decodeURIComponent(s)
  } catch {
    throw new Error('含非法的百分号转义序列(如孤立的 % 或 %ZZ)')
  }
}

// ── HTML 实体 ──
// 编码:用文本节点序列化(& < > 会被转义)。
// 注意不能用"游离 textarea 的 innerHTML":textarea.value 不产生子节点,读出来是空串。
// 解码:用 textarea 的 RCDATA 语义 —— 实体被解析、原文标签仍按文本保留(不创建元素、不加载资源)。
function htmlEncode(s) {
  const d = document.createElement('div')
  d.textContent = s
  return d.innerHTML
}
function htmlDecode(s) {
  const ta = document.createElement('textarea')
  ta.innerHTML = s
  return ta.textContent || ''
}

// ── Unicode 转义(\uXXXX;增补平面按代理对输出两个 \uXXXX) ──
function uniEncode(s) {
  let r = ''
  for (const ch of s) {
    const cp = ch.codePointAt(0)
    if (cp > 0xFFFF) {
      const h = cp - 0x10000
      r += `\\u${(0xD800 + (h >> 10)).toString(16).toUpperCase().padStart(4, '0')}`
      r += `\\u${(0xDC00 + (h & 0x3FF)).toString(16).toUpperCase().padStart(4, '0')}`
    } else if (cp > 0x7E || cp < 0x20) {
      r += `\\u${cp.toString(16).toUpperCase().padStart(4, '0')}`
    } else {
      r += ch
    }
  }
  return r
}
function uniDecode(s) {
  const bad = s.replace(/\\u\{[0-9a-fA-F]{1,6}\}|\\u[0-9a-fA-F]{4}/g, '')
  if (bad.includes('\\u'))
    throw new Error('存在不完整的 \\u 转义(应为 \\uXXXX 或 \\u{XXXXX})')
  return s
    .replace(/\\u\{([0-9a-fA-F]{1,6})\}/g, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/\\u([0-9a-fA-F]{4})/g, (_, h) => String.fromCharCode(parseInt(h, 16)))
}

function compute() {
  if (isBlank(src.value)) {
    out.value = ''
    errMsg.value = ''
    return
  }
  try {
    const s = src.value
    let r
    if (kind.value === 'base64')
      r = dir.value === 'encode' ? b64Encode(s) : b64Decode(s)
    else if (kind.value === 'url')
      r = dir.value === 'encode' ? urlEncode(s) : urlDecode(s)
    else if (kind.value === 'html')
      r = dir.value === 'encode' ? htmlEncode(s) : htmlDecode(s)
    else
      r = dir.value === 'encode' ? uniEncode(s) : uniDecode(s)
    out.value = r
    errMsg.value = ''
  } catch (e) {
    out.value = ''
    errMsg.value = String((e && e.message) || e)
  }
}

function onInput(e) {
  src.value = e.target.value
  schedule(compute)
}
function switchKind(k) {
  kind.value = k
  runNow(compute)
}
function switchDir(d) {
  dir.value = d
  runNow(compute)
}
function fillBack() {
  if (!out.value) {
    toast('没有可回填的内容', 'warn')
    return
  }
  src.value = out.value
  runNow(compute)
}
async function doCopy() {
  const r = await copyText(out.value, outEl.value)
  if (r === 'ok')
    toast('已复制到剪贴板', 'ok')
  else if (r === 'fail')
    toast('没有可复制的内容', 'warn')
  else
    toast('已选中,请手动复制(Ctrl+C)', 'warn')
}
</script>

<template>
  <ToolShell :error="errMsg">
    <template #bar>
      <div class="seg dt-seg" role="group" aria-label="编码类型">
        <button v-for="k in KIND" :key="k.key" type="button" class="seg-item"
                :class="{ active: kind === k.key }" :aria-pressed="kind === k.key"
                @click="switchKind(k.key)">{{ k.name }}</button>
      </div>
      <div class="seg dt-seg" role="group" aria-label="方向">
        <button type="button" class="seg-item" :class="{ active: dir === 'encode' }"
                :aria-pressed="dir === 'encode'" @click="switchDir('encode')">编码</button>
        <button type="button" class="seg-item" :class="{ active: dir === 'decode' }"
                :aria-pressed="dir === 'decode'" @click="switchDir('decode')">解码</button>
      </div>
      <span class="sp"></span>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="fillBack">输出回填</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="doCopy">复制结果</button>
    </template>

    <template #notice>
      <div v-if="paused" class="dt-warn">
        <span aria-hidden="true">⚠</span>
        <span>内容较大({{ fmtCount(src.length) }} 字符),已暂停自动转换</span>
        <button type="button" class="btn btn-secondary btn-sm" style="margin-left:auto" @click="runNow(compute)">刷新结果</button>
      </div>
    </template>

    <div class="dt-cols">
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输入</span>
          <span class="dt-cap">{{ dir === 'encode' ? '待编码原文' : '待解码内容' }}</span>
        </div>
        <textarea class="dt-ta" spellcheck="false" placeholder="在此粘贴内容…" :value="src"
                  @input="onInput"></textarea>
      </div>
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输出</span>
          <span class="dt-cap">{{ dir === 'encode' ? '编码结果' : '解码结果' }}</span>
        </div>
        <textarea ref="outEl" class="dt-ta wrap" readonly :value="out"
                  :placeholder="errMsg ? '转换失败:见上方错误提示' : '结果会显示在这里'"></textarea>
      </div>
    </div>

    <template #status>
      <span>输入:<b>{{ lineCount(src) }}</b> 行</span>
      <span>输出:<b>{{ fmtCount(out.length) }}</b> 字符</span>
      <span class="sp"></span>
      <span class="dt-hint">Base64 按 UTF-8 处理;Unicode 转义支持 \uXXXX 与 \u{XXXXX}</span>
    </template>
  </ToolShell>
</template>
