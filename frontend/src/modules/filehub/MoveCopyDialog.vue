<script setup>
// 移动/复制 · 目录树选择器(可跨空间,§3.5/§3.6)
// 树懒加载:切空间拉根目录,点展开再拉子目录;冲突策略 skip/rename/overwrite 重试整批(ask=409 冲突清单)
import { ref, watch } from 'vue'
import Modal from '../../components/Modal.vue'
import DirTreeNode from './DirTreeNode.vue'
import { filehubApi, fmtSize } from '../../api/filehub'

const props = defineProps({
  show: { type: Boolean, default: false },
  mode: { type: String, default: 'move' },      // move | copy
  targets: { type: Array, default: () => [] },  // 待操作条目 [{id,name,type,size}]
  meSpace: { type: Number, default: 0 }         // 我的空间 space=uid(会话注入)
})
const emit = defineEmits(['close', 'done'])

const spaces = [{ space: 0, name: '公共空间' }, { space: 'me', name: '我的空间' }]
const spaceSel = ref(0)
const roots = ref([])                 // [{id,name,open,loading,loaded,kids}]
const destName = ref('')
const destId = ref(0)
const conflict = ref('ask')
const conflicts = ref([])             // ask 409 冲突清单
const busy = ref(false)

watch(() => props.show, async (v) => {
  if (!v) return
  conflict.value = 'ask'; conflicts.value = []
  await loadRoots()
})

async function loadRoots() {
  roots.value = []
  destId.value = 0; destName.value = '空间根'
  try {
    const d = await filehubApi.list({ space: realSpace(), dir_id: 0, page: 1, size: 1000 })
    roots.value = (d.list || []).filter(n => Number(n.type) === 1).map(toTree)
  } catch { /* 静默 */ }
}
// 我的空间 space=uid(由父组件经会话注入)
function realSpace() { return spaceSel.value === 'me' ? Number(props.meSpace) : Number(spaceSel.value) }
function toTree(n) {
  return { id: n.id, name: n.name, open: false, loading: false, loaded: false, kids: [] }
}

/**
 * 展开/收起一个节点;首次展开时懒加载其子目录
 *
 * 加载态挂在节点自身上(供递归组件显示骨架条),重复展开不会重复请求。
 *
 * @param t  树节点(见 toTree)
 */
async function loadKids(t) {
  t.open = !t.open
  if (t.open && !t.loaded && !t.loading) {
    t.loading = true
    try {
      const d = await filehubApi.list({ space: realSpace(), dir_id: t.id, page: 1, size: 1000 })
      t.kids = (d.list || []).filter(n => Number(n.type) === 1).map(toTree)
      t.loaded = true
    } catch { /* 静默 */ } finally { t.loading = false }
  }
}
function pick(t) { destId.value = t.id; destName.value = t.name }
function pickRoot() { destId.value = 0; destName.value = '空间根' }
watch(spaceSel, loadRoots)

async function confirm() {
  busy.value = true
  conflicts.value = []
  try {
    const body = { ids: props.targets.map(t => t.id), target_space: realSpace(), target_dir_id: destId.value, conflict: conflict.value }
    const fn = props.mode === 'move' ? filehubApi.move : filehubApi.copy
    const r = await fn(body)
    emit('done', r)
  } catch (e) {
    if (e.code === 'NAME_EXISTS' && e.data && e.data.conflicts) conflicts.value = e.data.conflicts
    else emit('done', { __error: e })
  } finally { busy.value = false }
}
const totalSize = () => props.targets.reduce((a, t) => a + (Number(t.type) === 2 ? Number(t.size) : 0), 0)
</script>

<template>
  <Modal :show="show" :title="`${mode === 'move' ? '移动' : '复制'} ${targets.length} 项到…`" @close="emit('close')">
    <div class="row" style="gap:8px;margin-bottom:12px;flex-wrap:wrap">
      <div class="seg" style="width:220px">
        <button v-for="s in spaces" :key="s.space" type="button" class="seg-item"
                :class="{ active: spaceSel === s.space }" @click="spaceSel = s.space">{{ s.name }}</button>
      </div>
      <span class="num" style="font-size:var(--fs-cap);color:var(--color-text-3)">
        共 {{ fmtSize(totalSize()) }}
      </span>
    </div>

    <div class="tree-box">
      <button type="button" class="tree-item" :class="{ sel: destId === 0 }" @click="pickRoot">
        <span class="ficon f-folder" style="width:24px;height:24px;border-radius:8px">
          <svg viewBox="0 0 24 24" fill="currentColor" style="width:14px;height:14px"><path d="M4 6a2 2 0 0 1 2-2h4.6a2 2 0 0 1 1.4.6L13.4 6H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6Z"/></svg>
        </span>
        {{ spaces.find(s => s.space === spaceSel).name }}(根)
      </button>
      <DirTreeNode v-for="t in roots" :key="t.id" :node="t" :sel-id="destId"
                   :load-kids="loadKids" @pick="pick" />
    </div>

    <div class="dest-bar" style="margin-top:12px">
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>
      目标:{{ spaces.find(s => s.space === spaceSel).name }} › {{ destName }}
    </div>

    <div v-if="conflicts.length" class="conflict-list" style="margin-top:12px">
      <div v-for="c in conflicts" :key="c.id" class="ci">
        <span class="cname">{{ c.name }}</span>
        <span class="cwhy">{{ c.why }}</span>
      </div>
      <div class="conflict-row" style="margin:10px 12px 12px">
        <label><input v-model="conflict" type="radio" value="skip" />跳过</label>
        <label><input v-model="conflict" type="radio" value="rename" />自动重命名</label>
        <label><input v-model="conflict" type="radio" value="overwrite" />覆盖文件</label>
      </div>
    </div>

    <template #foot>
      <button class="btn btn-ghost" type="button" @click="emit('close')">取消</button>
      <button class="btn btn-primary" type="button" :disabled="busy" @click="confirm">
        {{ mode === 'move' ? '移动到这里' : '复制到这里' }}
      </button>
    </template>
  </Modal>
</template>
