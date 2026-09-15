<script setup>
// 门户壳:固定顶栏(品牌/心跳胶囊/主题/用户菜单/退出) + 抽屉侧边导航 + keep-alive 工作区
// 心跳 10s:401 立即下线;网络中断提示不强制下线;响应携带 policyVersion 驱动按需拉取
import { ref, computed, onMounted, onBeforeUnmount, inject } from 'vue'
import { useRouter } from 'vue-router'
import { authApi } from '../api/auth'
import { portalApi } from '../api/portal'
import { useSessionStore } from '../stores/session'
import { useModulesStore } from '../stores/modules'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const router = useRouter()
const session = useSessionStore()
const modulesStore = useModulesStore()
const toast = inject('toast')

const sideCollapsed = ref(false)
const sideOpen = ref(false)        // 移动端抽屉
const connState = ref('on')        // on | warn | err(连续失败 ≥3 显示已断开)
const failCount = ref(0)
const home = ref(null)             // /portal/home 数据(主页模块用)
const menuOpen = ref(false)        // 用户菜单展开
const hbChipEl = ref(null)         // 心跳状态胶囊(脉冲闪动目标)
let heartbeatTimer = null

// 模块排序(§2.3 全序):正数升序在前,负数区 -1 最先依次在后
const displayModules = computed(() => {
  const list = [...modulesStore.modules]
  const pos = list.filter(m => m.index > 0).sort((a, b) => a.index - b.index)
  const neg = list.filter(m => m.index <= 0).sort((a, b) => b.index - a.index)
  return [...pos, ...neg]
})

const avatarChar = computed(() => {
  const n = session.user && (session.user.nickname || session.user.account)
  return n ? n.charAt(0).toUpperCase() : '…'
})

// 模块图标:按 code 语义映射,未知模块用通用网格图标(矢量,不用 emoji)
function icoOf(m) {
  if (m.code === 'home') {
    return '<path d="m3 10 9-7 9 7v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 22V12h6v10"/>'
  }
  if (m.code === 'systemManager') {
    return '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.9 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.9.3h.1a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5h.1a1.7 1.7 0 0 0 1.9-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.9v.1a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1Z"/>'
  }
  return '<rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><rect x="14" y="14" width="7" height="7" rx="2"/>'
}

function toggleSide() {
  if (window.innerWidth <= 768) sideOpen.value = !sideOpen.value
  else sideCollapsed.value = !sideCollapsed.value
}

async function loadHome() {
  try {
    const data = await portalApi.home()
    home.value = data
    session.setUser({
      uid: data.uid, account: data.account, nickname: data.nickname,
      roleCode: data.role && data.role.code, level: data.role && data.role.level,
      permissions: data.permissions || [],
      forceChange: session.forceChange
    })
    modulesStore.fetchModules()
  } catch (e) {
    if (e.status === 401) return
    throw e
  }
}

async function heartbeat() {
  try {
    const data = await authApi.heartbeat()
    connState.value = 'on'
    failCount.value = 0
    pulseDot()
    if (data.forceChange) {
      session.setForceChange(true)
      router.push('/force-reset')
      return
    }
    // 轮询应答:比对策略版本号,不一致按需拉取模块清单
    modulesStore.setPolicyVersion(Number(data.policyVersion || 0))
  } catch (e) {
    if (e.status === 401) return   // client.js 已跳登录
    // 网络中断/服务端异常:提示但不强制下线,恢复自动续
    failCount.value += 1
    connState.value = failCount.value >= 3 ? 'err' : 'warn'
  }
}

async function doLogout() {
  menuOpen.value = false
  try {
    await authApi.logout()
  } catch {
    /* 会话可能已失效,照常登出 */
  }
  session.clear()
  router.push('/login')
}

function goModule(m) {
  router.push(m.url || `/portal/${m.code}`)
  sideOpen.value = false
}

// 用户菜单:个人中心(本期仅预留入口)
function goProfile() {
  menuOpen.value = false
  toast('个人中心建设中', 'warn')
}

// 心跳应答时状态点单次闪动(WAAPI):无常驻 CSS 动画,避免高刷屏持续产帧拉高 CPU
function pulseDot() {
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return
  hbChipEl.value?.querySelector('.hb-dot')?.animate(
    [{ opacity: 1, transform: 'scale(1)' }, { opacity: .35, transform: 'scale(.72)' }, { opacity: 1, transform: 'scale(1)' }],
    { duration: 600, easing: 'ease-in-out' }
  )
}

// 点击页面任意处关闭用户菜单(不能靠全屏遮罩:顶栏 backdrop-filter 会使 fixed 遮罩
// 相对顶栏定位,只覆盖 60px 高的顶栏区域,点不到下方页面)
function onDocClick() {
  menuOpen.value = false
}

onMounted(async () => {
  document.addEventListener('click', onDocClick)
  try {
    await loadHome()
  } catch (e) {
    if (e.status === 401) return
    toast('加载门户数据失败', 'err')
  }
  heartbeat()
  heartbeatTimer = setInterval(heartbeat, 10000)   // 10s 心跳
})

onBeforeUnmount(() => {
  document.removeEventListener('click', onDocClick)
  if (heartbeatTimer) clearInterval(heartbeatTimer)
})
</script>

<template>
  <div class="portal">
    <!-- 顶部状态栏:固定悬浮 -->
    <header class="portal-topbar">
      <button class="icon-btn" type="button" aria-label="切换导航" @click="toggleSide">
        <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18M3 12h18M3 18h18"/></svg>
      </button>
      <div class="portal-brand"><span class="brand-logo">Z</span>ZiMo</div>

      <div class="portal-topbar-right">
        <!-- 心跳连接状态胶囊 -->
        <span ref="hbChipEl" class="hb-chip" :class="connState" :title="connState === 'on' ? '已连接' : connState === 'warn' ? '网络不稳定,自动重试中' : '连接已断开,恢复后自动续'">
          <span class="hb-dot" aria-hidden="true"></span>
          <span>{{ connState === 'on' ? '已连接' : connState === 'warn' ? '重连中…' : '已断开' }}</span>
        </span>

        <ThemeSwitch />

        <!-- 用户菜单 -->
        <div style="position:relative">
          <button class="user-chip" type="button" :title="session.user ? session.user.account : ''"
                  aria-haspopup="menu" :aria-expanded="menuOpen" @click.stop="menuOpen = !menuOpen">
            <span class="avatar">{{ avatarChar }}</span>
            <span class="uc-name">{{ session.user ? session.user.nickname || session.user.account : '…' }}</span>
            <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" style="color:var(--color-text-3)"><path d="m6 9 6 6 6-6"/></svg>
          </button>
          <div v-if="menuOpen" class="menu" style="right:0;top:calc(100% + 8px);min-width:210px" @click.stop>
            <div style="padding:10px 12px 8px">
              <div style="font-weight:700">{{ session.user ? session.user.nickname || session.user.account : '' }}</div>
              <div class="num" style="font-size:12px;color:var(--color-text-3)">
                {{ session.user ? session.user.account : '' }}<template v-if="session.user && session.user.roleCode"> · {{ session.user.roleCode }}</template>
              </div>
            </div>
            <div class="menu-sep"></div>
            <button class="menu-item" type="button" @click="goProfile">
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
              个人中心
              <span class="badge badge-dim" style="margin-left:auto">建设中</span>
            </button>
            <div class="menu-sep"></div>
            <button class="menu-item danger" type="button" @click="doLogout">
              <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9"/></svg>
              退出登录
            </button>
          </div>
        </div>

        <!-- 独立退出按钮(需求 §7.3:昵称区域右侧) -->
        <button class="icon-btn" type="button" aria-label="退出登录" title="退出登录" @click="doLogout">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9"/></svg>
        </button>
      </div>
    </header>

    <!-- 移动端抽屉遮罩 -->
    <div v-if="sideOpen" class="side-mask" @click="sideOpen = false"></div>

    <!-- 侧边导航:抽屉式,模块清单服务端下发驱动 -->
    <nav class="portal-side" :class="{ collapsed: sideCollapsed, open: sideOpen }">
      <div class="side-label-cap side-cap">功能模块</div>
      <button v-for="m in displayModules" :key="m.code" type="button" class="side-item"
              :class="{ active: $route.path === (m.url || '/portal/' + m.code) }" @click="goModule(m)">
        <span class="side-ico">
          <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" v-html="icoOf(m)"></svg>
        </span>
        <span class="side-label">{{ m.name }}</span>
      </button>
      <div class="side-foot">ZiMo Portal · v1.0</div>
    </nav>

    <!-- 工作区:keep-alive 嵌入渲染,模块切走保留运行状态 -->
    <main class="portal-main" :class="{ wide: sideCollapsed }">
      <router-view v-slot="{ Component }">
        <keep-alive>
          <component :is="Component" :home="home" />
        </keep-alive>
      </router-view>
    </main>

    <!-- 门户特效区(壳级):固定视口底部,所有模块共享,新增模块自动附带 -->
    <div class="portal-fx" :class="{ wide: sideCollapsed }" aria-hidden="true">
      <i class="pf-flow"></i>
    </div>
  </div>
</template>

<style scoped>
.user-chip{
  display:inline-flex;align-items:center;gap:9px;padding:4px 10px 4px 4px;
  border-radius:var(--r-pill);color:var(--color-text);font-weight:600;font-size:var(--fs-body);
  transition:background var(--dur);
}
.user-chip:hover{background:var(--color-border-soft)}
.uc-name{max-width:110px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.side-mask{position:fixed;inset:0;z-index:650;background:var(--color-scrim)}

/* 门户特效区(壳级):固定视口底部,所有模块共享;静态三色渐变(活力蓝→青→粉) */
.portal-fx{
  position:fixed;left:var(--sidebar-w);right:0;bottom:0;height:56px;z-index:600;
  overflow:hidden;
  background:
    linear-gradient(90deg,rgba(2,132,199,.18) 0%,rgba(6,182,212,.24) 30%,rgba(236,72,153,.18) 70%,rgba(2,132,199,.18) 100%),
    var(--color-bg);
  border-top:1px solid var(--color-border-soft);
  transition:left var(--dur-slow) var(--ease);
}
.portal-fx.wide{left:var(--sidebar-w-min)}
@media (max-width:768px){
  .portal-fx{left:0}
}
</style>

// build-probe-20260913
