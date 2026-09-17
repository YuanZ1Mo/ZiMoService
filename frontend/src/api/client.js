// fetch 封装:credentials include、401 统一跳登录(记录回跳)、错误码映射

// 业务 API 基址:页面端口(80/443)与 RESTful 端口(39441)为同站跨端口
const API_BASE = `${location.protocol}//${location.hostname}:39441/zimo/api`

const CONTENT_JSON = { 'Content-Type': 'application/json' }

function goLogin(redirect) {
  const target = redirect || location.pathname + location.search
  if (location.pathname === '/login') return
  location.href = `/login?redirect=${encodeURIComponent(target)}`
}

async function request(path, { method = 'GET', body, timeout = 15000 } = {}) {
  const ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null
  // timeout <= 0 = 不设超时:服务端少数接口是同步完成的(合并分片、批量复制、批量建目录),
  // 耗时随数据量走,固定 15 秒会在服务端已成功的情况下报"网络异常",诱发重复提交
  const timer = ctrl && timeout > 0 ? setTimeout(() => ctrl.abort(), timeout) : null
  let resp
  try {
    resp = await fetch(API_BASE + path, {
      method,
      credentials: 'include',
      headers: body !== undefined ? CONTENT_JSON : undefined,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      signal: ctrl ? ctrl.signal : undefined
    })
  } catch (e) {
    if (timer) clearTimeout(timer)
    const err = new Error('网络异常,请检查连接')
    err.network = true
    throw err
  }
  if (timer) clearTimeout(timer)

  let data = null
  try {
    data = await resp.json()
  } catch {
    /* 非 JSON 响应 */
  }

  if (resp.status === 401) {
    // 会话失效:记录回跳地址 → 跳登录
    const err = new Error((data && data.message) || '会话已失效,请重新登录')
    err.status = 401
    err.code = data && data.code
    goLogin()
    throw err
  }
  if (resp.status === 403 && data && data.code === 'FORCE_CHANGE_REQUIRED') {
    location.href = '/force-reset'
    const err = new Error(data.message || '需要强制重置密码')
    err.status = 403
    err.code = data.code
    throw err
  }
  if (!resp.ok) {
    const err = new Error((data && data.message) || `请求失败(${resp.status})`)
    err.status = resp.status
    err.code = data && data.code
    err.data = data
    throw err
  }
  return data
}

export const api = {
  get: (path, opts) => request(path, opts),
  post: (path, body, opts) => request(path, { method: 'POST', body, ...opts }),
  patch: (path, body, opts) => request(path, { method: 'PATCH', body, ...opts }),
  del: (path, opts) => request(path, { method: 'DELETE', ...opts }),
  request
}
