import { createRouter, createWebHistory } from 'vue-router'
import { useSessionStore } from '../stores/session'

// 通用页面(免会话白名单页 + 强制改密)
import LoginView from '../pages/LoginView.vue'
import RegisterView from '../pages/RegisterView.vue'
import ResetView from '../pages/ResetView.vue'
import ForceResetView from '../pages/ForceResetView.vue'
import NotFoundView from '../pages/NotFoundView.vue'
// 门户壳
import PortalLayout from '../layouts/PortalLayout.vue'

// 模块页面:src/modules/<kebab>/index.vue 与权限点 code 一一对应(服务端下发模块清单驱动)
const moduleViews = import.meta.glob('../modules/**/index.vue')

function kebabOf(key) {
  // ../modules/home/index.vue → home;../modules/system-manager/index.vue → system-manager
  const m = key.match(/\.\.\/modules\/([^/]+)\/index\.vue$/)
  return m ? m[1] : null
}

const moduleChildren = Object.entries(moduleViews)
  .map(([key, loader]) => {
    const name = kebabOf(key)
    if (!name) return null
    return {
      path: name, // /portal/<kebab>
      name: `portal-${name}`,
      component: loader,
      meta: { module: name }
    }
  })
  .filter(Boolean)

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/login', component: LoginView, meta: { public: true } },
    { path: '/register', component: RegisterView, meta: { public: true } },
    { path: '/reset', component: ResetView, meta: { public: true } },
    { path: '/404', component: NotFoundView, meta: { public: true } },
    { path: '/force-reset', component: ForceResetView, meta: { forceChange: true } },
    {
      path: '/portal',
      component: PortalLayout,
      redirect: '/portal/home',
      children: moduleChildren
    },
    { path: '/', redirect: '/portal' },
    { path: '/:pathMatch(.*)*', redirect: '/404' }
  ],
  scrollBehavior() {
    return { top: 0 }
  }
})

// 全局守卫:会话 / force_change / 模块权限
router.beforeEach(async (to) => {
  const session = useSessionStore()
  if (session.isLoggedIn === null) {
    // 冷启动(整页刷新/首次访问):先带 cookie 探测会话,再重走守卫;
    // 探测成功恢复用户上下文(不再退回登录页),失败则按未登录处理。
    await session.probe()
    return to.fullPath
  }
  if (to.meta.public) {
    // 已登录访问登录/注册 → 直接进门户
    if (session.isLoggedIn && (to.path === '/login' || to.path === '/register' || to.path === '/reset')) {
      return '/portal'
    }
    return true
  }
  if (!session.isLoggedIn) {
    session.redirectAfterLogin = to.fullPath
    return `/login?redirect=${encodeURIComponent(to.fullPath)}`
  }
  if (session.forceChange) {
    if (to.path !== '/force-reset') return '/force-reset'
    return true
  }
  if (to.path === '/force-reset') return '/portal'
  return true
})

export default router
