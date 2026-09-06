import { api } from './client'

// /auth/* 注册/登录/登出/强制改密/心跳
export const authApi = {
  register: (payload) => api.post('/auth/register', payload),
  login: (payload) => api.post('/auth/login', payload),
  logout: () => api.post('/auth/logout'),
  forceReset: (newPassword) => api.post('/auth/force-reset', { newPassword }),
  heartbeat: () => api.post('/auth/heartbeat')
}
