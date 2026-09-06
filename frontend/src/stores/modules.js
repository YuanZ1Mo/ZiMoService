import { defineStore } from 'pinia'
import { portalApi } from '../api/portal'

// 模块清单(服务端下发,侧边栏数据源)
export const useModulesStore = defineStore('modules', {
  state: () => ({
    modules: [],       // [{code,name,url,index}]
    policyVersion: 0   // 心跳应答的策略变更版本号
  }),
  actions: {
    async fetchModules() {
      try {
        const list = await portalApi.modules()
        this.modules = Array.isArray(list) ? list : []
      } catch (e) {
        if (e.status !== 401 && e.status !== 403) throw e
      }
    },
    setPolicyVersion(v) {
      if (typeof v === 'number' && v !== this.policyVersion) {
        const changed = this.policyVersion !== 0
        this.policyVersion = v
        // 版本变化 → 按需拉取模块清单(轮询应答)
        if (changed) this.fetchModules()
      }
    }
  }
})
