import { api } from './client'

// 小工具模块(唯一接口:页面进入鉴权探测,不携带任何工具数据)
export const devToolsApi = {
  check: () => api.get('/devTools/check')
}
