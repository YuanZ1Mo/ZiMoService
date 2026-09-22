<script setup>
// 生成器(二期 A):UUID v4 / 强密码 / 随机 Token
// 随机源一律用 crypto.getRandomValues;取字符用拒绝采样,避免取模偏差
import { ref, reactive, inject, onMounted } from 'vue'
import ToolShell from './ToolShell.vue'
import { copyText } from './toolbox'

const toast = inject('toast')

const KIND = [
  { key: 'uuid', name: 'UUID v4' },
  { key: 'password', name: '强密码' },
  { key: 'token', name: '随机 Token' }
]
const SETS = {
  upper: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
  lower: 'abcdefghijklmnopqrstuvwxyz',
  digit: '0123456789',
  symbol: '!@#$%^&*()-_=+[]{};:,.?/'
}
const AMBIGUOUS = '0O1lI|`'

const kind = ref('uuid')
const count = ref(5)
const pwdLen = ref(16)
const tokLen = ref(32)
const sets = reactive({ upper: true, lower: true, digit: true, symbol: true })
const noAmbiguous = ref(false)
const out = ref('')
const errMsg = ref('')
const outEl = ref(null)

const hasCrypto = typeof crypto !== 'undefined' && typeof crypto.getRandomValues === 'function'

/// [0, max) 均匀随机:拒绝采样(朴素取模会让前几个值概率偏高)
function randInt(max) {
  const limit = Math.floor(0xFFFFFFFF / max) * max
  const buf = new Uint32Array(1)
  let v = 0
  do {
    crypto.getRandomValues(buf)
    v = buf[0]
  } while (v >= limit)
  return v % max
}
function uuidV4() {
  if (typeof crypto.randomUUID === 'function')
    return crypto.randomUUID()
  // 老环境回退:自行按 RFC 4122 v4 置位(getRandomValues 在非安全上下文也可用)
  const b = new Uint8Array(16)
  crypto.getRandomValues(b)
  b[6] = (b[6] & 0x0F) | 0x40
  b[8] = (b[8] & 0x3F) | 0x80
  const h = Array.from(b, x => x.toString(16).padStart(2, '0'))
  return `${h.slice(0, 4).join('')}-${h.slice(4, 6).join('')}-${h.slice(6, 8).join('')}-${h.slice(8, 10).join('')}-${h.slice(10, 16).join('')}`
}
function poolOf(key) {
  let s = SETS[key]
  if (noAmbiguous.value)
    s = Array.from(s).filter(ch => !AMBIGUOUS.includes(ch)).join('')
  return s
}
function genPassword(len) {
  const active = Object.keys(SETS).filter(k => sets[k])
  if (!active.length)
    throw new Error('至少选择一种字符集')
  const pool = active.map(k => poolOf(k)).filter(Boolean).join('')
  if (!pool)
    throw new Error('排除易混字符后没有可用字符,请多选一种字符集')
  const chars = []
  // 每个已选字符集至少命中一个,保证强度下限
  for (const k of active) {
    const s = poolOf(k)
    if (s)
      chars.push(s[randInt(s.length)])
  }
  while (chars.length < len)
    chars.push(pool[randInt(pool.length)])
  // Fisher–Yates 洗牌,避免"各字符集各一个"固定在开头
  for (let i = chars.length - 1; i > 0; i--) {
    const j = randInt(i + 1)
    ;[chars[i], chars[j]] = [chars[j], chars[i]]
  }
  return chars.join('')
}
function genToken(len) {
  const bytes = new Uint8Array(Math.ceil((len * 3) / 4) + 3)
  crypto.getRandomValues(bytes)
  let bin = ''
  for (const b of bytes)
    bin += String.fromCharCode(b)
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '').slice(0, len)
}

function generate() {
  if (!hasCrypto) {
    errMsg.value = '当前环境不支持安全随机数(crypto.getRandomValues),无法生成'
    out.value = ''
    return
  }
  const n = Math.min(50, Math.max(1, Number(count.value) || 1))
  const len = Math.min(256, Math.max(4, Number(kind.value === 'password' ? pwdLen.value : tokLen.value) || 16))
  try {
    const rows = []
    for (let i = 0; i < n; i++) {
      if (kind.value === 'uuid')
        rows.push(uuidV4())
      else if (kind.value === 'password')
        rows.push(genPassword(len))
      else
        rows.push(genToken(len))
    }
    out.value = rows.join('\n')
    errMsg.value = ''
  } catch (e) {
    out.value = ''
    errMsg.value = String((e && e.message) || e)
  }
}
function switchKind(k) {
  kind.value = k
  generate()
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

onMounted(generate)
</script>

<template>
  <ToolShell :error="errMsg">
    <template #bar>
      <div class="seg dt-seg" role="group" aria-label="生成类型">
        <button v-for="k in KIND" :key="k.key" type="button" class="seg-item"
                :class="{ active: kind === k.key }" :aria-pressed="kind === k.key"
                @click="switchKind(k.key)">{{ k.name }}</button>
      </div>
      <span class="sp"></span>
      <button type="button" class="btn btn-primary btn-sm" @click="generate">生成</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="doCopy">复制结果</button>
    </template>

    <div class="dt-pane dt-pane-fixed">
      <div class="dt-pane-head">
        <span class="dt-pane-title">选项</span>
        <span class="dt-cap">生成结果不落任何存储,刷新即丢</span>
      </div>
      <div style="padding:12px 14px" class="dt-row">
        <label class="dt-field">数量
          <input v-model="count" class="input dt-num" type="number" min="1" max="50" />
        </label>
        <label v-if="kind === 'password'" class="dt-field">密码长度
          <input v-model="pwdLen" class="input dt-num" type="number" min="4" max="256" />
        </label>
        <label v-else-if="kind === 'token'" class="dt-field">Token 长度
          <input v-model="tokLen" class="input dt-num" type="number" min="4" max="256" />
        </label>
        <template v-if="kind === 'password'">
          <span class="dt-checks">
            <label><input v-model="sets.upper" type="checkbox" />大写</label>
            <label><input v-model="sets.lower" type="checkbox" />小写</label>
            <label><input v-model="sets.digit" type="checkbox" />数字</label>
            <label><input v-model="sets.symbol" type="checkbox" />符号</label>
            <label><input v-model="noAmbiguous" type="checkbox" />排除易混(0O1lI)</label>
          </span>
        </template>
      </div>
    </div>

    <div class="dt-pane dt-pane-fill">
      <div class="dt-pane-head">
        <span class="dt-pane-title">结果</span>
        <span class="dt-cap">每行一个</span>
      </div>
      <textarea ref="outEl" class="dt-ta wrap" readonly :value="out" placeholder="点「生成」出结果"></textarea>
    </div>

    <template #status>
      <span>已生成:<b>{{ out ? out.split('\n').length : 0 }}</b> 条</span>
      <span class="sp"></span>
      <span class="dt-hint">随机源:crypto.getRandomValues(拒绝采样,无取模偏差)</span>
    </template>
  </ToolShell>
</template>
