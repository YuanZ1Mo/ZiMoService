<script setup>
// 密码强度条(本地启发式评分,服务端复核为准)
// 规则:长度≥8 / 大小写 / 数字 / 符号 → 0-4 档;<8 位封顶 1 档
import { computed } from 'vue'

const props = defineProps({
  password: { type: String, default: '' }
})

const score = computed(() => {
  const v = props.password || ''
  if (!v) return 0
  let s = 0
  if (v.length >= 8) s++
  if (/[a-z]/.test(v) && /[A-Z]/.test(v)) s++
  if (/\d/.test(v)) s++
  if (/[^A-Za-z0-9]/.test(v)) s++
  if (v.length < 8) s = Math.min(s, 1)
  return s
})
const labels = ['输入密码后实时评估', '过弱 · 建议加长并混合字符', '一般 · 可再增强', '良好', '很强']
const colors = ['', 'var(--color-err)', 'var(--color-warn)', 'var(--color-ok)', 'var(--color-ok)']
</script>

<template>
  <div class="strength">
    <div class="strength-bars" :class="`s${score}`"><i></i><i></i><i></i><i></i></div>
    <span class="strength-label" :style="score ? `color:${colors[score]}` : ''">
      密码强度:{{ labels[score] }}
    </span>
  </div>
</template>
