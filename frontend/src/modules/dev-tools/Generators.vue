<script setup>
// 生成器(二期 A):UUID v4 / 强密码 / 随机 Token
// 随机源一律用 crypto.getRandomValues;取字符用拒绝采样,避免取模偏差
import { ref, reactive, inject } from 'vue'
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

const COUNT_MAX = 50
const LEN_MIN = 4
const LEN_MAX = 256
const kind = ref('uuid')
const count = ref(5)
const clampWarn = ref('')
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

/// 数值钳制:number 输入的 min/max 只做表单校验提示、不阻止输入,超限在此收敛并给出提示
function clampNum(field, min, max, label) {
  let n = Math.floor(Number(field.value))
  if (!Number.isFinite(n) || n < min)
    n = min
  if (n > max) {
    n = max
    clampWarn.value = `${label}超出范围(${min}~${max}),已按 ${max} 处理`
  } else {
    clampWarn.value = ''
  }
  field.value = n
}
/// 数量(1~50)
function clampCount() {
  clampNum(count, 1, COUNT_MAX, '数量')
}
/// 密码/Token 长度(4~256):按当前类型取对应字段
function clampLen() {
  const isPwd = kind.value === 'password'
  clampNum(isPwd ? pwdLen : tokLen, LEN_MIN, LEN_MAX, isPwd ? '密码长度' : 'Token 长度')
}
function generate() {
  if (!hasCrypto) {
    errMsg.value = '当前环境不支持安全随机数(crypto.getRandomValues),无法生成'
    out.value = ''
    return
  }
  const n = Math.min(COUNT_MAX, Math.max(1, Number(count.value) || 1))
  const len = Math.min(LEN_MAX, Math.max(LEN_MIN, Number(kind.value === 'password' ? pwdLen.value : tokLen.value) || 16))
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
// 切换类型只换输入项,不重算:结果由「生成」按钮产出,切换不清空上次结果
function switchKind(k) {
  kind.value = k
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
      <div class="seg dt-seg" role="group" aria-label="生成类型">
        <button v-for="k in KIND" :key="k.key" type="button" class="seg-item"
                :class="{ active: kind === k.key }" :aria-pressed="kind === k.key"
                @click="switchKind(k.key)">{{ k.name }}</button>
      </div>
      <span class="sp"></span>
      <button type="button" class="btn btn-primary btn-sm" @click="generate">生成</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="doCopy">复制结果</button>
    </template>

    <template #notice>
      <div v-if="clampWarn" class="dt-warn">
        <span aria-hidden="true">⚠</span>
        <span>{{ clampWarn }}</span>
      </div>
    </template>

    <div class="dt-pane dt-pane-fixed">
      <div class="dt-pane-head">
        <span class="dt-pane-title">选项</span>
        <span class="dt-cap">生成结果不落任何存储,刷新即丢</span>
      </div>
      <div style="padding:12px 14px" class="dt-row">
        <label class="dt-field">数量
          <input v-model="count" class="input dt-num" type="number" min="1" :max="COUNT_MAX"
                 @change="clampCount" />
        </label>
        <label v-if="kind === 'password'" class="dt-field">密码长度
          <input v-model="pwdLen" class="input dt-num" type="number" :min="LEN_MIN" :max="LEN_MAX"
                 @change="clampLen" />
        </label>
        <label v-else-if="kind === 'token'" class="dt-field">Token 长度
          <input v-model="tokLen" class="input dt-num" type="number" :min="LEN_MIN" :max="LEN_MAX"
                 @change="clampLen" />
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
