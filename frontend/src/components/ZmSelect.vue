<script setup>
// 自绘下拉(替代原生 <select>)
//
// 原生 select 的**弹出列表由系统绘制**,CSS 完全够不着,箭头也不会随展开翻转 ——
// 这两点都改不了,除非自己画。这里换成"按钮 + 浮层列表":展开态、选项样式、箭头翻转都可控。
//
// 浮层必须传送到 body:弹窗容器是 overflow:auto,内联浮层会被它裁掉;
// 定位用 position:fixed + 触发按钮的矩形,贴到视口底部时改为向上弹。
import { ref, computed, onMounted, onBeforeUnmount, nextTick } from 'vue'

const props = defineProps({
  modelValue: { type: [String, Number, Boolean], default: '' },
  /// 选项:[{ value, label, disabled?, group? }];带 group 时按组渲染(组标题不可选,键盘自动跳过)
  options: { type: Array, required: true },
  disabled: { type: Boolean, default: false },
  placeholder: { type: String, default: '请选择' },
  /// 控件内的前缀文案(原样渲染,含分隔符),如 "状态:";空则不显示
  prefix: { type: String, default: '' }
})
const emit = defineEmits(['update:modelValue', 'change'])

const open   = ref(false)
const wrap   = ref(null)   // 触发按钮(浮层定位基准)
const pop    = ref(null)   // 浮层(挂在 body 上)
const pos    = ref({ left: 0, top: 0, width: 160 })
const active = ref(-1)     // 键盘高亮项的下标(始终指向 options,与分组无关)

const cur = computed(() => props.options.find(o => o.value === props.modelValue) || null)
/// 按 group 切段:相邻同组归为一组;无 group 的选项自成一段(不渲染组标题)
const groups = computed(() => {
  const out = []
  let last = null
  props.options.forEach((o, i) => {
    const name = o.group || ''
    if (!last || last.name !== name) {
      last = { name, items: [] }
      out.push(last)
    }
    last.items.push({ o, i })
  })
  return out
})
/// 浮层最大高度,须与下面 CSS 的 max-height 一致(定位时按它估算是否够放)
const POP_MAX_H = 280

/// 按触发按钮的位置摆放浮层;下方放不下就向上弹
function place() {
  // 收起时直接返回:否则页面上**任何**滚动都会走一次 getBoundingClientRect
  // (强制同步布局,打断渲染流水线),滚动明显发卡
  if (!open.value) return
  const el = wrap.value
  if (!el) return
  const r     = el.getBoundingClientRect()
  const below = r.bottom + 6
  pos.value = {
    left: r.left,
    width: Math.max(r.width, 140),
    top: below + POP_MAX_H > window.innerHeight ? Math.max(8, r.top - POP_MAX_H - 6) : below
  }
}
function close() { open.value = false }
function toggle() {
  if (props.disabled) return
  open.value = !open.value
  if (open.value) {
    active.value = props.options.findIndex(o => o.value === props.modelValue)
    nextTick(place)
  }
}
function pick(o) {
  if (o.disabled) return
  close()
  if (o.value === props.modelValue) return
  emit('update:modelValue', o.value)
  emit('change', o.value)
}
/**
 * 键盘操作:收起时空格/回车/下箭头展开;展开时上下移动高亮、回车选中、Esc 关闭
 *
 * @param e keydown 事件
 */
function onKey(e) {
  if (props.disabled) return
  if (!open.value) {
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'ArrowDown') {
      e.preventDefault()
      toggle()
    }
    return
  }
  if (e.key === 'Escape') {
    e.stopPropagation()   // 别让 Esc 冒泡去关掉外层弹窗
    close()
    return
  }
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault()
    const n   = props.options.length
    const dir = e.key === 'ArrowDown' ? 1 : -1
    let   i   = active.value
    for (let step = 0; step < n; ++step)   // 跳过禁用项
    {
      i = (i + dir + n) % n
      if (!props.options[i].disabled) break
    }
    active.value = i
    return
  }
  if (e.key === 'Enter' && active.value >= 0) {
    e.preventDefault()
    pick(props.options[active.value])
  }
}
function onDocDown(e) {
  if (!open.value) return
  const t = e.target
  // 浮层在 body 上,不在 wrap 里,两处都要判
  if ((wrap.value && wrap.value.contains(t)) || (pop.value && pop.value.contains(t))) return
  close()
}
onMounted(() => {
  document.addEventListener('mousedown', onDocDown, true)
  window.addEventListener('resize', place)
  window.addEventListener('scroll', place, true)   // 捕获:弹窗内部滚动时浮层也要跟随
})
onBeforeUnmount(() => {
  document.removeEventListener('mousedown', onDocDown, true)
  window.removeEventListener('resize', place)
  window.removeEventListener('scroll', place, true)
})
</script>

<template>
  <div ref="wrap" class="zsel" :class="{ open, disabled }" tabindex="0"
       role="combobox" :aria-expanded="open" @click="toggle" @keydown="onKey">
    <span v-if="prefix" class="zsel-prefix">{{ prefix }}</span>
    <span v-if="cur && cur.group" class="zsel-group">{{ cur.group }} ·</span>
    <span class="zsel-label">{{ cur ? cur.label : placeholder }}</span>
    <!-- 箭头随展开翻转:原生 select 做不到的就是这一点 -->
    <svg class="zsel-arrow" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         stroke-width="2.4" stroke-linecap="round"><path d="m6 9 6 6 6-6"/></svg>
  </div>
  <Teleport to="body">
    <div v-if="open" ref="pop" class="menu zsel-pop" role="listbox"
         :style="{ position: 'fixed', left: pos.left + 'px', top: pos.top + 'px', width: pos.width + 'px' }">
      <div v-for="g in groups" :key="g.name || '_'" :role="g.name ? 'group' : null"
           :aria-label="g.name || null">
        <div v-if="g.name" class="zsel-group-head" role="presentation">{{ g.name }}</div>
        <button v-for="it in g.items" :key="String(it.o.value)" type="button"
                class="menu-item zsel-opt" :class="{ on: it.i === active, picked: it.o.value === modelValue }"
                :disabled="it.o.disabled" role="option" :aria-selected="it.o.value === modelValue"
                @click="pick(it.o)" @mousemove="active = it.i">
          <span class="zsel-tick">
            <svg v-if="it.o.value === modelValue" width="14" height="14" viewBox="0 0 24 24" fill="none"
                 stroke="currentColor" stroke-width="2.6" stroke-linecap="round"><path d="m5 13 4 4L19 7"/></svg>
          </span>
          {{ it.o.label }}
        </button>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.zsel{display:flex;align-items:center;gap:8px;height:38px;padding:0 12px;cursor:pointer;
  background:var(--color-card);border:1.5px solid var(--color-border);border-radius:var(--r-md);
  font-size:var(--fs-body);color:var(--color-text);
  transition:border-color var(--dur),box-shadow var(--dur)}
.zsel:hover:not(.disabled){border-color:var(--sky-400)}
.zsel:focus-visible,.zsel.open{border-color:var(--color-primary);box-shadow:0 0 0 3px var(--color-ring);outline:none}
.zsel.disabled{background:var(--color-border-soft);color:var(--color-text-3);cursor:not-allowed;border-style:dashed}
/* 前缀(如"状态:")留在控件内部:整块是一个可点区域,点哪都能展开 */
.zsel-prefix{flex:none;color:var(--color-text-3)}
/* 分组名(仅带 group 的选项显示) */
.zsel-group{flex:none;color:var(--color-text-3)}
.zsel-label{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
/* 展开时箭头翻转 180° */
.zsel-arrow{flex:none;color:var(--color-text-3);transition:transform var(--dur)}
.zsel.open .zsel-arrow{transform:rotate(180deg)}
/* 浮层:复用全局 .menu/.menu-item 的外观,只补高度、层级与选中态。
   z-index 必须高过弹窗遮罩(.modal-mask 是 1000):.menu 自带的 65 会让浮层被
   遮罩整个盖住 —— 表现为"弹窗里的下拉打不开"(工具条里的则正常) */
.zsel-pop{max-height:280px;overflow-y:auto;padding:6px;z-index:1100}
/* 分组:组间加分隔线,组标题不可选(键盘只在 options 上移动,天然跳过) */
.zsel-pop > div + div{margin-top:4px;border-top:1px solid var(--color-border-soft)}
.zsel-group-head{padding:6px 10px 2px;font-size:var(--fs-cap);font-weight:700;color:var(--color-text-3)}
.zsel-opt{padding:8px 10px}
.zsel-opt.on{background:var(--color-border-soft);color:var(--color-text)}
.zsel-opt.picked{color:var(--color-primary);font-weight:600}
.zsel-opt.picked.on{background:var(--color-primary-bg, var(--color-border-soft))}
.zsel-tick{display:inline-flex;width:16px;flex:none;color:currentColor}
</style>
