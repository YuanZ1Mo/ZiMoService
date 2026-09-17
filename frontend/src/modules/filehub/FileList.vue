<script setup>
// 文件列表(列表/网格双视图)
// 虚拟滚动:定高行 57px + 窗口裁剪(>300 行场景,§7.6 硬约束);目录恒在文件前(服务端排序保证)
// 交互:单击选中 / 双击进目录或下载 / 行尾 ⋯ 菜单 / 行拖拽到文件夹移动 / 系统文件拖入上传
import { ref, computed, watch, onMounted, onBeforeUnmount } from 'vue'
import FileIcon from './FileIcon.vue'
import { fmtSize, fmtTime, kindLabel, highlightText } from '../../api/filehub'
import { collectDropItems } from '../../api/drop-entries'

const props = defineProps({
  items: { type: Array, default: () => [] },
  selected: { type: Set, required: true },
  view: { type: String, default: 'list' },          // list | grid
  keyword: { type: String, default: '' },           // 搜索高亮
  showPath: { type: Boolean, default: false },      // 搜索结果:显示相对路径
  sort: { type: String, default: 'name' },
  order: { type: String, default: 'asc' },
  loading: { type: Boolean, default: false },
  hasMore: { type: Boolean, default: false },
  /// 整表替换标记:调用方每次替换 items(刷新/换目录/搜索)自增,列表据此复位滚动
  epoch: { type: Number, default: 0 }
})
const emit = defineEmits(['toggle', 'open', 'ctx', 'drag-to', 'files', 'sort', 'load-more'])

// 可排序列表头:键必须落在服务端支持的范围(name|size|mtime|type,见 ListOrderClause)
// "创建者"列不参与排序(服务端无该排序键),故单独成列、不写在本表内
const SORTS = [['name', '名称'], ['type', '类型'], ['size', '大小'], ['mtime', '修改时间']]

// ── 虚拟滚动(定高窗口裁剪,约 40 行核心) ──
const ROW_H = 57
const scroller = ref(null)
const scrollTop = ref(0)
const viewH = ref(600)
function onScroll() {
  const el = scroller.value
  if (!el) return
  scrollTop.value = el.scrollTop
  // 滚动到底自动加载下一页(§3.1 无限滚动)
  if (el.scrollTop + el.clientHeight >= el.scrollHeight - 60 && props.hasMore && !props.loading)
    emit('load-more')
}
/** 复位滚动:整表被替换后旧位置可能越过新列表长度,视口会整片空白 */
function resetScroll() {
  scrollTop.value = 0
  if (scroller.value) scroller.value.scrollTop = 0
}
watch(() => props.view, resetScroll)
watch(() => props.epoch, resetScroll)
const vStart = computed(() => props.view !== 'list' ? 0 : Math.max(0, Math.floor(scrollTop.value / ROW_H) - 5))
const vEnd = computed(() => props.view !== 'list' ? props.items.length : Math.min(props.items.length, vStart.value + Math.ceil(viewH.value / ROW_H) + 10))
const padTop = computed(() => props.view !== 'list' ? 0 : vStart.value * ROW_H)
const padBottom = computed(() => props.view !== 'list' ? 0 : (props.items.length - vEnd.value) * ROW_H)
const vItems = computed(() => props.items.slice(vStart.value, vEnd.value))
function measure(el) {
  if (el) viewH.value = el.clientHeight || 600
}
onMounted(() => {
  measure(scroller.value)
  window.addEventListener('resize', onResize)
})
function onResize() { measure(scroller.value) }
onBeforeUnmount(() => window.removeEventListener('resize', onResize))

// ── 拖拽(内部:拖条目 → 悬停文件夹行移动) ──
const dragOverId = ref(0)
function onDragStart(e, node) {
  // 带上当前选中集:拖选中项之一 = 移动整批
  const ids = props.selected.size && props.selected.has(node.id) ? [...props.selected] : [node.id]
  e.dataTransfer.setData('text/zimo-node-ids', JSON.stringify(ids))
  e.dataTransfer.effectAllowed = 'move'
}
function onDragOverRow(e, node) {
  if (Number(node.type) !== 1) return
  e.preventDefault()
  dragOverId.value = node.id
}
function onDragLeaveRow(node) { if (dragOverId.value === node.id) dragOverId.value = 0 }
async function onDropRow(e, node) {
  dragOverId.value = 0
  if (Number(node.type) !== 1) return
  e.stopPropagation()   // 行内处理,避免冒泡到容器再触发"上传到当前目录"
  const raw = e.dataTransfer.getData('text/zimo-node-ids')
  if (raw) { emit('drag-to', { dirId: node.id, ids: JSON.parse(raw) }); return }
  // 系统文件/文件夹拖到文件夹行:上传到该文件夹(文件夹保留层级)
  const drop = await collectDropItems(e.dataTransfer)
  if (drop.files.length || drop.dirs.length) emit('files', { ...drop, dirId: node.id })
}
// 系统文件拖入空白区(上传到当前目录)
async function onDropFiles(e) {
  dragOverId.value = 0
  const drop = await collectDropItems(e.dataTransfer)
  if (drop.files.length || drop.dirs.length) emit('files', { ...drop, dirId: 0 })
}

</script>

<template>
  <div class="ftbl-wrap" style="position:relative;border-radius:var(--r-lg);overflow:hidden;border:none"
       @dragover.prevent @drop.prevent="onDropFiles">
    <!-- 列表视图 -->
    <div v-show="view === 'list'" ref="scroller" class="fh-vscroll" @scroll.passive="onScroll">
      <div :style="{ height: padTop + 'px' }"></div>
      <table class="ftbl">
        <thead>
          <tr>
            <th class="col-cb"></th>
            <th v-for="[k, label] in SORTS" :key="k" class="sortable" :class="{ sorted: sort === k }"
                :style="k === 'name' ? 'min-width:200px' : ''" @click="emit('sort', k)">
              {{ label }}{{ sort === k ? (order === 'asc' ? ' ↑' : ' ↓') : '' }}
            </th>
            <th style="min-width:70px">创建者</th>
            <th v-if="showPath" style="min-width:180px">位置</th>
            <th style="width:60px"></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="n in vItems" :key="n.id"
              :class="{ sel: selected.has(n.id), 'drop-to': dragOverId === n.id }"
              draggable="true"
              @click="emit('toggle', n)"
              @dblclick="emit('open', n)"
              @contextmenu.prevent="emit('ctx', $event, n)"
              @dragstart="onDragStart($event, n)"
              @dragover="onDragOverRow($event, n)"
              @dragleave="onDragLeaveRow(n)"
              @drop.prevent="onDropRow($event, n)">
            <td class="col-cb" @click.stop>
              <input type="checkbox" class="fcheck" :checked="selected.has(n.id)" @change="emit('toggle', n)" :aria-label="`选择 ${n.name}`" />
            </td>
            <td>
              <div class="fcell">
                <FileIcon :node="n" ext />
                <span class="fname">
                  <template v-for="(seg, i) in highlightText(n.name, keyword)" :key="i">
                    <mark v-if="seg.hit">{{ seg.t }}</mark><template v-else>{{ seg.t }}</template>
                  </template>
                </span>
              </div>
            </td>
            <td><span class="ftype">{{ kindLabel(n) }}</span></td>
            <td style="min-width:80px">
              <span class="num" :class="{ 'items-num': Number(n.type) === 1 }">
                {{ Number(n.type) === 1 ? `${n.items} 项` : fmtSize(n.size) }}
              </span>
            </td>
            <td style="min-width:110px"><span class="num">{{ fmtTime(n.update_time) }}</span></td>
            <td style="min-width:70px">{{ n.owner_name || '—' }}</td>
            <td v-if="showPath"><span style="font-size:var(--fs-cap);color:var(--color-text-2)">{{ n.path || '(空间根)' }}</span></td>
            <td>
              <span class="row-ops">
                <button class="dots-btn" type="button" aria-label="更多操作" @click.stop="emit('ctx', $event, n)">
                  <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="5" r="1.7"/><circle cx="12" cy="12" r="1.7"/><circle cx="12" cy="19" r="1.7"/></svg>
                </button>
              </span>
            </td>
          </tr>
        </tbody>
      </table>
      <div :style="{ height: padBottom + 'px' }"></div>
      <div v-if="loading && !items.length" style="padding:16px">
        <div v-for="i in 6" :key="i" class="skeleton" style="height:20px;margin-bottom:12px"></div>
      </div>
      <div v-if="hasMore && loading" style="padding:12px;text-align:center;color:var(--color-text-3);font-size:var(--fs-cap)">加载中…</div>
    </div>

    <!-- 网格视图 -->
    <div v-if="view === 'grid'" class="fgrid">
      <div v-for="n in items" :key="n.id" class="gcell" :class="{ sel: selected.has(n.id) }" draggable="true"
           @click="emit('toggle', n)" @dblclick="emit('open', n)" @contextmenu.prevent="emit('ctx', $event, n)"
           @dragstart="onDragStart($event, n)"
           @dragover="onDragOverRow($event, n)" @dragleave="onDragLeaveRow(n)" @drop.prevent="onDropRow($event, n)">
        <FileIcon :node="n" />
        <span class="gname">{{ n.name }}</span>
        <span class="gsub">{{ Number(n.type) === 1 ? `${n.items} 项` : fmtSize(n.size) }}</span>
      </div>
    </div>

    <!-- 空态 -->
    <div v-if="!loading && !items.length" class="empty">
      <div class="empty-icon">📁</div>
      <div class="empty-title">此目录为空</div>
      <div class="empty-sub">把文件拖到这里,或从上方"上传"开始</div>
    </div>
  </div>
</template>

<style scoped>
.fh-vscroll{overflow-y:auto;max-height:calc(100vh - 330px);min-height:240px}
.fcheck{width:16px;height:16px;accent-color:var(--color-primary);cursor:pointer}
@media (max-width:768px){.fh-vscroll{max-height:60vh}}
</style>
