import { api } from './client'

// /admin/* 用户管理
export const adminApi = {
  users: (params = {}) => {
    const q = new URLSearchParams()
    Object.entries(params).forEach(([k, v]) => {
      if (v !== undefined && v !== null && v !== '') q.set(k, v)
    })
    const qs = q.toString()
    return api.get(`/admin/users${qs ? '?' + qs : ''}`)
  },
  columns: () => api.get('/admin/users/columns'),
  permCodes: () => api.get('/admin/users/perm-codes'),
  detail: (uid) => api.get(`/admin/users/${uid}`),
  patch: (uid, body) => api.patch(`/admin/users/${uid}`, body),
  role: (uid, roleCode) => api.post(`/admin/users/${uid}/role`, { roleCode }),
  // 模块授权:提交该用户最终应持有的权限 code 全集(服务端与角色默认做 diff)
  permissions: (uid, codes) => api.post(`/admin/users/${uid}/permissions`, { codes }),
  disable: (uid) => api.post(`/admin/users/${uid}/disable`),
  enable: (uid) => api.post(`/admin/users/${uid}/enable`),
  remove: (uid) => api.del(`/admin/users/${uid}`),
  restore: (uid) => api.post(`/admin/users/${uid}/restore`),
  resetPassword: (uid) => api.post(`/admin/users/${uid}/reset-password`)
}
