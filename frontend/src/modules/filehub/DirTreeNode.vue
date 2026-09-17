<script setup>
// 目录树节点:自引用递归(模板里写 <DirTreeNode> 即引用本组件),故层级不限
// 展开态与子节点缓存在 node 上:同一节点重复展开不再请求(懒加载)
import FileIcon from './FileIcon.vue'

const props = defineProps({
  node: { type: Object, required: true },        // { id, name, open, loading, loaded, kids }
  selId: { type: Number, default: 0 },           // 当前选中目录 id(高亮用)
  loadKids: { type: Function, required: true }   // 展开时拉取子目录(空间上下文由调用方持有)
})
const emit = defineEmits(['pick'])
</script>

<template>
  <button type="button" class="tree-item" :class="{ sel: selId === node.id }"
          @click="loadKids(node); emit('pick', node)">
    <svg class="tw" :class="{ exp: node.open }" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="m9 6 6 6-6 6"/></svg>
    <FileIcon :node="{ type: 1, ext: '' }" />
    {{ node.name }}
  </button>
  <div v-if="node.open" class="tree-kids">
    <DirTreeNode v-for="k in node.kids" :key="k.id" :node="k" :sel-id="selId"
                 :load-kids="loadKids" @pick="emit('pick', $event)" />
    <div v-if="node.loading" style="padding:8px 12px">
      <span class="skeleton" style="display:block;height:16px"></span>
    </div>
  </div>
</template>
