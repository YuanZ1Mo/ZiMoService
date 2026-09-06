<script setup>
// 根组件:纯路由出口;全局 Toast 浮层(inject('toast'))
import { ref, provide } from 'vue'

const toasts = ref([])
let seq = 0
const icons = { ok: '✓', err: '✕', warn: '⚠', info: 'ℹ' }
function toast(message, type = 'info', duration = 2600) {
  const id = ++seq
  toasts.value.push({ id, message, type })
  setTimeout(() => {
    toasts.value = toasts.value.filter(t => t.id !== id)
  }, duration)
}
provide('toast', toast)
</script>

<template>
  <router-view />
  <div class="toast-stack" aria-live="polite">
    <TransitionGroup name="toast">
      <div v-for="t in toasts" :key="t.id" class="toast" :class="`toast-${t.type}`">
        <span aria-hidden="true">{{ icons[t.type] || icons.info }}</span>
        <span>{{ t.message }}</span>
      </div>
    </TransitionGroup>
  </div>
</template>
