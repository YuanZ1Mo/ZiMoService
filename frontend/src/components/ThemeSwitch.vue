<script setup>
// 亮/暗主题切换:light → dark → system(跟随系统) 循环
// 初始:本地偏好优先;否则按系统偏好落 data-theme;system 档监听系统变化
import { ref, onMounted, onBeforeUnmount } from 'vue'

const theme = ref('system')
let mq = null
const STORE_KEY = 'zimo-theme'

function apply() {
  const root = document.documentElement
  if (theme.value === 'dark') root.dataset.theme = 'dark'
  else if (theme.value === 'light') root.dataset.theme = 'light'
  else {
    // system:跟随系统偏好
    const dark = mq ? mq.matches : window.matchMedia('(prefers-color-scheme: dark)').matches
    if (dark) root.dataset.theme = 'dark'
    else root.dataset.theme = 'light'
  }
}
function onSystemChange() {
  if (theme.value === 'system') apply()
}
function cycle() {
  theme.value = theme.value === 'light' ? 'dark' : theme.value === 'dark' ? 'system' : 'light'
  // 手动偏好持久化;跟随系统不落盘(保持随系统变化)
  try {
    if (theme.value === 'system') localStorage.removeItem(STORE_KEY)
    else localStorage.setItem(STORE_KEY, theme.value)
  } catch { /* 隐私模式等场景忽略 */ }
  apply()
}

onMounted(() => {
  mq = window.matchMedia('(prefers-color-scheme: dark)')
  if (mq.addEventListener) mq.addEventListener('change', onSystemChange)
  // 恢复上次手动选择;无记录则按系统偏好
  try {
    const saved = localStorage.getItem(STORE_KEY)
    if (saved === 'light' || saved === 'dark') theme.value = saved
  } catch { /* ignore */ }
  apply()
})
onBeforeUnmount(() => {
  if (mq && mq.removeEventListener) mq.removeEventListener('change', onSystemChange)
})
</script>

<template>
  <button class="theme-switch" type="button" :title="`主题:${theme === 'system' ? '跟随系统' : theme === 'dark' ? '暗色' : '亮色'}`"
          aria-label="切换主题" @click="cycle">
    <svg v-if="theme === 'light'" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
      <circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>
    </svg>
    <svg v-else-if="theme === 'dark'" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
      <path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8Z"/>
    </svg>
    <svg v-else width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
      <circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 0 0 18Z" fill="currentColor" stroke="none"/>
    </svg>
  </button>
</template>
