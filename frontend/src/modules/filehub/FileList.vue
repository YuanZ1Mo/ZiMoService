<script setup>
// 文件列表(列表/网格双视图)
// 虚拟滚动:窗口裁剪(行数超 300 才启用,行高按实际渲染量取);目录恒在文件前(服务端排序保证)
// 交互:单击选中 / 双击进目录或下载 / 行尾 ⋯ 菜单 / 行拖拽到文件夹移动 / 系统文件拖入上传
import { ref, computed, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
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
  epoch: { type: Number, default: 0 },
  dirId: { type: Number, default: 0 },   /// 当前目录 id(拖入上传要明确目标,不再靠假值兜底)
  space: { type: Number, default: 0 }    /// 当前空间(0=公共;个人空间不展示"创建者"列)
})
const emit = defineEmits(['toggle', 'open', 'ctx', 'drag-to', 'files', 'sort', 'load-more', 'toggle-all'])

// 可排序列表头:键必须落在服务端支持的范围(name|size|mtime|type,见 ListOrderClause)
// "创建者"列不参与排序(服务端无该排序键),故单独成列、不写在本表内
const SORTS = [['name', '名称'], ['type', '类型'], ['size', '大小'], ['mtime', '修改时间']]

// ── 虚拟滚动(定高窗口裁剪,约 40 行核心) ──
// 行高:先用兜底值,挂载/数据变化后按实际渲染的行量一次
// (此前写死 57,而 CSS 实际约 55 —— 长列表会累积出可观的偏移)
const ROW_H_FALLBACK = 55
const rowH = ref(ROW_H_FALLBACK)
const VIRTUAL_MIN = 300      // 超过这个行数才启用窗口裁剪(§7.6)
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
/// 是否走窗口裁剪:仅列表视图、且行数超过阈值
const virtualOn = computed(() => props.view === 'list' && props.items.length > VIRTUAL_MIN)
const vStart = computed(() => !virtualOn.value ? 0 : Math.max(0, Math.floor(scrollTop.value / rowH.value) - 5))
const vEnd = computed(() => !virtualOn.value ? props.items.length
                                             : Math.min(props.items.length,
                                                        vStart.value + Math.ceil(viewH.value / rowH.value) + 10))
const padTop = computed(() => !virtualOn.value ? 0 : vStart.value * rowH.value)
const padBottom = computed(() => !virtualOn.value ? 0 : (props.items.length - vEnd.value) * rowH.value)
const vItems = computed(() => virtualOn.value ? props.items.slice(vStart.value, vEnd.value) : props.items)
// 表头全选态:全部已加载条目都在选中集里=全选;部分选中=半选(indeterminate)
const allChecked = computed(() => props.items.length > 0 && props.items.every(n => props.selected.has(n.id)))
const someChecked = computed(() => !allChecked.value && props.items.some(n => props.selected.has(n.id)))
/**
 * 量一次可视高度与真实行高
 *
 * 行高按首行实测(CSS 改字号/图标尺寸时不必同步改常量),量不到就沿用兜底值。
 *
 * @param el  滚动容器
 */
function measure(el) {
  if (el) viewH.value = el.clientHeight || 600
  const row = scroller.value && scroller.value.querySelector('tbody tr')
  const h = row && row.getBoundingClientRect().height
  if (h) rowH.value = h
}
onMounted(() => {
  measure(scroller.value)
  window.addEventListener('resize', onResize)
})
function onResize() { measure(scroller.value) }
// 首帧可能还没有行(空目录/加载中):数据到了再量
watch(() => props.items, () => nextTick(() => measure(scroller.value)))
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
  // 显式传当前目录 id(此前传 0 靠调用方的假值兜底才落到当前目录,是隐性约定)
  if (drop.files.length || drop.dirs.length) emit('files', { ...drop, dirId: props.dirId })
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
            <th class="col-cb">
              <!-- 全选:勾选=选中当前已加载的全部条目;部分选中时显示半选态 -->
              <input type="checkbox" class="fcheck" :checked="allChecked" :indeterminate="someChecked"
                     :aria-label="allChecked ? '取消全选' : '全选'" @change="emit('toggle-all', $event.target.checked)" />
            </th>
            <th v-for="[k, label] in SORTS" :key="k" class="sortable" :class="{ sorted: sort === k }"
                :style="k === 'name' ? 'min-width:200px' : ''" @click="emit('sort', k)">
              {{ label }}{{ sort === k ? (order === 'asc' ? ' ↑' : ' ↓') : '' }}
            </th>
            <th v-if="Number(space) === 0" style="min-width:70px">创建者</th>
            <th v-if="showPath" style="min-width:180px">位置</th>
            <th style="width:60px"></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="n in vItems" :key="n.id"
              :class="{ sel: selected.has(n.id), 'drop-to': dragOverId === n.id }"
              draggable="true"
              @click="emit('toggle', n, $event)"
              @dblclick="emit('open', n)"
              @contextmenu.prevent="emit('ctx', $event, n)"
              @dragstart="onDragStart($event, n)"
              @dragover="onDragOverRow($event, n)"
              @dragleave="onDragLeaveRow(n)"
              @drop.prevent="onDropRow($event, n)">
            <td class="col-cb" @click.stop>
              <!-- 复选框是"纯开关":点它就增删自己,不清空别的(Tab 键切换不受影响) -->
              <input type="checkbox" class="fcheck" :checked="selected.has(n.id)"
                     @click.stop="emit('toggle', n, $event, true)" :aria-label="`选择 ${n.name}`" />
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
            <td v-if="Number(space) === 0" style="min-width:70px">{{ n.owner_name || '—' }}</td>
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
           @click="emit('toggle', n, $event)" @dblclick="emit('open', n)" @contextmenu.prevent="emit('ctx', $event, n)"
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
