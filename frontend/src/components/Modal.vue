<script setup>
// 模态框:遮罩点击 / Esc 关闭
import { watch, onBeforeUnmount } from 'vue'

const props = defineProps({
  title: { type: String, default: '' },
  show: { type: Boolean, default: false }
})
const emit = defineEmits(['close'])

function onKeydown(e) {
  if (e.key === 'Escape') emit('close')
}
watch(() => props.show, (v) => {
  if (v) window.addEventListener('keydown', onKeydown)
  else window.removeEventListener('keydown', onKeydown)
})
onBeforeUnmount(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <Teleport to="body">
    <Transition name="toast">
      <div v-if="show" class="modal-mask" @click.self="emit('close')">
        <div class="modal">
          <h3 v-if="title" class="modal-title">{{ title }}</h3>
          <slot />
          <div v-if="$slots.foot" class="modal-foot">
            <slot name="foot" />
          </div>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>
