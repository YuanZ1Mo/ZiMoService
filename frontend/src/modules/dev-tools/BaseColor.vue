<script setup>
// 进制与颜色(二期 A):2/8/10/16 进制互转(BigInt,大整数不丢精度)+ HEX/RGB/HSL 互转
import { ref, computed, inject, watch } from 'vue'
import ToolShell from './ToolShell.vue'
import { copyText } from './toolbox'

const toast = inject('toast')

const MODE = [
  { key: 'radix', name: '进制转换' },
  { key: 'color', name: '颜色转换' }
]
const RADIX = [
  { key: 2, name: '2 进制' },
  { key: 8, name: '8 进制' },
  { key: 10, name: '10 进制' },
  { key: 16, name: '16 进制' }
]
const mode = ref('radix')
const radixIn = ref(10)
// 两种模式各留一份输入:共用一个输入框会在切换时串味("255" 在颜色模式下会被当作 #255 解析)
const radixInput = ref('')
const colorInput = ref('')
const input = computed({
  get: () => (mode.value === 'radix' ? radixInput.value : colorInput.value),
  set: (v) => {
    if (mode.value === 'radix')
      radixInput.value = v
    else
      colorInput.value = v
  }
})
const errMsg = ref('')
const result = ref(null)   // 进制:{2,8,10,16} / 颜色:{hex,rgb,hsl,alpha}
const outEl = ref(null)

// ── 进制(BigInt,支持任意大整数) ──
function parseRadix(text) {
  let t = text.trim()
  if (!t)
    return null
  let base = radixIn.value
  if (/^0[xX]/.test(t)) {
    base = 16
    t = t.slice(2)
  } else if (/^0[bB]/.test(t)) {
    base = 2
    t = t.slice(2)
  } else if (/^0[oO]/.test(t)) {
    base = 8
    t = t.slice(2)
  }
  const neg = t.startsWith('-')
  if (neg)
    t = t.slice(1)
  const digits = '0123456789abcdefghijklmnopqrstuvwxyz'.slice(0, base)
  const low = t.toLowerCase()
  for (const ch of low) {
    if (!digits.includes(ch))
      throw new Error(`「${ch}」不是合法的 ${base} 进制数字`)
  }
  if (!low)
    return null
  return BigInt((neg ? '-' : '') + '0' + (base === 16 ? 'x' : base === 8 ? 'o' : base === 2 ? 'b' : '') + low)
}
function computeRadix() {
  const v = parseRadix(input.value)
  if (v === null) {
    result.value = null
    return
  }
  result.value = { 2: v.toString(2), 8: v.toString(8), 10: v.toString(10), 16: v.toString(16) }
}

// ── 颜色 ──
function hexToRgb(s) {
  const m = s.trim().replace(/^#/, '')
  if (/^[0-9a-fA-F]{3}$/.test(m))
    return [0, 1, 2].map(i => parseInt(m[i] + m[i], 16))
  if (/^[0-9a-fA-F]{6}$/.test(m))
    return [0, 2, 4].map(i => parseInt(m.slice(i, i + 2), 16))
  return null
}
function rgbToHsl(r, g, b) {
  const R = r / 255
  const G = g / 255
  const B = b / 255
  const max = Math.max(R, G, B)
  const min = Math.min(R, G, B)
  const l = (max + min) / 2
  const d = max - min
  let h = 0
  let s = 0
  if (d !== 0) {
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min)
    if (max === R)
      h = ((G - B) / d + (G < B ? 6 : 0)) / 6
    else if (max === G)
      h = ((B - R) / d + 2) / 6
    else
      h = ((R - G) / d + 4) / 6
  }
  return [Math.round(h * 360), Math.round(s * 100), Math.round(l * 100)]
}
function hslToRgb(h, s, l) {
  const S = s / 100
  const L = l / 100
  const c = (1 - Math.abs(2 * L - 1)) * S
  const hp = (((h % 360) + 360) % 360) / 60
  const x = c * (1 - Math.abs((hp % 2) - 1))
  const [r1, g1, b1] = hp < 1 ? [c, x, 0] : hp < 2 ? [x, c, 0] : hp < 3 ? [0, c, x]
    : hp < 4 ? [0, x, c] : hp < 5 ? [x, 0, c] : [c, 0, x]
  const m = L - c / 2
  return [r1, g1, b1].map(v => Math.round((v + m) * 255))
}
function parseColor(text) {
  const t = text.trim()
  if (!t)
    return null
  let rgb = null
  let alpha = 1
  if (t.startsWith('#'))
    rgb = hexToRgb(t)
  else if (/^rgba?\(/i.test(t)) {
    const nums = t.replace(/^rgba?\(/i, '').replace(/\)$/, '').split(/[,\s/]+/).filter(Boolean)
    if (nums.length < 3)
      throw new Error('rgb() 需要 3 个分量')
    rgb = nums.slice(0, 3).map(n => Math.round(Number(n)))
    if (nums.length > 3)
      alpha = Number(nums[3])
  } else if (/^hsla?\(/i.test(t)) {
    const nums = t.replace(/^hsla?\(/i, '').replace(/\)$/, '').split(/[,\s/%]+/).filter(Boolean)
    if (nums.length < 3)
      throw new Error('hsl() 需要 3 个分量')
    rgb = hslToRgb(Number(nums[0]), Number(nums[1]), Number(nums[2]))
    if (nums.length > 3)
      alpha = Number(nums[3])
  } else {
    rgb = hexToRgb(t)   // 允许省略 #
  }
  if (!rgb)
    throw new Error('无法识别的颜色,支持 #RGB / #RRGGBB / rgb() / hsl()')
  if (rgb.some(v => !Number.isFinite(v) || v < 0 || v > 255))
    throw new Error('RGB 分量需在 0~255 之间')
  if (!Number.isFinite(alpha) || alpha < 0 || alpha > 1)
    throw new Error('透明度需在 0~1 之间')
  const [r, g, b] = rgb
  const [h, s, l] = rgbToHsl(r, g, b)
  const hex = '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('')
  return {
    hex,
    rgb: alpha < 1 ? `rgba(${r}, ${g}, ${b}, ${alpha})` : `rgb(${r}, ${g}, ${b})`,
    hsl: alpha < 1 ? `hsla(${h}, ${s}%, ${l}%, ${alpha})` : `hsl(${h}, ${s}%, ${l}%)`,
    swatch: `rgba(${r}, ${g}, ${b}, ${alpha})`
  }
}
function computeColor() {
  const c = parseColor(input.value)
  result.value = c
}

function compute() {
  try {
    if (mode.value === 'radix')
      computeRadix()
    else
      computeColor()
    errMsg.value = ''
  } catch (e) {
    result.value = null
    errMsg.value = String((e && e.message) || e)
  }
}
watch([input, mode, radixIn], compute, { immediate: true })

/// 结果区文本(复制用):进制 4 行;颜色 3 行
const resultText = computed(() => {
  const r = result.value
  if (!r)
    return ''
  if (mode.value === 'radix')
    return [2, 8, 10, 16].map(b => `${b} 进制: ${r[b]}`).join('\n')
  return `HEX: ${r.hex}\nRGB: ${r.rgb}\nHSL: ${r.hsl}`
})
async function doCopy() {
  const r = await copyText(resultText.value, outEl.value)
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
      <div class="seg dt-seg" role="group" aria-label="转换类型">
        <button v-for="m in MODE" :key="m.key" type="button" class="seg-item"
                :class="{ active: mode === m.key }" :aria-pressed="mode === m.key"
                @click="mode = m.key">{{ m.name }}</button>
      </div>
      <template v-if="mode === 'radix'">
        <div class="seg dt-seg" role="group" aria-label="输入进制">
          <button v-for="r in RADIX" :key="r.key" type="button" class="seg-item"
                  :class="{ active: radixIn === r.key }" :aria-pressed="radixIn === r.key"
                  @click="radixIn = r.key">{{ r.name }}</button>
        </div>
      </template>
      <span class="sp"></span>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!resultText" @click="doCopy">复制结果</button>
    </template>

    <div class="dt-pane dt-pane-fixed">
      <div class="dt-pane-head">
        <span class="dt-pane-title">输入</span>
        <span class="dt-cap">{{ mode === 'radix' ? '支持 0x / 0b / 0o 前缀;大整数按 BigInt 精确处理' : '#RGB / #RRGGBB / rgb() / hsl()' }}</span>
      </div>
      <div style="padding:12px 14px">
        <input v-model="input" class="dt-in" spellcheck="false"
               :placeholder="mode === 'radix' ? '如 255 或 0xFF' : '如 #0284C7'" />
      </div>
    </div>

    <div class="dt-pane dt-pane-fill">
      <div class="dt-pane-head">
        <span class="dt-pane-title">结果</span>
      </div>
      <div class="dt-pane-body">
        <div v-if="mode === 'color' && result" class="dt-row" style="margin-bottom:12px">
          <span class="dt-swatch" :style="{ background: result.swatch }" role="img" :aria-label="'颜色预览 ' + result.hex"></span>
          <span class="dt-hint">色块预览(与主题无关)</span>
        </div>
        <dl v-if="result && mode === 'radix'" class="dt-kv">
          <template v-for="b in [2, 8, 10, 16]" :key="b">
            <dt>{{ b }} 进制</dt>
            <dd>{{ result[b] }}</dd>
          </template>
        </dl>
        <dl v-else-if="result" class="dt-kv">
          <dt>HEX</dt><dd>{{ result.hex }}</dd>
          <dt>RGB</dt><dd>{{ result.rgb }}</dd>
          <dt>HSL</dt><dd>{{ result.hsl }}</dd>
        </dl>
        <div v-else class="dt-hint">输入后即时转换</div>
      </div>
      <textarea ref="outEl" class="dt-ta" style="position:fixed;opacity:0;pointer-events:none;height:1px"
                readonly :value="resultText" tabindex="-1" aria-hidden="true"></textarea>
    </div>

    <template #status>
      <span class="dt-hint">{{ mode === 'radix' ? '十进制以外的结果不带前缀' : 'HSL 取整到度/百分比' }}</span>
    </template>
  </ToolShell>
</template>
