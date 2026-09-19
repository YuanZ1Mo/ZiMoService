<script setup>
// 右下角传输任务面板:进行中(服务端 tasks/active + 客户端上传队列)/ 历史
// 轮询契约在 stores/filehub.js;本组件只负责展示与操作(取消/重试/下载/清理)
import { ref, computed, watch, inject, onDeactivated } from 'vue'
import { useFilehubStore } from '../../stores/filehub'
import { filehubApi, fmtSize, fmtTime } from '../../api/filehub'

const store = useFilehubStore()
const toast = inject('toast')
const tab = ref('active')
const loadingHistory = ref(false)

// ── 面板可拖动:默认贴右下角,按住标题栏可拖到任意位置(位置记住在当前会话) ──
// 拖动后改用 left/top 定位(内联覆盖 CSS 的 right/bottom);没拖过就不设内联定位,回落到右下角
const panelEl = ref(null)
const dragging = ref(false)
const dragPos = ref(null)   // { left, top }
let offMove = null
let offUp = null
/**
 * 按下标题栏开始拖动
 *
 * 标题栏内的按钮(进行中/历史、收起)不参与,否则点标签页会变成拖拽。
 *
 * @param e  mousedown 事件
 */
function onDragStart(e) {
  if (e.button !== 0 || !panelEl.value) return
  if (e.target.closest && e.target.closest('button')) return
  const r    = panelEl.value.getBoundingClientRect()
  const from = { mx: e.clientX, my: e.clientY, left: r.left, top: r.top }
  dragging.value = true
  e.preventDefault()   // 拖动时不要顺带选中标题文字
  // 夹在视口内,且让开门户顶栏与侧栏 —— 两者 z-index 都比面板高,压上去会盖住标题栏,
  // 从被盖住的那段就再也抓不动面板了(移动端侧栏是移出屏外的抽屉,right 为负 → 退回 8)
  const topMin    = (parseFloat(getComputedStyle(document.documentElement)
                                .getPropertyValue('--topbar-h')) || 60) + 8
  const side      = document.querySelector('.portal-side')
  const sideRight = side ? side.getBoundingClientRect().right : 0
  const maxL      = Math.max(0, window.innerWidth - r.width - 16)
  const leftMin   = Math.min(maxL, Math.max(8, Math.round(sideRight) + 8))
  offMove = (ev) =>
  {
    const maxT = Math.max(topMin, window.innerHeight - r.height - 16)
    dragPos.value = {
      left: Math.min(maxL, Math.max(leftMin, from.left + ev.clientX - from.mx)),
      top:  Math.min(maxT, Math.max(topMin, from.top + ev.clientY - from.my))
    }
  }
  offUp = () =>
  {
    dragging.value = false
    window.removeEventListener('mousemove', offMove)
    window.removeEventListener('mouseup', offUp)
    offMove = offUp = null
  }
  window.addEventListener('mousemove', offMove)
  window.addEventListener('mouseup', offUp)
}
// 面板随模块失活时若仍在拖,清掉全局监听,避免残留到其它页面
onDeactivated(() => { if (offUp) offUp() })

const TYPE_NAME = { 1: '上传', 2: '复制', 3: '打包下载', 4: '目录统计', 5: '一致性同步', 6: '回收站清理' }
const STATUS_NAME = { 1: '排队中', 2: '进行中', 3: '已完成', 4: '失败', 5: '已取消', 6: '已中断' }
const statusChip = (s) => ({ 1: 'wait', 2: 'run', 3: 'ok', 4: 'fail', 5: 'stop', 6: 'stop' }[Number(s)] || 'wait')
// 任务进度:有条目数按条目(打包/复制/同步),否则按字节(上传)
function progress(t) {
  if (Number(t.status) === 3) return 100
  if (Number(t.total_items) > 0) return Math.min(100, Math.round(t.done_items / t.total_items * 100))
  if (Number(t.size) > 0) return Math.min(100, Math.round(t.done_size / t.size * 100))
  return Number(t.status) === 2 ? 50 : 0
}
function progressText(t) {
  const parts = []
  if (Number(t.total_items) > 0) parts.push(`${t.done_items} / ${t.total_items} 项`)
  if (Number(t.size) > 0) parts.push(`${fmtSize(t.done_size)} / ${fmtSize(t.size)}`)
  return parts.join(' · ') || (STATUS_NAME[Number(t.status)] || '')
}

const historyItems = computed(() => store.history)
async function loadHistory() {
  loadingHistory.value = true
  try {
    const d = await filehubApi.tasks({ page: 1, size: 50 })
    store.history = d.list || []
    store.historyTotal = d.total || 0
  } catch { /* 静默 */ } finally { loadingHistory.value = false }
}
watch(tab, (v) => { if (v === 'history') loadHistory() })
watch(() => store.panelOpen, (v) => { if (v && tab.value === 'history') loadHistory() })

const isEmpty = computed(() =>
  tab.value === 'active'
    ? !store.serverTasks.length && !store.uploads.length
    : !historyItems.value.length && !loadingHistory.value)

/**
 * 下载打包产物
 *
 * 产物在缓存区,可能已被清理(默认空闲 30 分钟回收),失败要说一声,不能静默。
 *
 * @param t  任务对象(取 task_no)
 */
async function downloadZip(t) {
  try { await store.downloadZip(t.task_no) }
  catch (e) { toast(e.message || '下载失败,压缩包可能已被清理', 'err') }
}
async function cancelTask(t) {
  try { await filehubApi.taskCancel(t.task_no); store.refreshActive() }
  catch (e) { toast(e.message || '取消失败', 'err') }
}
async function retryTask(t) {
  try { await filehubApi.taskRetry(t.task_no); store.refreshActive() }
  catch (e) { toast(e.message || '重试失败', 'err') }
}
async function clearHistory() {
  try {
    await filehubApi.tasksClear()
    store.history = []; store.historyTotal = 0; store.clearFinished()
  } catch (e) { toast(e.message || '清除历史失败', 'err') }
}
/**
 * 删除单条历史记录
 *
 * 只删记录,不动打包压缩包 —— 压缩包是缓存文件,由缓存回收统一处理(空闲 30 分钟 /
 * 每日兜底 / 容量阈值),与"清除历史"同一口径。
 * 删完按服务端重拉列表 —— 原先只改本地数组,切 tab 重新拉取时记录又冒出来了。
 *
 * @param t  任务对象(取 task_no)
 */
async function deleteTask(t) {
  try {
    await filehubApi.taskDelete(t.task_no)
    await loadHistory()
  } catch (e) { toast(e.message || '删除失败', 'err') }
}
</script>

<template>
  <!-- 入口在侧栏「传输任务」(原先的右下角悬浮球会盖住列表最后一行的 ⋯ 操作) -->

  <!-- 面板留在模块 DOM 内:传送到 body 会在 keep-alive 缓存期间残留到其它页面 -->
  <div v-if="store.panelOpen" ref="panelEl" class="task-panel" :class="{ dragging }" role="dialog"
       aria-label="传输任务面板"
       :style="dragPos ? { left: dragPos.left + 'px', top: dragPos.top + 'px', right: 'auto', bottom: 'auto' } : null">
    <!-- 标题栏 = 拖动把手(按住可拖到任意位置;栏内按钮不参与拖动) -->
    <div class="task-head" @mousedown="onDragStart">
      <b>传输任务</b>
      <span v-if="store.activeCount" class="badge badge-info num">进行中 {{ store.activeCount }}</span>
      <div class="task-tabs">
        <button type="button" :class="{ on: tab === 'active' }" @click="tab = 'active'">进行中</button>
        <button type="button" :class="{ on: tab === 'history' }" @click="tab = 'history'">历史</button>
      </div>
      <button class="icon-btn" style="width:30px;height:30px" type="button" aria-label="收起面板" @click="store.togglePanel(false)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
      </button>
    </div>

    <div class="task-list">
      <template v-if="tab === 'active'">
        <!-- 客户端上传队列(客户端视角进度) -->
        <template v-for="u in [...store.uploads].reverse()" :key="u.key">
          <div class="grp-label" v-if="u.batch && u === store.uploads.find(x => x.batch === u.batch)">上传{{ u.batch ? `「${u.batch}」` : '' }}</div>
          <div class="titem">
            <div class="t-top">
              <span class="st-chip" :class="{ run: u.status === 'run', wait: u.status === 'wait', ok: u.status === 'ok', fail: u.status === 'fail', stop: u.status === 'stop' }">
                {{ { run: '上传中', wait: '排队中', ok: '已完成', fail: '失败', stop: '已取消' }[u.status] }}
              </span>
              <span class="t-name">{{ u.name }}</span>
              <span class="t-ops">
                <button v-if="u.status === 'run' || u.status === 'wait'" class="btn btn-ghost btn-sm" type="button" @click="store.cancelUpload(u.key)">取消</button>
                <!-- 同名冲突:服务端列了冲突条目,选策略后按该策略重发(直接重试仍是 ask,会再撞 409) -->
                <template v-else-if="u.status === 'fail' && u.conflicts.length">
                  <button class="btn btn-ghost btn-sm" type="button" @click="store.retryUpload(u.key, 'skip')">跳过</button>
                  <button class="btn btn-primary btn-sm" type="button" @click="store.retryUpload(u.key, 'rename')">重命名</button>
                  <button class="btn btn-ghost btn-sm" type="button" @click="store.retryUpload(u.key, 'overwrite')">覆盖</button>
                </template>
                <button v-else-if="u.status === 'fail'" class="btn btn-secondary btn-sm" type="button" @click="store.retryUpload(u.key)">重试</button>
              </span>
            </div>
            <div class="tprog" :class="{ done: u.status === 'ok', err: u.status === 'fail' }"><i :style="{ width: (u.size ? Math.min(100, u.done / u.size * 100) : 0) + '%' }"></i></div>
            <div class="t-sub">
              <template v-if="u.status === 'fail' && u.conflicts.length">
                与「{{ u.conflicts.map(c => c.name).join('、') }}」同名,请选择处理方式
              </template>
              <template v-else-if="u.status === 'fail'">{{ u.error }}</template>
              <template v-else-if="u.status === 'ok'">{{ u.skipped ? '已跳过(同名)' : (u.instant ? '秒传完成' : '上传完成') }}</template>
              <template v-else><span class="num">{{ fmtSize(u.done) }} / {{ fmtSize(u.size) }}</span></template>
            </div>
          </div>
        </template>
        <!-- 服务端任务(1.5s 轮询 tasks/active) -->
        <div v-for="t in store.serverTasks" :key="t.task_no" class="titem">
          <div class="t-top">
            <span class="st-chip" :class="statusChip(t.status)">{{ STATUS_NAME[Number(t.status)] }}</span>
            <span class="t-name">{{ t.name }}</span>
            <span class="t-ops">
              <button class="btn btn-ghost btn-sm" type="button" @click="cancelTask(t)">取消</button>
            </span>
          </div>
          <div class="tprog"><i :style="{ width: progress(t) + '%' }"></i></div>
          <div class="t-sub"><span class="num">{{ TYPE_NAME[Number(t.type)] }} · {{ progressText(t) }}</span></div>
        </div>
      </template>

      <template v-else>
        <div v-if="loadingHistory" style="padding:12px">
          <div v-for="i in 4" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
        </div>
        <div v-for="t in historyItems" :key="t.task_no" class="titem">
          <div class="t-top">
            <span class="st-chip" :class="statusChip(t.status)">{{ STATUS_NAME[Number(t.status)] }}</span>
            <span class="t-name">{{ t.name }}</span>
            <span class="t-ops">
              <!-- 打包完成:可下载产物(产物空闲 30 分钟自动回收) -->
              <button v-if="Number(t.type) === 3 && Number(t.status) === 3" class="btn btn-secondary btn-sm" type="button" @click="downloadZip(t)">下载</button>
              <button v-if="Number(t.status) === 4 || Number(t.status) === 6" class="btn btn-secondary btn-sm" type="button" @click="retryTask(t)">重试</button>
              <!-- 删除记录(压缩包是缓存,由缓存回收处理,不随记录删) -->
              <button class="btn btn-danger-soft btn-sm" type="button" @click="deleteTask(t)">删除</button>
            </span>
          </div>
          <div class="tprog" :class="{ done: Number(t.status) === 3, err: Number(t.status) === 4 }"><i :style="{ width: progress(t) + '%' }"></i></div>
          <div class="t-sub">
            <template v-if="t.error && Number(t.status) !== 3">{{ t.error }}</template>
            <template v-else>
              <span class="num">{{ TYPE_NAME[Number(t.type)] }} · {{ fmtTime(t.end_time || t.create_time) }}{{ Number(t.total_items) ? ` · ${t.total_items} 项` : '' }}{{ t.size ? ` · ${fmtSize(t.size)}` : '' }}</span>
            </template>
          </div>
        </div>
      </template>

      <div v-if="isEmpty" class="empty" style="padding:32px 16px">
        <div class="empty-icon">🛫</div>
        <div class="empty-title" style="font-size:var(--fs-body)">{{ tab === 'active' ? '暂无进行中任务' : '暂无历史任务' }}</div>
      </div>
    </div>

    <!-- 清除历史只对历史页有意义,进行中页不显示(原先无条件渲染,进行中页也有这个按钮) -->
    <div v-if="tab === 'history'" class="t-foot">
      <button class="btn btn-ghost btn-sm" type="button" @click="clearHistory">清除历史</button>
    </div>
  </div>
</template>
