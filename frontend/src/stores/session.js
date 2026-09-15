import { defineStore } from 'pinia'

// 会话 / 用户上下文 / force_change 状态
// isLoggedIn 三态:null=未探测(冷启动/整页刷新后) / true=已登录 / false=未登录
export const useSessionStore = defineStore('session', {
  state: () => ({
    user: null,          // {uid, account, nickname, roleCode, level}
    forceChange: false,
    redirectAfterLogin: '',   // 401 时记录的回跳地址
    connected: true,     // 心跳网络状态
    checked: false       // 是否已完成会话探测
  }),
  getters: {
    isLoggedIn: (s) => (s.checked ? !!s.user : null)
  },
  actions: {
    setUser(u) {
      this.user = u
      this.forceChange = !!(u && u.forceChange)
    },
    setForceChange(v) {
      this.forceChange = !!v
    },
    clear() {
      this.user = null
      this.forceChange = false
      this.connected = true
    },
    // 冷启动会话探测:整页刷新/首次访问时带 cookie 探测 /portal/home
    // 成功 → 恢复用户上下文;失败(未登录/网络) → 视为未登录。不抛异常。
    async probe() {
      this.checked = true
      try {
        const base = `${location.protocol}//${location.hostname}:39441/zimo/api`
        const resp = await fetch(base + '/portal/home', { credentials: 'include' })
        if (!resp.ok) return false
        const data = await resp.json()
        this.user = {
          uid: data.uid,
          account: data.account,
          nickname: data.nickname,
          roleCode: data.role && data.role.code,
          level: data.role && data.role.level,
          permissions: data.permissions || []
        }
        this.forceChange = false
        return true
      } catch {
        this.user = null
        return false
      }
    }
  }
})
