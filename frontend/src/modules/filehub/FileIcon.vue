<script setup>
// 文件类型图标:类型底色 + 线性 SVG(kind 由 api/filehub.js kindOf 归类,服务端只回 ext)
import { computed } from 'vue'

const props = defineProps({
  node: { type: Object, required: true },   // 条目对象 {type, ext, name}
  ext: { type: Boolean, default: false }     // 是否显示扩展名角标
})

const kind = computed(() => {
  if (Number(props.node.type) === 1) return 'folder'
  const e = (props.node.ext || props.node.name || '').split('.').pop().toLowerCase()
  if (['doc', 'docx', 'pdf', 'xls', 'xlsx', 'ppt', 'pptx', 'md', 'txt'].includes(e)) return 'doc'
  if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'svg', 'bmp'].includes(e)) return 'img'
  if (['mp4', 'mov', 'mkv', 'avi', 'webm'].includes(e)) return 'vid'
  if (['mp3', 'flac', 'wav', 'ogg', 'm4a'].includes(e)) return 'aud'
  if (['zip', '7z', 'rar', 'gz', 'tar'].includes(e)) return 'zip'
  return 'oth'
})
const extLabel = computed(() => {
  if (!props.ext || Number(props.node.type) !== 1) return ''
  const e = (props.node.ext || '').toUpperCase()
  return e.length <= 4 ? e : ''
})
</script>

<template>
  <span class="ficon" :class="`f-${kind}`" aria-hidden="true">
    <!-- 文件夹 -->
    <svg v-if="kind === 'folder'" viewBox="0 0 24 24" fill="currentColor"><path d="M4 6a2 2 0 0 1 2-2h4.6a2 2 0 0 1 1.4.6L13.4 6H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6Z"/></svg>
    <!-- 文档 -->
    <svg v-else-if="kind === 'doc'" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5M9 13h6M9 17h4"/></svg>
    <!-- 图片 -->
    <svg v-else-if="kind === 'img'" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><rect x="4" y="5" width="16" height="14" rx="2"/><circle cx="9" cy="10" r="1.6" fill="currentColor" stroke="none"/><path d="m5 18 5.2-5.2a1.4 1.4 0 0 1 2 0L19 19"/></svg>
    <!-- 视频 -->
    <svg v-else-if="kind === 'vid'" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="m10 9.5 5 2.5-5 2.5Z" fill="currentColor" stroke="none"/></svg>
    <!-- 音频 -->
    <svg v-else-if="kind === 'aud'" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M9 18V6l8-2v12"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="14.5" cy="16" r="2.5"/></svg>
    <!-- 压缩包 -->
    <svg v-else-if="kind === 'zip'" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/><path d="M12 11v2m0 2v1" stroke-dasharray="2 2.6"/></svg>
    <!-- 其他 -->
    <svg v-else viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="m8 9-3 3 3 3M16 9l3 3-3 3"/></svg>
    <span v-if="extLabel" class="fext">{{ extLabel }}</span>
  </span>
</template>
