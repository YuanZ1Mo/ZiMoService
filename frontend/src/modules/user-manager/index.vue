<script setup>
// 系统管理 - 用户管理(模块 code=userManager,index=-1)
// 列表/表格(服务端下发列元数据驱动) + 分页/搜索/角色/状态筛选 + 管理操作
// 交互:行内"编辑 + 更多菜单";破坏性操作走确认弹窗;重置密码临时口令一次性展示
import { ref, reactive, computed, onMounted, onBeforeUnmount, onDeactivated, inject } from 'vue'
import { adminApi } from '../../api/admin'
import Modal from '../../components/Modal.vue'

const toast = inject('toast')

const columns = ref([])
const rows = ref([])
const total = ref(0)
const loading = ref(false)
const denied = ref(false)   // 无权限(PERM_DENIED):整页提示而非空列表

const query = reactive({ page: 1, size: 10, search: '', role: '', status: '' })
const incDel = ref(false)   // 包含已删除(默认不显示,符合"列表默认不显示"约定)
const roles = ref(['developer', 'admin', 'user'])
const permList = ref([])
const statusSeg = [
  { value: '', name: '全部' },
  { value: '1', name: '正常' },
  { value: '2', name: '已停用' }
]

// 对话框状态
const dlg = reactive({ kind: '', show: false, user: null })
const editForm = reactive({})
const roleSel = ref('')
const permSel = ref({ permCode: '', grantType: 1 })
const tempPassword = ref('')
const copied = ref(false)
const busy = ref(false)

// 行操作菜单(固定定位,避免表格滚动容器裁剪)
const rowMenu = reactive({ uid: null, x: 0, y: 0 })

// 确认弹窗(替代 window.confirm)
const confirmBox = reactive({ show: false, title: '', msg: '', fn: null })

// 系统管理子标签:本期实现用户管理,其余为 TODO 预留入口(点击提示建设中)
const sysTabs = [
  { key: 'users', name: '用户管理', todo: false },
  { key: 'roles', name: '角色管理', todo: true },
  { key: 'perms', name: '权限管理', todo: true },
  { key: 'audit', name: '审计日志', todo: true }
]
const sysTab = ref('users')

function switchTab(tab) {
  sysTab.value = tab.key
  if (tab.todo) toast(`${tab.name}建设中`, 'warn')
}

const visibleColumns = computed(() => columns.value.filter(c => c.visible))
const totalPages = computed(() => Math.max(1, Math.ceil(total.value / query.size)))

function fmtTime(v) {
  if (!v) return '-'
  const d = new Date(Number(v) * 1000)
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}

function cellText(row, col) {
  const v = row[col.key]
  if (v === undefined || v === null || v === '') return '-'
  if (col.key === 'status') return Number(v) === 2 ? '已停用' : Number(v) === 1 ? '正常' : v
  if (col.key === 'deleted') return Number(v) === 1 ? '已删除' : '否'
  if (col.key === 'force_change') return Number(v) === 1 ? '待改密' : '—'
  if (col.key.includes('time')) return fmtTime(v)
  return String(v)
}

function statusBadge(row) {
  if (Number(row.deleted) === 1) return 'badge-err'
  if (Number(row.status) === 2) return 'badge-warn'
  return 'badge-ok'
}
function statusText(row) {
  if (Number(row.deleted) === 1) return '已删除'
  if (Number(row.status) === 2) return '已停用'
  return '正常'
}
function roleBadge(code) {
  if (code === 'developer') return 'badge-role-developer'
  if (code === 'admin') return 'badge-role-admin'
  return 'badge-role-user'
}
function avatarChar(row) {
  const n = row.nickname || row.account || '?'
  return n.charAt(0).toUpperCase()
}

async function load() {
  loading.value = true
  try {
    const data = await adminApi.users({
      page: query.page, size: query.size, search: query.search,
      role: query.role, status: query.status,
      includeDeleted: incDel.value ? 1 : ''
    })
    rows.value = data.list || []
    total.value = data.total || 0
  } catch (e) {
    if (e.status === 403) { denied.value = true; return }
    toast(e.message || '加载用户列表失败', 'err')
  } finally {
    loading.value = false
  }
}

async function init() {
  try {
    const [c, p] = await Promise.all([adminApi.columns(), adminApi.permCodes()])
    columns.value = (c && c.columns) || []
    permList.value = (p && p.permissions) || []
  } catch (e) {
    if (e.status === 403) { denied.value = true; return }
    toast('加载列配置失败', 'err')
  }
  await load()
}

function search() {
  query.page = 1
  load()
}
function pageOf(n) {
  if (n < 1 || n > totalPages.value) return
  query.page = n
  load()
}

// ── 行操作菜单 ──
function toggleRowMenu(e, row) {
  const r = e.currentTarget.getBoundingClientRect()
  const mw = 190, mh = 216
  rowMenu.uid = rowMenu.uid === row.uid ? null : row.uid
  rowMenu.x = Math.min(r.right - mw, window.innerWidth - mw - 8)
  rowMenu.y = r.bottom + 6
  if (rowMenu.y + mh > window.innerHeight) rowMenu.y = r.top - mh - 6
}
function closeRowMenu() { rowMenu.uid = null }
function onDocClick() { closeRowMenu() }
onMounted(() => document.addEventListener('click', onDocClick))
onBeforeUnmount(() => document.removeEventListener('click', onDocClick))
onDeactivated(closeRowMenu)

// ── 打开各弹窗 ──
function openEdit(row) {
  dlg.kind = 'edit'
  dlg.user = row
  Object.keys(editForm).forEach(k => delete editForm[k])
  editForm.nickname = row.nickname
  editForm.email = row.email || ''
  editForm.phone = row.phone || ''
  editForm.status = String(row.status || 1)
  dlg.show = true
}
function openRole(row) {
  dlg.kind = 'role'
  dlg.user = row
  roleSel.value = row.role_code
  dlg.show = true
}
function openPerm(row) {
  dlg.kind = 'perm'
  dlg.user = row
  permSel.value = { permCode: permList.value.length ? permList.value[0].code : '', grantType: 1 }
  dlg.show = true
}
function openReset(row) {
  dlg.kind = 'reset'
  dlg.user = row
  tempPassword.value = ''
  copied.value = false
  dlg.show = true
}

// ── 确认弹窗 ──
function askConfirm(title, msg, fn) {
  confirmBox.title = title
  confirmBox.msg = msg
  confirmBox.fn = fn
  confirmBox.show = true
}
function runConfirm() {
  confirmBox.show = false
  if (confirmBox.fn) confirmBox.fn()
}

async function saveEdit() {
  busy.value = true
  try {
    const patch = { nickname: editForm.nickname, email: editForm.email, phone: editForm.phone, status: Number(editForm.status) }
    await adminApi.patch(dlg.user.uid, patch)
    toast('已保存', 'ok')
    dlg.show = false
    load()
  } catch (e) {
    toast(e.message || '保存失败', 'err')
  } finally {
    busy.value = false
  }
}

async function saveRole() {
  busy.value = true
  try {
    await adminApi.role(dlg.user.uid, roleSel.value)
    toast('角色已更新', 'ok')
    dlg.show = false
    load()
  } catch (e) {
    toast(e.message || '更新失败', 'err')
  } finally {
    busy.value = false
  }
}

async function savePerm() {
  busy.value = true
  try {
    await adminApi.permissions(dlg.user.uid, permSel.value.permCode, Number(permSel.value.grantType))
    toast('模块权限已更新', 'ok')
    dlg.show = false
  } catch (e) {
    toast(e.message || '授权失败', 'err')
  } finally {
    busy.value = false
  }
}

async function doReset() {
  busy.value = true
  try {
    const data = await adminApi.resetPassword(dlg.user.uid)
    tempPassword.value = data.tempPassword || ''
    toast('已重置密码,请转交用户', 'ok')
  } catch (e) {
    toast(e.message || '重置失败', 'err')
  } finally {
    busy.value = false
  }
}

async function toggleStatus(row) {
  const disabled = Number(row.status) === 2
  closeRowMenu()
  askConfirm(
    disabled ? `确定启用用户「${row.account}」?` : `确定停用用户「${row.account}」?`,
    disabled
      ? '启用后该用户可重新登录。'
      : '停用后该用户不可登录,其全部会话将被立即吊销;后续可随时"启用"恢复。',
    async () => {
      try {
        if (disabled) await adminApi.enable(row.uid)
        else await adminApi.disable(row.uid)
        toast(disabled ? '已启用' : '已停用', 'ok')
        load()
      } catch (e) {
        toast(e.message || '操作失败', 'err')
      }
    }
  )
}

async function toggleDelete(row) {
  const deleted = Number(row.deleted) === 1
  closeRowMenu()
  askConfirm(
    deleted ? `确定恢复用户「${row.account}」?` : `确定删除用户「${row.account}」?`,
    deleted
      ? '恢复后该用户重新变为正常状态,可正常登录。'
      : '删除为软删除:该用户将不可登录且列表默认不显示;可通过"包含已删除"开关找到并恢复。',
    async () => {
      try {
        if (deleted) await adminApi.restore(row.uid)
        else await adminApi.remove(row.uid)
        toast(deleted ? '已恢复' : '已删除', 'ok')
        load()
      } catch (e) {
        toast(e.message || '操作失败', 'err')
      }
    }
  )
}

async function copyTmp() {
  try {
    await navigator.clipboard.writeText(tempPassword.value)
    copied.value = true
    setTimeout(() => { copied.value = false }, 1500)
  } catch { /* 剪贴板不可用时忽略,用户可手动选中复制 */ }
}

onMounted(init)
</script>

<template>
  <div>
    <!-- 系统管理子标签:用户管理 + TODO 预留入口(§9.1) -->
    <div class="page-head">
      <h1>系统管理</h1>
      <p class="page-sub">管理用户、角色与权限 · 所有操作将记录审计日志</p>
    </div>
    <div class="underline-tabs">
      <button v-for="t in sysTabs" :key="t.key" type="button" class="utab"
              :class="{ on: sysTab === t.key, todo: t.todo }" @click="switchTab(t)">
        {{ t.name }}
        <span v-if="t.todo" class="badge badge-dim">TODO</span>
      </button>
    </div>

    <!-- 无权限:明确提示,不渲染列表(接口 PERM_DENIED) -->
    <div v-if="denied" class="card work-card" style="margin-top:18px">
      <div class="empty">
        <div class="empty-icon">⛔</div>
        <div class="empty-title">无权限访问</div>
        <div class="empty-sub">您没有用户管理模块的操作权限,请联系管理员授权</div>
      </div>
    </div>

    <div v-else class="card work-card" style="margin-top:18px">
      <!-- 工具栏 -->
      <div class="toolbar">
        <div class="input-wrap tb-search">
          <span class="input-prefix" aria-hidden="true">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></svg>
          </span>
          <input v-model.trim="query.search" class="input" style="height:38px" placeholder="搜索账号 / 昵称"
                 @keyup.enter="search" />
        </div>
        <select v-model="query.role" class="input tb-sel" @change="search">
          <option value="">全部角色</option>
          <option v-for="r in roles" :key="r" :value="r">{{ r }}</option>
        </select>
        <div class="seg seg-filter">
          <button v-for="s in statusSeg" :key="s.value" type="button" class="seg-item"
                  :class="{ active: query.status === s.value }" @click="query.status = s.value; search()">
            {{ s.name }}
          </button>
        </div>
        <div style="flex:1"></div>
        <button class="switch-row" type="button" @click="incDel = !incDel; search()">
          <span class="switch" :class="{ on: incDel }"></span>包含已删除
        </button>
      </div>

      <!-- 列表(列元数据驱动) -->
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr>
              <th v-for="c in visibleColumns" :key="c.key">{{ c.label }}</th>
              <th style="text-align:right">操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="loading">
              <td :colspan="visibleColumns.length + 1" style="padding:16px">
                <div v-for="i in 5" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
              </td>
            </tr>
            <template v-else>
              <tr v-for="row in rows" :key="row.uid" :class="{ 'is-deleted': Number(row.deleted) === 1 }">
                <td v-for="c in visibleColumns" :key="c.key">
                  <!-- 用户列:头像 + 昵称 + 账号/UID -->
                  <template v-if="c.key === 'account' || c.key === 'nickname'">
                    <div v-if="c.key === 'account'" class="cell-user">
                      <span class="avatar">{{ avatarChar(row) }}</span>
                      <div style="min-width:0">
                        <b>{{ row.nickname || row.account }}</b>
                        <div class="sub num">{{ row.account }} · UID {{ row.uid }}</div>
                      </div>
                    </div>
                    <template v-else>{{ cellText(row, c) }}</template>
                  </template>
                  <!-- 角色列:语义徽章 -->
                  <span v-else-if="c.key === 'role_code'" class="badge" :class="roleBadge(row.role_code)">
                    {{ row.role_code || '-' }}
                  </span>
                  <!-- 状态列:状态徽章(含已删除) -->
                  <span v-else-if="c.key === 'status'" class="badge" :class="statusBadge(row)">
                    <span class="dot" aria-hidden="true"></span>{{ statusText(row) }}
                  </span>
                  <!-- 强制改密列 -->
                  <span v-else-if="c.key === 'force_change'">
                    <span v-if="Number(row.force_change) === 1" class="badge badge-force">待改密</span>
                    <span v-else style="color:var(--color-text-3)">—</span>
                  </span>
                  <template v-else>{{ cellText(row, c) }}</template>
                </td>
                <td>
                  <div class="tbl-actions">
                    <!-- 已删除:仅"恢复"(按钮按状态切换,已确认决策) -->
                    <template v-if="Number(row.deleted) === 1">
                      <button class="btn btn-primary btn-sm" type="button" @click="toggleDelete(row)">恢复</button>
                    </template>
                    <template v-else>
                      <button class="btn btn-secondary btn-sm" type="button" @click="openEdit(row)">编辑</button>
                      <button class="icon-btn" style="width:30px;height:30px" type="button" aria-label="更多操作"
                              @click.stop="toggleRowMenu($event, row)">
                        <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor"><circle cx="5" cy="12" r="1.8"/><circle cx="12" cy="12" r="1.8"/><circle cx="19" cy="12" r="1.8"/></svg>
                      </button>
                    </template>
                  </div>
                </td>
              </tr>
              <tr v-if="!rows.length">
                <td :colspan="visibleColumns.length + 1">
                  <div class="empty">
                    <div class="empty-icon">👤</div>
                    <div class="empty-title">暂无用户</div>
                    <div class="empty-sub">调整搜索或筛选条件试试</div>
                  </div>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>

      <!-- 行操作菜单(固定定位) -->
      <Teleport to="body">
        <div v-if="rowMenu.uid" style="position:fixed;inset:0;z-index:890" @click="closeRowMenu"></div>
        <div v-if="rowMenu.uid" class="menu" style="position:fixed;z-index:891;min-width:190px"
             :style="{ left: rowMenu.x + 'px', top: rowMenu.y + 'px' }">
          <template v-for="row in rows.filter(r => r.uid === rowMenu.uid)" :key="row.uid">
            <button class="menu-item" type="button" @click="rowMenu.uid = null; openRole(row)">提权 / 降权</button>
            <button class="menu-item" type="button" @click="rowMenu.uid = null; openPerm(row)">模块授权</button>
            <button class="menu-item" type="button" @click="rowMenu.uid = null; openReset(row)">强制重置密码</button>
            <div class="menu-sep"></div>
            <button class="menu-item" type="button" @click="toggleStatus(row)">
              {{ Number(row.status) === 2 ? '启用' : '停用' }}
            </button>
            <button class="menu-item danger" type="button" @click="toggleDelete(row)">删除(软删除)</button>
          </template>
        </div>
      </Teleport>

      <!-- 分页 -->
      <div class="pager">
        <span class="pg-info">共 <b class="num">{{ total }}</b> 名用户 · 第 <b class="num">{{ query.page }} / {{ totalPages }}</b> 页</span>
        <div class="pg-btns">
          <button class="pg-btn" type="button" :disabled="query.page <= 1" @click="pageOf(query.page - 1)">‹</button>
          <button class="pg-btn" type="button" :disabled="query.page >= totalPages" @click="pageOf(query.page + 1)">›</button>
        </div>
      </div>
    </div>

    <!-- 编辑抽屉:基本信息(列元数据驱动,置灰规则) + 角色变更(等级压制) + 模块授权(单人覆盖) -->
    <Teleport to="body">
      <Transition name="drawer">
        <div v-if="dlg.show && dlg.kind !== 'reset'" class="drawer-mask" @click="dlg.show = false"></div>
      </Transition>
      <Transition name="drawer">
        <aside v-if="dlg.show && dlg.kind !== 'reset'" class="edit-drawer" aria-hidden="false">
          <div class="drawer-head">
            <div>
              <b style="font-size:16px">编辑用户 · {{ dlg.user ? (dlg.user.nickname || dlg.user.account) : '' }}</b>
              <div class="num" style="font-size:12px;color:var(--color-text-3)">UID {{ dlg.user ? dlg.user.uid : '' }} · {{ dlg.user ? dlg.user.account : '' }}</div>
            </div>
            <button class="icon-btn" type="button" aria-label="关闭" @click="dlg.show = false">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18M6 6l12 12"/></svg>
            </button>
          </div>

          <div class="drawer-body">
            <!-- 节一:基本信息(账号置灰只读;敏感列不下发) -->
            <section class="drawer-sec">
              <h4>基本信息 <span class="sec-tag">PATCH /admin/users/{uid} · 白名单列</span></h4>
              <div class="form-item">
                <label class="form-label">账号</label>
                <input class="input" :value="dlg.user ? dlg.user.account : ''" readonly />
                <span class="form-hint">置灰:注册后不可修改</span>
              </div>
              <div class="form-item">
                <label class="form-label">昵称</label>
                <input v-model.trim="editForm.nickname" class="input" />
              </div>
              <div class="form-item">
                <label class="form-label">邮箱</label>
                <input v-model.trim="editForm.email" class="input" placeholder="可空" />
              </div>
              <div class="form-item">
                <label class="form-label">手机号</label>
                <input v-model.trim="editForm.phone" class="input" placeholder="可空" />
              </div>
              <div class="form-item">
                <label class="form-label">状态</label>
                <select v-model="editForm.status" class="input">
                  <option value="1">正常</option>
                  <option value="2">停用</option>
                </select>
              </div>
            </section>

            <!-- 节二:角色变更(等级压制) -->
            <section class="drawer-sec">
              <h4>角色变更 <span class="sec-tag">POST /role · 等级压制</span></h4>
              <div class="form-item">
                <select v-model="roleSel" class="input">
                  <option value="developer">developer(开发者, LV3)</option>
                  <option value="admin">admin(管理员, LV2)</option>
                  <option value="user">user(用户, LV1)</option>
                </select>
              </div>
              <div class="sec-hint">
                <span aria-hidden="true">ℹ</span>
                <span>仅允许操作等级低于自己的用户;提升最高到操作者的下一级;不可操作自己。developer 受最高等级天然保护。</span>
              </div>
              <button class="btn btn-secondary btn-sm" type="button" style="margin-top:10px" :disabled="busy" @click="saveRole">更新角色</button>
            </section>

            <!-- 节三:模块授权(单人覆盖) -->
            <section class="drawer-sec">
              <h4>模块授权 <span class="sec-tag">POST /permissions · 单人覆盖</span></h4>
              <div class="form-item">
                <label class="form-label">权限点(模块)</label>
                <select v-model="permSel.permCode" class="input">
                  <option v-for="p in permList" :key="p.code" :value="p.code">
                    {{ p.name }}({{ p.code }}){{ p.enabled ? '' : ' · 已停用' }}
                  </option>
                </select>
              </div>
              <div class="form-item">
                <label class="form-label">动作</label>
                <select v-model="permSel.grantType" class="input">
                  <option :value="1">授予(白名单追加)</option>
                  <option :value="2">拒绝(覆盖角色默认,优先级最高)</option>
                </select>
              </div>
              <button class="btn btn-secondary btn-sm" type="button" :disabled="busy" @click="savePerm">应用授权</button>
            </section>
          </div>

          <div class="drawer-foot">
            <button class="btn btn-ghost" type="button" @click="dlg.show = false">取消</button>
            <button class="btn btn-grad" type="button" :disabled="busy" @click="saveEdit">保存修改</button>
          </div>
        </aside>
      </Transition>
    </Teleport>

    <!-- 强制重置密码 -->
    <Modal :show="dlg.show && dlg.kind === 'reset'" title="强制重置密码" @close="dlg.show = false">
      <p class="dlg-tip">将生成一次性临时密码并置强制改密,同时吊销该用户全部会话;该用户下次登录后须设置新密码。</p>
      <button v-if="!tempPassword" class="btn btn-danger btn-block" type="button" :disabled="busy" @click="doReset">
        生成临时密码
      </button>
      <div v-else class="temp-box">
        <div class="temp-label">临时密码(仅本次显示,请转交用户)</div>
        <div class="temp-row">
          <code class="temp-value">{{ tempPassword }}</code>
          <button class="btn btn-secondary btn-sm" type="button" @click="copyTmp">{{ copied ? '已复制 ✓' : '复制' }}</button>
        </div>
        <div class="temp-warn">⚠ 关闭后不可再查看,请立即通过安全渠道转交</div>
      </div>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="dlg.show = false">关闭</button>
      </template>
    </Modal>

    <!-- 危险操作确认 -->
    <Modal :show="confirmBox.show" :title="confirmBox.title" @close="confirmBox.show = false">
      <p class="dlg-tip" style="margin:0">{{ confirmBox.msg }}</p>
      <template #foot>
        <button class="btn btn-ghost" type="button" @click="confirmBox.show = false">取消</button>
        <button class="btn btn-danger" type="button" @click="runConfirm">确认执行</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.page-sub{color:var(--color-text-2);font-size:var(--fs-body);margin:6px 0 0}
.underline-tabs{display:flex;gap:8px;border-bottom:1px solid var(--color-border)}
.utab{
  display:inline-flex;align-items:center;gap:6px;
  padding:10px 16px;margin-bottom:-1px;
  font-weight:600;font-size:var(--fs-body);color:var(--color-text-2);
  border-bottom:2px solid transparent;
  transition:color var(--dur),border-color var(--dur);
}
.utab:hover{color:var(--color-text)}
.utab.on{color:var(--color-primary);border-color:var(--color-primary)}
.utab.todo{color:var(--color-text-3)}
.seg-filter{padding:3px;gap:2px}
.seg-filter .seg-item{height:30px;font-size:var(--fs-cap);flex:none;padding:0 12px}
.switch-row{display:inline-flex;align-items:center;gap:8px;font-size:var(--fs-cap);color:var(--color-text-2);user-select:none}
.temp-box{text-align:center;padding:18px;border-radius:var(--r-md);background:var(--color-warn-bg);border:1.5px dashed var(--color-warn)}
.temp-label{color:var(--color-text-2);font-size:var(--fs-cap)}
.temp-row{display:flex;align-items:center;justify-content:center;gap:10px;margin-top:10px}
.temp-value{font-family:Consolas,monospace;font-size:20px;font-weight:700;letter-spacing:1px;color:var(--color-text)}
.temp-warn{font-size:var(--fs-cap);color:var(--color-warn);margin-top:10px}

/* 编辑抽屉(右侧滑出,设计稿 05-user-manager) */
.drawer-mask{position:fixed;inset:0;z-index:1090;background:var(--color-scrim)}
.edit-drawer{
  position:fixed;top:0;right:0;bottom:0;z-index:1091;width:440px;max-width:94vw;
  background:var(--color-card);box-shadow:var(--shadow-lg);
  display:flex;flex-direction:column;
}
.drawer-head{display:flex;align-items:center;justify-content:space-between;padding:20px 24px;border-bottom:1px solid var(--color-border-soft)}
.drawer-body{flex:1;overflow-y:auto;padding:24px}
.drawer-foot{display:flex;gap:12px;justify-content:flex-end;padding:16px 24px;border-top:1px solid var(--color-border-soft)}
.drawer-sec{margin-bottom:28px}
.drawer-sec>h4{font-size:var(--fs-body);font-weight:700;margin-bottom:12px;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.sec-tag{font-size:var(--fs-cap);font-weight:600;color:var(--color-text-3)}
.sec-hint{
  display:flex;gap:8px;align-items:flex-start;padding:9px 12px;border-radius:var(--r-sm);
  background:var(--color-border-soft);font-size:var(--fs-cap);color:var(--color-text-2);
}
.drawer-enter-active,.drawer-leave-active{transition:opacity var(--dur-slow) var(--ease)}
.drawer-enter-active .edit-drawer,.drawer-leave-active .edit-drawer{transition:transform var(--dur-slow) var(--ease)}
.drawer-enter-from,.drawer-leave-to{opacity:0}
.drawer-enter-from .edit-drawer,.drawer-leave-to .edit-drawer{transform:translateX(105%)}
@media (max-width:768px){
  .edit-drawer{width:100vw;max-width:100vw}
}
</style>
