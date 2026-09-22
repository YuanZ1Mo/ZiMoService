<script setup>
// 小工具模块 /portal/dev-tools(模块 code=devTools,index=4)
// 容器:顶部 tab 切换工具 + 服务端 check 把关(首次挂载判定,切回复检)
// 工具全部在浏览器本地处理:除 check 复检外不再向服务端发请求(设计 §3.2)
import { ref, computed, onMounted, onActivated } from 'vue'
import { useSessionStore } from '../../stores/session'
import { devToolsApi } from '../../api/devTools'
import JsonFormat from './JsonFormat.vue'
import MarkdownEditor from './MarkdownEditor.vue'
import './dev-tools.css'

// 门户壳会向模块页透传 :home(本模块用不到,声明以避开落在根元素上的 fallthrough 属性)
defineProps({ home: { type: Object, default: null } })

const session = useSessionStore()

const TOOLS = [
  { key: 'json', name: 'JSON 格式化', comp: JsonFormat },
  { key: 'markdown', name: 'Markdown 编辑器', comp: MarkdownEditor }
]
const cur = ref('json')
const curComp = computed(() => (TOOLS.find(t => t.key === cur.value) || TOOLS[0]).comp)

// 准入状态:loading(首帧骨架) | ok(挂载工具) | denied(无权限占位) | error(检查失败可重试)
const gate = ref('loading')
// keep-alive 下首次挂载也会触发 onActivated:跳过首次,避免重复请求(设计 §3.2)
let firstActivate = true

// 本地权限快照只作否定快速路径:确定无权限 → 直接占位,不等 check;
// 有权限时不提前挂载(统一等 check 200),避免"旧快照先挂载、check 回来再卸载"的闪动
function localNoPerm() {
  const ps = session.user && session.user.permissions
  return Array.isArray(ps) && !ps.includes('devTools')
}

async function ensureAccess(silent) {
  // 本地快照只用来"先画占位、少闪一下":仍要发请求求证 —— 否则管理员事后授予权限时,
  // 本地 permissions(登录时下发)还是旧的,页面会一直卡在占位
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

onMounted(() => { ensureAccess(false) })
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
      <p class="page-sub">JSON 格式化与 Markdown 编辑 · 内容全部在浏览器本地处理,不上传服务器</p>
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
      <div class="underline-tabs">
        <button v-for="t in TOOLS" :key="t.key" type="button" class="utab"
                :class="{ on: cur === t.key }" @click="cur = t.key">{{ t.name }}</button>
      </div>
      <div class="dt-body">
        <keep-alive>
          <component :is="curComp" />
        </keep-alive>
      </div>
    </template>
  </div>
</template>
