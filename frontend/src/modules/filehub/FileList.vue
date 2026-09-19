<script setup>
// 文件列表(列表/网格双视图)
// 虚拟滚动:窗口裁剪(行数超 300 才启用,行高按实际渲染量取);目录恒在文件前(服务端排序保证)
// 交互:单击选中 / 双击进目录或下载 / 行尾 ⋯ 菜单 / 行拖拽到文件夹移动 / 系统文件拖入上传
import { ref, computed, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
import FileIcon from './FileIcon.vue'
import { fmtNodeSize, fmtTime, kindLabel, highlightText } from '../../api/filehub'
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
  space: { type: Number, default: 0 },   /// 当前空间(0=公共;个人空间不展示"创建者"列)
  /// space = 空间文件区;trash = 回收站;share = 分享页(免登录只读)
  variant: { type: String, default: 'space' },
  retainDays: { type: Number, default: 30 }      /// 回收站保留天数("剩余 N 天"按它推算)
})
const emit = defineEmits(['toggle', 'open', 'ctx', 'drag-to', 'files', 'sort', 'load-more', 'toggle-all'])

// 可排序列表头:键必须落在服务端支持的范围(name|size|mtime|type,见 ListOrderClause)
// "创建者"列不参与排序(服务端无该排序键),故单独成列、不写在本表内
const SORTS = [['name', '名称'], ['type', '类型'], ['size', '大小'], ['mtime', '修改时间']]
// 各列显式宽度:表格是 table-layout:auto,列宽由整列内容算出来 —— 空数据时只剩表头,
// 没给宽度的列会缩到表头文字宽,于是"有数据/无数据"两种状态下列宽对不上。全给上就一致了
const SORT_W = { name: '220px', type: '90px', size: '130px', mtime: '130px', delete_time: '130px' }
// 回收站变体:列集与手势都不同 ——
//   列:修改时间换删除时间,多"剩余"与常驻"原位置"(空间里"位置"只在搜索时出现)
//   手势:不接拖拽(回收站内不能移动、不能拖入上传),双击不触发动作(文件夹不下钻、文件不下载)
const isTrash = computed(() => props.variant === 'trash')
const isShare = computed(() => props.variant === 'share')
// 只读视图(回收站 / 分享页)不接拖拽:既不能把条目拖进文件夹,也不能把系统文件拖进来上传
const noDrag = computed(() => isTrash.value || isShare.value)
// 回收站没有"修改时间"的语义,服务端按删除时间排;两边的键都必须落在服务端支持的范围内
const SORTS_TRASH = [['name', '名称'], ['type', '类型'], ['size', '大小'], ['delete_time', '删除时间']]
const sorts = computed(() => (isTrash.value ? SORTS_TRASH : SORTS))

/// 剩余保留天数(按删除时间推算;已过保留期显示 0)
function remainDays(n) {
  return Math.max(0, props.retainDays - Math.floor((Date.now() / 1000 - (n.delete_time || 0)) / 86400))
}
/// 原位置三态文案:有路径原样显示;空且 origin_parent_id=0 = 本来就删在空间根;
/// 空且非 0 = 原目录已删除。后两态必须分开 —— 前者就在根下,后者是原目录没了
function pathText(n) {
  if (n.path) return n.path
  return Number(n.origin_parent_id) ? '原目录已删除' : '空间根'
}
/// 原位置是否需要警示(原目录已删除:恢复会落到空间根并自动重命名)
function pathWarn(n) {
  return !n.path && !!Number(n.origin_parent_id)
}
function pathTip(n) {
  if (n.path) return n.path
  return Number(n.origin_parent_id) ? '原目录已删除,恢复时将落到空间根并自动重命名'
                                    : '删除时就在空间根'
}
const emptyTitle = computed(() => props.keyword ? '没有匹配的条目'
                                                : (isTrash.value ? '回收站为空' : '此目录为空'))
const emptySub = computed(() => {
  if (props.keyword) return '换个关键词试试'
  if (isTrash.value) return '你在此空间删除的条目会出现在这里'
  // 分享页没有上传入口,提示"拖到这里上传"会指向一个不存在的动作
  if (isShare.value) return '分享者在这个目录下还没有放东西'
  return '把文件拖到这里,或从上方"上传"开始'
})
/// 双击:空间里是"进目录 / 下载",回收站里不触发任何动作
function onOpenRow(n) {
  if (isTrash.value) return
  emit('open', n)
}

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

// ── 右键菜单触发源(交给父组件决定作用对象) ──
//   row  = 右键某一行:行在选中集内则操作整批,否则收敛为它
//   dots = 行尾 ⋯:只操作该行(单选语义)
//   area = 右键列表空白区:操作当前选中集;没有选中项时父组件不弹菜单
function onAreaCtx(e) { emit('ctx', e, null, 'area') }

// ── 拖拽(内部:拖条目 → 悬停文件夹行移动) ──
const dragOverId = ref(0)
function onDragStart(e, node) {
  if (noDrag.value) return   // 只读视图里条目不能移动
  // 带上当前选中集:拖选中项之一 = 移动整批
  const ids = props.selected.size && props.selected.has(node.id) ? [...props.selected] : [node.id]
  e.dataTransfer.setData('text/zimo-node-ids', JSON.stringify(ids))
  e.dataTransfer.effectAllowed = 'move'
}
function onDragOverRow(e, node) {
  if (noDrag.value) return   // 只读视图不是拖动目标
  if (Number(node.type) !== 1) return
  e.preventDefault()
  dragOverId.value = node.id
}
function onDragLeaveRow(node) { if (dragOverId.value === node.id) dragOverId.value = 0 }
async function onDropRow(e, node) {
  dragOverId.value = 0
  if (noDrag.value) return
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
  if (noDrag.value) return   // 只读视图不能拖入上传
  const drop = await collectDropItems(e.dataTransfer)
  // 显式传当前目录 id(此前传 0 靠调用方的假值兜底才落到当前目录,是隐性约定)
  if (drop.files.length || drop.dirs.length) emit('files', { ...drop, dirId: props.dirId })
}

</script>

<template>
  <div class="ftbl-wrap" style="position:relative;border-radius:var(--r-lg);overflow:hidden;border:none"
       @dragover.prevent @drop.prevent="onDropFiles" @contextmenu.prevent="onAreaCtx">
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
            <th v-for="[k, label] in sorts" :key="k" class="sortable" :class="{ sorted: sort === k }"
                :style="{ width: SORT_W[k] }" @click="emit('sort', k)">
              {{ label }}{{ sort === k ? (order === 'asc' ? ' ↑' : ' ↓') : '' }}
            </th>
            <!-- 创建者只在空间文件区给(分享页是免登录的,不该暴露文件是谁上传的) -->
            <th v-if="!isTrash && !isShare && Number(space) === 0" style="width:90px">创建者</th>
            <th v-if="isTrash" style="width:96px">剩余</th>
            <th v-if="isTrash || showPath" style="width:190px">{{ isTrash ? '原位置' : '位置' }}</th>
            <th style="width:60px"></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="n in vItems" :key="n.id"
              :class="{ sel: selected.has(n.id), 'drop-to': dragOverId === n.id }"
              :draggable="!noDrag"
              @click="emit('toggle', n, $event)"
              @dblclick="onOpenRow(n)"
              @contextmenu.prevent.stop="emit('ctx', $event, n, 'row')"
              @dragstart="onDragStart($event, n)"
              @dragover="onDragOverRow($event, n)"
              @dragleave="onDragLeaveRow(n)"
              @drop.prevent="onDropRow($event, n)">
            <!-- dblclick.stop:复选框双击不能冒泡到行,否则会被当成双击"打开"(点复选框进目录) -->
            <td class="col-cb" @click.stop @dblclick.stop>
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
            <td style="min-width:110px">
              <span class="num" :class="{ 'items-num': Number(n.type) === 1 }">
                {{ fmtNodeSize(n) }}
              </span>
            </td>
            <!-- 第 4 列随变体换字段:空间=修改时间,回收站=删除时间
                 (回收站接口不下发 update_time,写死会整列显示成"—") -->
            <td style="min-width:110px"><span class="num">{{ fmtTime(isTrash ? n.delete_time : n.update_time) }}</span></td>
            <td v-if="!isTrash && !isShare && Number(space) === 0" style="min-width:70px">{{ n.owner_name || '—' }}</td>
            <td v-if="isTrash"><span class="badge badge-warn num">剩 {{ remainDays(n) }} 天</span></td>
            <!-- 原位置三态:直接套空间的 `path || '(空间根)'` 会把"原目录已删除"错报成
                 "空间根" —— 两者恢复的去处不同,不能合并 -->
            <td v-if="isTrash" style="min-width:180px">
              <span class="cellpath" :class="{ warn: pathWarn(n) }" :title="pathTip(n)">{{ pathText(n) }}</span>
            </td>
            <td v-else-if="showPath" style="min-width:180px">
              <span class="cellpath" :title="n.path || '(空间根)'">{{ n.path || '(空间根)' }}</span>
            </td>
            <td>
              <span class="row-ops">
                <button class="dots-btn" type="button" aria-label="更多操作" @click.stop="emit('ctx', $event, n, 'dots')">
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
      <div v-for="n in items" :key="n.id" class="gcell" :class="{ sel: selected.has(n.id) }" :draggable="!noDrag"
           @click="emit('toggle', n, $event)" @dblclick="onOpenRow(n)" @contextmenu.prevent.stop="emit('ctx', $event, n, 'row')"
           @dragstart="onDragStart($event, n)"
           @dragover="onDragOverRow($event, n)" @dragleave="onDragLeaveRow(n)" @drop.prevent="onDropRow($event, n)">
        <FileIcon :node="n" />
        <span class="gname">{{ n.name }}</span>
        <span class="gsub">{{ fmtNodeSize(n) }}<template v-if="isTrash"> · 剩 {{ remainDays(n) }} 天</template></span>
      </div>
    </div>

    <!-- 空态 -->
    <div v-if="!loading && !items.length" class="empty">
      <div class="empty-icon">{{ isTrash ? '🗑' : '📁' }}</div>
      <div class="empty-title">{{ emptyTitle }}</div>
      <div class="empty-sub">{{ emptySub }}</div>
    </div>
  </div>
</template>

<style scoped>
/* 列表根:吃掉卡片剩余高度,列表/网格在内部滚动。
   高度不再由 calc(100vh - 常数) 猜(猜大会让模块区多出一条外部滚动条) */
.ftbl-wrap{flex:1;min-height:0;display:flex;flex-direction:column}
/* 空态覆盖在列表区上:列表为空时它不占位,否则 flex 会把提示挤到卡片底部 */
.ftbl-wrap > .empty{position:absolute;inset:0;justify-content:center}
.fh-vscroll{flex:1;min-height:120px;overflow-y:auto}
/* 路径列:定宽截断 + 悬停看全名。不截断的话深层路径会把表格撑宽,把后面几列挤出视口 */
.cellpath{display:inline-block;max-width:180px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
          vertical-align:bottom;font-size:var(--fs-cap);color:var(--color-text-2)}
/* 原目录已删除:恢复会落到空间根并自动重命名,与"本来就删在空间根"不是一回事 */
.cellpath.warn{color:var(--color-warn)}
.fcheck{width:16px;height:16px;accent-color:var(--color-primary);cursor:pointer}
</style>
