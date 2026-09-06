import { api } from './client'

// /portal/* 模块清单/主页
export const portalApi = {
  modules: () => api.get('/portal/modules'),
  home: () => api.get('/portal/home')
}
