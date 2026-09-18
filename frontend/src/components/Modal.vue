<script setup>
// 模态框:遮罩点击 / Esc 关闭
// 注意:此处刻意**不使用 <Transition>** —— 过渡的卸载依赖 transitionend 回调,
// 在后台标签页(rAF 被降频)或合成层异常时会丢失回调,表现为"弹窗点了取消/创建成功
// 后不关闭、遮罩长期驻留"。改为直接条件渲染 + CSS 动画做入场,卸载即时可靠。
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
    <div v-if="show" class="modal-mask" @click.self="emit('close')">
      <div class="modal">
        <h3 v-if="title" class="modal-title">{{ title }}</h3>
        <slot />
        <div v-if="$slots.foot" class="modal-foot">
          <slot name="foot" />
        </div>
      </div>
    </div>
  </Teleport>
</template>
