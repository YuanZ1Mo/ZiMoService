// /filehub/* 接口封装(照 api/admin.js 写法)
// ⚠ USE_MOCK = true 时走内存 mock(api/filehub-mock.js,响应结构严格照需求 §6);
//   服务端联调就绪后改为 false 即切真实 HTTP,界面与队列逻辑零改动。
import { api } from './client'
import { mock, mockMe } from './filehub-mock'

export const USE_MOCK = true

// 查询参数序列化(GET)
function qs(params = {}) {
  const q = new URLSearchParams()
  Object.entries(params).forEach(([k, v]) => {
    if (v !== undefined && v !== null && v !== '') q.set(k, v)
  })
  const s = q.toString()
  return s ? '?' + s : ''
}

// ── 上传引擎:单请求(≤8MB)/ 分片(8MB,并发 3,断点续传,§3.8) ──
// 真实实现要点:元数据走请求头、体为原始字节(非 multipart,§6.5)
const CHUNK_SIZE = 8 * 1024 * 1024
const sleep = (ms) => new Promise(r => setTimeout(r, ms))

async function mockUpload(file, { space, dirId, conflict, signal, onProgress }) {
  // mock:按 8MB 粒度推进,服务端逐片"接收"
  const total = file.size
  const chunks = Math.max(1, Math.ceil(total / CHUNK_SIZE))
  if (total <= CHUNK_SIZE) {
    for (let p = 0; p <= 100; p += 20) {
      if (signal?.aborted) throw new DOMException('aborted', 'AbortError')
      onProgress(Math.min(100, p) / 100 * total)
      await sleep(90)
    }
  } else {
    for (let i = 0; i < chunks; i++) {
      if (signal?.aborted) throw new DOMException('aborted', 'AbortError')
      await sleep(180 + Math.random() * 220)
      onProgress(Math.min(total, (i + 1) * CHUNK_SIZE))
    }
  }
  const r = await mock.uploadSimple({ space, dir_id: dirId, name: file.name, size: file.size, conflict })
  onProgress(total)
  return r
}

async function realUpload(file, { space, dirId, conflict, signal, onProgress, onSession }) {
  // 单请求:POST /filehub/upload/simple(头传元数据,体为原始字节)
  if (file.size <= CHUNK_SIZE) {
    const r = await rawRequest('/filehub/upload/simple', {
      method: 'POST',
      headers: { 'X-Space': String(space), 'X-Dir-Id': String(dirId), 'X-File-Name': encodeURIComponent(file.name), 'X-Conflict': conflict },
      body: file,
      signal
    })
    onProgress(file.size)
    return r
  }
  // 分片:init → 逐片(并发 3,可断点续传)→ complete
  let init
  try {
    init = await api.post('/filehub/upload/init', { space, dir_id: dirId, name: file.name, size: file.size, conflict })
  } catch (e) {
    if (e.code === 'NAME_EXISTS' || e.code === 'QUOTA_EXCEEDED' || e.code === 'TOO_MANY_UPLOADS') throw e
    throw e
  }
  if (init.instant) { onProgress(file.size); return { node_id: init.node_id, task_no: init.task_no } }
  if (onSession) onSession(init.upload_id)
  const uploaded = new Set(init.uploaded || [])
  let doneBytes = uploaded.size * init.chunk_size
  const report = () => onProgress(Math.min(file.size, doneBytes))
  const indices = []
  for (let i = 0; i < init.chunk_total; i++) if (!uploaded.has(i)) indices.push(i)
  let cursor = 0
  const worker = async () => {
    while (cursor < indices.length) {
      if (signal?.aborted) throw new DOMException('aborted', 'AbortError')
      const idx = indices[cursor++]
      const start = idx * init.chunk_size
      const blob = file.slice(start, Math.min(file.size, start + init.chunk_size))
      await rawRequest('/filehub/upload/chunk', {
        method: 'PUT',
        headers: { 'X-Upload-Id': init.upload_id, 'X-Chunk-Index': String(idx) },
        body: blob,
        signal
      })
      doneBytes += blob.size
      report()
    }
  }
  await Promise.all([worker(), worker(), worker()])
  return api.post('/filehub/upload/complete', { upload_id: init.upload_id })
}

// 原始请求(上传专用:非 JSON 体;401 仍走统一跳登录逻辑 —— 复用 client 语义的精简版)
async function rawRequest(path, { method, headers, body, signal }) {
  const resp = await fetch(`${location.protocol}//${location.hostname}:39441/zimo/api${path}`, {
    method, credentials: 'include', headers, body, signal
  })
  if (resp.status === 401) { location.href = '/login?redirect=' + encodeURIComponent(location.pathname); throw new Error('会话已失效') }
  let data = null
  try { data = await resp.json() } catch { /* 空响应 */ }
  if (!resp.ok) {
    const err = new Error((data && data.message) || `请求失败(${resp.status})`)
    err.status = resp.status; err.code = data && data.code; err.data = data
    throw err
  }
  return data
}

// 下载:换令牌后跳直链(mock 下为 blob 占位链接,用 a[download] 保住文件名)
export function downloadByUrl(url, filename) {
  if (url.startsWith('blob:')) {
    const a = document.createElement('a')
    a.href = url; a.download = filename || 'download'
    document.body.appendChild(a); a.click(); a.remove()
    return
  }
  location.href = url
}

// ── API 集合(路径与需求 §6 一一对应) ──
export const filehubApi = {
  // 空间与浏览
  spaces: () => USE_MOCK ? mock.spaces() : api.get('/filehub/spaces'),
  list: (p) => USE_MOCK ? mock.list(p) : api.get('/filehub/list' + qs(p)),
  search: (p) => USE_MOCK ? mock.search(p) : api.get('/filehub/search' + qs(p)),
  node: (id) => USE_MOCK ? mock.node(id) : api.get(`/filehub/node/${id}`),
  stat: (space, ids) => USE_MOCK ? mock.stat({ space, ids }) : api.post('/filehub/stat', { space, ids }),
  // 目录与文件
  createDir: (space, parent_id, name) => USE_MOCK ? mock.createDir({ space, parent_id, name }) : api.post('/filehub/dirs', { space, parent_id, name }),
  // 批量建目录树(文件夹上传先建层级,响应 map: "a/b" → 目录 id)
  ensureBatch: (space, parent_id, paths) => USE_MOCK ? Promise.resolve({ created: 0, reused: 0, roots: {}, map: {} }) : api.post('/filehub/dirs/ensure_batch', { space, parent_id, paths }),
  rename: (id, name) => USE_MOCK ? mock.rename(id, { name }) : api.patch(`/filehub/nodes/${id}`, { name }),
  move: (body) => USE_MOCK ? mock.move(body) : api.post('/filehub/nodes/move', body),
  copy: (body) => USE_MOCK ? mock.copy(body) : api.post('/filehub/nodes/copy', body),
  remove: (ids) => USE_MOCK ? mock.remove({ ids }) : api.post('/filehub/nodes/delete', { ids }),
  // 回收站
  trash: (p) => USE_MOCK ? mock.trash(p) : api.get('/filehub/trash' + qs(p)),
  trashRestore: (ids) => USE_MOCK ? mock.trashRestore({ ids }) : api.post('/filehub/trash/restore', { ids }),
  trashPurge: (ids) => USE_MOCK ? mock.trashPurge({ ids }) : api.post('/filehub/trash/purge', { ids }),
  trashClear: (space) => USE_MOCK ? mock.trashClear({ space }) : api.post('/filehub/trash/clear', { space }),
  // 上传(engine 由 store 驱动)
  upload: (file, opts) => USE_MOCK ? mockUpload(file, opts) : realUpload(file, opts),
  uploadCancel: (uploadId) => USE_MOCK ? mock.uploadCancel(uploadId) : api.del(`/filehub/upload/${uploadId}`),
  // 下载与打包
  downloadToken: (ids) => USE_MOCK ? mock.downloadToken({ ids }) : api.post('/filehub/download/token', { ids }),
  pack: (space, ids) => USE_MOCK ? mock.pack({ space, ids }) : api.post('/filehub/pack', { space, ids }),
  packClean: (taskNo) => USE_MOCK ? mock.packClean(taskNo) : api.post(`/filehub/pack/${taskNo}/clean`),
  // 打包产物下载:按 task_no 换直链(压缩包为缓存文件;真实实现为 POST /filehub/download/token {task_no})
  downloadPack: (taskNo) => USE_MOCK ? mock.downloadPack(taskNo) : api.post('/filehub/download/token', { task_no: taskNo }),
  // 传输任务
  tasks: (p) => USE_MOCK ? mock.tasks(p) : api.get('/filehub/tasks' + qs(p)),
  tasksActive: () => USE_MOCK ? mock.tasksActive() : api.get('/filehub/tasks/active'),
  taskCancel: (no) => USE_MOCK ? mock.taskCancel(no) : api.post(`/filehub/tasks/${no}/cancel`),
  taskRetry: (no) => USE_MOCK ? mock.taskRetry(no) : api.post(`/filehub/tasks/${no}/retry`),
  tasksClear: (status) => USE_MOCK ? mock.tasksClear({ status }) : api.post('/filehub/tasks/clear', { status }),
  // 分享(登录侧)
  shareCreate: (body) => USE_MOCK ? mock.shareCreate(body) : api.post('/filehub/shares', body),
  shareList: (p) => USE_MOCK ? mock.shareList(p) : api.get('/filehub/shares' + qs(p)),
  sharePatch: (id, body) => USE_MOCK ? mock.sharePatch(id, body) : api.patch(`/filehub/shares/${id}`, body),
  shareCancel: (id) => USE_MOCK ? mock.shareCancel(id) : api.del(`/filehub/shares/${id}`),
  shareLogs: (id, p) => USE_MOCK ? mock.shareLogs(id, p) : api.get(`/filehub/shares/${id}/logs` + qs(p)),
  // 管理端(filehubAdmin)
  admin: {
    stats: () => USE_MOCK ? mock.adminStats() : api.get('/filehub/admin/stats'),
    syncStart: (dryRun) => USE_MOCK ? mock.adminSyncStart({ dry_run: dryRun }) : api.post('/filehub/admin/sync', { dry_run: dryRun }),
    syncStatus: () => USE_MOCK ? mock.adminSyncStatus() : api.get('/filehub/admin/sync'),
    syncCancel: () => USE_MOCK ? mock.adminSyncCancel() : api.post('/filehub/admin/sync/cancel'),
    cache: (p = {}) => USE_MOCK ? mock.adminCache(p) : api.get('/filehub/admin/cache' + qs(p)),
    cacheClean: (names) => USE_MOCK ? mock.adminCacheClean({ names }) : api.post('/filehub/admin/cache/clean', { names }),
    trash: (p) => USE_MOCK ? mock.adminTrash(p) : api.get('/filehub/admin/trash' + qs(p)),
    trashRestore: (ids) => USE_MOCK ? mock.adminTrashRestore ? mock.adminTrashRestore({ ids }) : mock.trashRestore({ ids }) : api.post('/filehub/admin/trash/restore', { ids }),
    trashPurge: (ids) => USE_MOCK ? mock.trashPurge({ ids }) : api.post('/filehub/admin/trash/purge', { ids }),
    trashClean: (space) => USE_MOCK ? mock.trashClear({ space: space || 0 }) : api.post('/filehub/admin/trash/clean', { space }),
    // 整体清空:物理删除全部回收站条目(不受 30 天保留期限制)
    trashClear: (space) => USE_MOCK ? mock.trashClear({ space: space || 0 }) : api.post('/filehub/admin/trash/clear', { space }),
    tasks: (p) => USE_MOCK ? mock.adminTasks(p) : api.get('/filehub/admin/tasks' + qs(p)),
    taskCancel: (no) => USE_MOCK ? mock.taskCancel(no) : api.post(`/filehub/admin/tasks/${no}/cancel`),
    logs: (p) => USE_MOCK ? mock.adminLogs(p) : api.get('/filehub/admin/logs' + qs(p)),
    shareLogs: (p) => USE_MOCK ? mock.adminShareLogs(p) : api.get('/filehub/admin/share_logs' + qs(p)),
    spaces: (p = {}) => USE_MOCK ? mock.adminSpaces(p) : api.get('/filehub/admin/spaces' + qs(p)),
    setQuota: (space, quota) => USE_MOCK ? mock.adminSetQuota(space, { quota }) : api.patch(`/filehub/admin/spaces/${space}`, { quota })
  },
  // mock 会话上下文同步(真实模式不需要)
  _syncMe(uid, name) { if (USE_MOCK) { mockMe.uid = uid; mockMe.name = name } }
}

// 文件类型归类与图标(§3.1:服务端只回 ext,分类由前端维护)
export const FILE_KINDS = {
  dir: { label: '文件夹' },
  doc: { label: '文档', exts: ['doc', 'docx', 'pdf', 'xls', 'xlsx', 'ppt', 'pptx', 'md', 'txt'] },
  img: { label: '图片', exts: ['jpg', 'jpeg', 'png', 'gif', 'webp', 'svg', 'bmp'] },
  vid: { label: '视频', exts: ['mp4', 'mov', 'mkv', 'avi', 'webm'] },
  aud: { label: '音频', exts: ['mp3', 'flac', 'wav', 'ogg', 'm4a'] },
  zip: { label: '压缩包', exts: ['zip', '7z', 'rar', 'gz', 'tar'] },
  oth: { label: '其他', exts: [] }
}
export function kindOf(node) {
  if (Number(node.type) === 1) return 'dir'
  const e = (node.ext || '').toLowerCase()
  for (const k of ['doc', 'img', 'vid', 'aud', 'zip']) {
    if (FILE_KINDS[k].exts.includes(e)) return k
  }
  return 'oth'
}

// 人性化字节(需求 §3.1)
export function fmtSize(bytes, items) {
  if (items !== undefined && items > 0 && !bytes) return `${items} 项`
  if (!bytes) return '—'
  const u = ['B', 'KB', 'MB', 'GB', 'TB']
  let i = 0, v = Number(bytes)
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++ }
  return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${u[i]}`
}
export function fmtTime(sec) {
  if (!sec) return '—'
  const d = new Date(Number(sec) * 1000)
  const p = (n) => String(n).padStart(2, '0')
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}
