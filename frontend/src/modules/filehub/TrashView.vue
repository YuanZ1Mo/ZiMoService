<script setup>
// 回收站视图(§3.7.2/§7.4):只读 + 恢复/彻底删除/清空;仅显示"我删除的"(del_owner_uid=我)
// 不提供重命名/移动/复制/上传入口;清空为异步任务(转任务面板),需二次确认
import { ref, reactive, computed, onMounted, watch, inject } from 'vue'
import Modal from '../../components/Modal.vue'
import FileIcon from './FileIcon.vue'
import { filehubApi, fmtSize, fmtTime } from '../../api/filehub'
import { useFilehubStore } from '../../stores/filehub'

const props = defineProps({
  space: { type: Number, required: true },
  meSpace: { type: Number, default: 0 }
})
const emit = defineEmits(['changed'])
const toast = inject('toast')
const store = useFilehubStore()

const rows = ref([])
const total = ref(0)
const used = reactive({ size: 0, items: 0 })
const retainDays = ref(30)
const loading = ref(false)
const selected = ref(new Set())
const confirmBox = reactive({ show: false, title: '', msg: '', html: '', fn: null })
const page = ref(1)
const PAGE_SIZE = 100
const totalPages = computed(() => Math.max(1, Math.ceil(total.value / PAGE_SIZE)))

async function load() {
  loading.value = true
  try {
    const d = await filehubApi.trash({ space: props.space, page: page.value, size: PAGE_SIZE })
    rows.value = d.list || []
    total.value = d.total || 0
    used.size = d.used_size || 0
    used.items = d.used_items || 0
    retainDays.value = d.retain_days || 30
    selected.value = new Set()
    // 删空当前页时回退一页,避免停在空白页
    if (!rows.value.length && page.value > 1 && total.value > 0) {
      page.value--
      return load()
    }
  } catch (e) {
    toast(e.message || '加载回收站失败', 'err')
  } finally { loading.value = false }
}
onMounted(load)
// 切空间:回收站内容与占用都换了,回到第 1 页重载
watch(() => props.space, () => { page.value = 1; load() })

/**
 * 翻页
 * @param p  目标页(越界或当前页则忽略)
 */
function goto(p) {
  if (p < 1 || p > totalPages.value || p === page.value) return
  page.value = p
  load()
}
function reload() { emit('changed'); load() }

function toggleSel(n) {
  const s = new Set(selected.value)
  s.has(n.id) ? s.delete(n.id) : s.add(n.id)
  selected.value = s
}
function selSize() {
  return rows.value.filter(r => selected.value.has(r.id)).reduce((a, r) => a + Number(r.size || 0), 0)
}
function selItems() {
  return rows.value.filter(r => selected.value.has(r.id))
}
const remainDays = (n) => Math.max(0, retainDays.value - Math.floor((Date.now() / 1000 - n.delete_time) / 86400))

function askConfirm(title, html, fn) {
  confirmBox.title = title; confirmBox.html = html; confirmBox.fn = fn; confirmBox.show = true
}
function restore(items) {
  if (!items.length) return
  askConfirm('恢复所选条目?', `将恢复 <b>${items.length}</b> 项到原位置;原位置被占用或原目录已删除时,恢复到空间根并自动重命名。`, async () => {
    try {
      const r = await filehubApi.trashRestore(items.map(x => x.id))
      const okN = (r.restored || []).length
      const failN = (r.failed || []).length
      // 是否被自动重命名:按服务端返回的最终名与原名比对
      // (原判定 `/ \(\d+\)$/` 要求序号在末尾,而实际是插在扩展名之前,对带扩展名的文件永不成立)
      const origin = new Map(items.map(x => [x.id, x.name]))
      const renamed = (r.restored || []).some(x => x.name && x.name !== origin.get(x.id))
      // 部分失败要说清数量,否则用户以为全都恢复了
      toast(`已恢复 ${okN} 项${renamed ? '(部分自动重命名)' : ''}` +
            (failN ? `,${failN} 项失败(可能已被彻底删除)` : ''), failN ? 'warn' : 'ok')
      reload()
    } catch (e) { toast(e.message || '恢复失败', 'err') }
  })
}
function purge(items) {
  if (!items.length) return
  const bytes = items.reduce((a, x) => a + Number(x.size || 0), 0)
  askConfirm(`彻底删除 ${items.length} 项?`,
    `将连同全部子项<b style="color:var(--color-err)">物理删除</b>,共 ${fmtSize(bytes)},<b style="color:var(--color-err)">不可撤销</b>。`,
    async () => {
      try {
        const r = await filehubApi.trashPurge(items.map(x => x.id))
        const okN = (r.success || []).length
        const failN = (r.failed || []).length
        toast(`已彻底删除 ${okN} 项` + (failN ? `,${failN} 项失败` : ''), failN ? 'warn' : 'ok')
        reload()
      } catch (e) { toast(e.message || '删除失败', 'err') }
    })
}
function clearAll() {
  askConfirm('清空回收站?',
    `将<b style="color:var(--color-err)">永久删除</b>此空间中你删除的全部 <b>${used.items}</b> 个条目,共 <b>${fmtSize(used.size)}</b>,不可撤销;操作在后台执行,进度见任务面板。`,
    async () => {
      try {
        await filehubApi.trashClear(props.space)
        toast('已创建清空任务,进度见任务面板', 'ok')
        store.togglePanel(true)   // 打开任务面板并起轮询(清空是后台任务,列表此时还没变)
        reload()
      } catch (e) { toast(e.message || '操作失败', 'err') }
    })
}
</script>

<template>
  <div class="card fh-toolbar">
    <span class="cap" style="color:var(--color-text-2)">
      条目将在删除 <b class="num">{{ retainDays }}</b> 天后自动清除 · 期间持续占用空间配额
    </span>
    <span class="grow" style="flex:1"></span>
    <span class="badge badge-warn num">{{ used.items }} 项 · {{ fmtSize(used.size) }}</span>
    <button class="btn btn-danger btn-sm" type="button" :disabled="!used.items" @click="clearAll">清空回收站</button>
  </div>

  <div class="card" style="padding:0;overflow:hidden">
    <div style="overflow-x:auto">
      <table class="ftbl">
        <thead>
          <tr>
            <th class="col-cb"></th>
            <th style="min-width:180px">名称</th>
            <th style="width:80px">类型</th>
            <th style="width:100px">大小</th>
            <th style="min-width:170px">原路径</th>
            <th style="width:120px">删除时间</th>
            <th style="width:90px">删除者</th>
            <th style="width:90px">剩余</th>
            <th style="width:150px"></th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="loading">
            <td :colspan="9" style="padding:16px">
              <div v-for="i in 5" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
            </td>
          </tr>
          <template v-else>
            <tr v-for="n in rows" :key="n.id" :class="{ sel: selected.has(n.id) }">
              <td class="col-cb" @click.stop>
                <input type="checkbox" class="fcheck" :checked="selected.has(n.id)" @change="toggleSel(n)" :aria-label="`选择 ${n.name}`" />
              </td>
              <td>
                <div class="fcell">
                  <FileIcon :node="n" />
                  <span class="fname">{{ n.name }}</span>
                </div>
              </td>
              <td><span class="ftype">{{ Number(n.type) === 1 ? '文件夹' : '文件' }}</span></td>
              <td>
                <span class="num" :class="{ 'items-num': Number(n.type) === 1 }">
                  {{ Number(n.type) === 1 ? `${n.items} 项 · ${fmtSize(n.size)}` : fmtSize(n.size) }}
                </span>
              </td>
              <td>
                <!-- path 为空有两种情形:条目本来就在空间根,或原父目录已被删除
                     (服务端 origin_parent_id=0 即前者),不能一律说成"原目录已删除" -->
                <span style="font-size:var(--fs-cap)"
                      :style="n.path || !Number(n.origin_parent_id) ? 'color:var(--color-text-2)' : 'color:var(--color-warn)'">
                  {{ n.path || (Number(n.origin_parent_id) ? '原目录已删除(恢复到空间根)' : '空间根') }}
                </span>
              </td>
              <td><span class="num">{{ fmtTime(n.delete_time) }}</span></td>
              <td><span class="cap">{{ n.del_owner_name }}</span></td>
              <td><span class="badge badge-warn num">剩 {{ remainDays(n) }} 天</span></td>
              <td>
                <span class="row-ops">
                  <button class="btn btn-secondary btn-sm" type="button" @click="restore([n])">恢复</button>
                  <button class="btn btn-danger-soft btn-sm" type="button" @click="purge([n])">彻底删除</button>
                </span>
              </td>
            </tr>
            <tr v-if="!rows.length">
              <td :colspan="9">
                <div class="empty">
                  <div class="empty-icon">🗑</div>
                  <div class="empty-title">回收站为空</div>
                  <div class="empty-sub">你在此空间删除的条目会出现在这里</div>
                </div>
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
    <!-- 分页:超过一页的条目此前完全不可见、无法勾选 -->
    <div v-if="total > PAGE_SIZE" class="pager">
      <span class="pg-info">共 <b class="num">{{ total }}</b> 项 · 第 <b class="num">{{ page }} / {{ totalPages }}</b> 页</span>
      <div class="pg-btns">
        <button class="pg-btn" type="button" :disabled="page <= 1" @click="goto(page - 1)">‹</button>
        <button class="pg-btn" type="button" :disabled="page >= totalPages" @click="goto(page + 1)">›</button>
      </div>
    </div>
  </div>

  <!-- 批量条 -->
  <div v-if="selected.size" class="card batch-bar">
    <button class="icon-btn" type="button" aria-label="取消选择" @click="selected = new Set()">
      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
    </button>
    <span class="sel-num num">已选 {{ selected.size }} 项 · {{ fmtSize(selSize()) }}</span>
    <span style="flex:1"></span>
    <button class="btn btn-secondary btn-sm" type="button" @click="restore(selItems())">恢复</button>
    <button class="btn btn-danger-soft btn-sm" type="button" @click="purge(selItems())">彻底删除</button>
  </div>

  <Modal :show="confirmBox.show" :title="confirmBox.title" @close="confirmBox.show = false">
    <p class="dlg-tip" style="margin:0" v-html="confirmBox.html"></p>
    <template #foot>
      <button class="btn btn-ghost" type="button" @click="confirmBox.show = false">取消</button>
      <button class="btn btn-danger" type="button" @click="confirmBox.show = false; confirmBox.fn && confirmBox.fn()">确认执行</button>
    </template>
  </Modal>
</template>

<style scoped>
.fcheck{width:16px;height:16px;accent-color:var(--color-primary);cursor:pointer}
</style>
