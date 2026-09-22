<script setup>
// 小工具模块 /portal/dev-tools(模块 code=devTools,index=4)
// 容器:分组下拉切换工具 + 服务端 check 把关(首次挂载判定,切回复检)
// 二期:工具按需加载(切到才拉自己的 chunk)、深链接 ?tool=<key>、keep-alive 不设上限
// 工具全部在浏览器本地处理:除 check 复检外不再向服务端发请求(设计 §3.2)
import { ref, computed, defineAsyncComponent, onMounted, onActivated, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import ZmSelect from '../../components/ZmSelect.vue'
import { useSessionStore } from '../../stores/session'
import { devToolsApi } from '../../api/devTools'
import './dev-tools.css'

// 门户壳会向模块页透传 :home(本模块用不到,声明以避开落在根元素上的 fallthrough 属性)
defineProps({ home: { type: Object, default: null } })

const route = useRoute()
const router = useRouter()
const session = useSessionStore()

// 工具清单:key = 深链接参数;group 驱动分组下拉;comp 按需加载
const TOOLS = [
  { key: 'json', name: 'JSON 格式化', group: '格式化与编辑', comp: defineAsyncComponent(() => import('./JsonFormat.vue')) },
  { key: 'markdown', name: 'Markdown 编辑器', group: '格式化与编辑', comp: defineAsyncComponent(() => import('./MarkdownEditor.vue')) },
  { key: 'timestamp', name: '时间戳转换', group: '时间与编码', comp: defineAsyncComponent(() => import('./Timestamp.vue')) },
  { key: 'encode', name: '编解码', group: '时间与编码', comp: defineAsyncComponent(() => import('./EncodeDecode.vue')) },
  { key: 'generators', name: '生成器', group: '生成与计算', comp: defineAsyncComponent(() => import('./Generators.vue')) },
  { key: 'basecolor', name: '进制与颜色', group: '生成与计算', comp: defineAsyncComponent(() => import('./BaseColor.vue')) },
  { key: 'string', name: '字符串整理', group: '文本处理', comp: defineAsyncComponent(() => import('./StringKit.vue')) }
]
const options = TOOLS.map(t => ({ value: t.key, label: t.name, group: t.group }))

function validKey(k) {
  return TOOLS.some(t => t.key === k) ? k : ''
}
const cur = ref(validKey(String(route.query.tool || '')) || TOOLS[0].key)
const curComp = computed(() => (TOOLS.find(t => t.key === cur.value) || TOOLS[0]).comp)
const curName = computed(() => (TOOLS.find(t => t.key === cur.value) || TOOLS[0]).name)

/// 切换工具:同步 query(用 replace,不污染浏览器历史)
function pick(k) {
  cur.value = validKey(String(k)) || TOOLS[0].key
  router.replace({ query: { ...route.query, tool: cur.value } })
}
// 外部改 query(前进/后退、粘贴链接)时跟随
watch(() => route.query.tool, (v) => {
  const k = validKey(String(v || ''))
  if (k && k !== cur.value)
    cur.value = k
})

// 准入状态:loading(首帧骨架) | ok(挂载工具) | denied(无权限占位) | error(检查失败可重试)
const gate = ref('loading')
// keep-alive 下首次挂载也会触发 onActivated:跳过首次,避免重复请求(设计 §3.2)
let firstActivate = true

// 本地权限快照只作否定快速路径:确定无权限 → 直接占位,仍发 check 求证
// (管理员事后授权时,本地 permissions 是登录时的旧快照,不能作为硬判据)
function localNoPerm() {
  const ps = session.user && session.user.permissions
  return Array.isArray(ps) && !ps.includes('devTools')
}

async function ensureAccess(silent) {
  const quickDeny = localNoPerm()
  if (quickDeny)
    gate.value = 'denied'
  else if (!silent)
    gate.value = 'loading'
  try {
    await devToolsApi.check()
    gate.value = 'ok'
  } catch (e) {
    if (e.status === 401) return   // 会话失效:client.js 已跳登录页
    if (e.status === 403 && e.code === 'FORCE_CHANGE_REQUIRED') return   // 已跳改密页,静默忽略
    if (e.status === 403 && e.code === 'PERM_DENIED') {
      gate.value = 'denied'
      return
    }
    // 网络异常等:首次给错误态(可重试);复检保持现状,不误判为无权限
    if (!silent && !quickDeny)
      gate.value = 'error'
  }
}

onMounted(() => {
  // 非法 ?tool= 归一为合法值(否则链接看着指向某工具、实际挂的是第一个,语义不干净)
  const raw = String(route.query.tool || '')
  if (raw && !validKey(raw))
    router.replace({ query: { ...route.query, tool: TOOLS[0].key } })
  ensureAccess(false)
})
onActivated(() => {
  if (firstActivate) {
    firstActivate = false
    return
  }
  ensureAccess(true)
})
</script>

<template>
  <div class="dt-page">
    <div class="page-head">
      <h1>小工具</h1>
      <p class="page-sub">常用开发小工具 · 内容全部在浏览器本地处理,不上传服务器</p>
    </div>

    <!-- 无权限:明确提示,不挂载工具 -->
    <div v-if="gate === 'denied'" class="card work-card">
      <div class="empty">
        <div class="empty-icon">⛔</div>
        <div class="empty-title">无权限访问</div>
        <div class="empty-sub">您没有小工具模块的访问权限,请联系管理员授权</div>
      </div>
    </div>

    <!-- 鉴权检查失败:可重试(不等同于无权限) -->
    <div v-else-if="gate === 'error'" class="card work-card">
      <div class="empty">
        <div class="empty-icon">⚠</div>
        <div class="empty-title">鉴权检查失败</div>
        <div class="empty-sub">网络异常或服务暂不可用,请稍后重试</div>
        <button class="btn btn-secondary" type="button" style="margin-top:6px"
                @click="ensureAccess(false)">重试</button>
      </div>
    </div>

    <div v-else-if="gate === 'loading'" class="card work-card">
      <div class="skeleton" style="height:200px"></div>
    </div>

    <template v-else>
      <div class="dt-switch">
        <ZmSelect v-model="cur" :options="options" prefix="工具:" @change="pick" />
        <span class="dt-cap">{{ curName }} · 共 {{ TOOLS.length }} 个工具</span>
      </div>
      <div class="dt-body">
        <keep-alive>
          <component :is="curComp" />
        </keep-alive>
      </div>
    </template>
  </div>
</template>
