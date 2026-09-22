<script setup>
// 二期工具布局壳(设计 §10.2 基座):工具条 / 错误条 / 提示条 / 主体 / 状态行
// 一期 JsonFormat 也回填到本壳,避免基座与一期两套 UI 并存
// 错误条用 role="alert":校验类错误必须可被读屏即时播报,且与出错字段同处一屏(不是只弹 toast)
defineProps({
  /// 错误文案;非空时渲染错误条
  error: { type: String, default: '' }
})
</script>

<template>
  <div class="dt-tool">
    <div class="dt-bar">
      <slot name="bar" />
    </div>

    <div v-if="error" class="dt-err" role="alert">
      <span aria-hidden="true">✕</span>
      <span>{{ error }}</span>
    </div>

    <slot name="notice" />

    <div class="dt-tool-main">
      <slot />
    </div>

    <div class="dt-stats">
      <slot name="status" />
    </div>
  </div>
</template>
