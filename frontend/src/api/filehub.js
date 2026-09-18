// /filehub/* 接口封装(照 api/admin.js 写法)
// 全部走真实 HTTP:上传的元数据走请求头、体为原始字节(§6.5)
import { api } from './client'

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

// 同步型慢接口的超时(毫秒;服务端在这些接口里把活干完才回响应,给 15 秒会在
// 服务端已成功时报"网络异常",用户重试就会重复复制/重复建目录)
const TIMEOUT_MERGE = 10 * 60 * 1000    // 分片合并:20GB 上限,按磁盘速度走
const TIMEOUT_COPY = 5 * 60 * 1000      // 同步复制:≤500 项且 ≤1GB 才走同步分支
const TIMEOUT_ENSURE = 2 * 60 * 1000    // 批量建目录:单批 ≤2000 条路径

// 秒传/校验用整文件哈希的上限:Web Crypto 没有流式接口,要整份读进内存,
// 超过这个体积就放弃算哈希(不上传秒传,服务端也不做校验,上传照常)
const HASH_MAX_BYTES = 256 * 1024 * 1024

/**
 * 算整文件的 SHA-256(小写十六进制,与服务端 ToHex 口径一致)
 *
 * 拿不到哈希(浏览器不支持 / 超过体积上限 / 读取失败)一律返回空串 —— 服务端把空
 * hash 当作"不校验、不比秒传",不影响上传本身。
 *
 * @param file  待上传的文件
 * @return 64 字符十六进制摘要;空串 = 本次不做哈希
 */
async function fileSha256(file) {
  if (!file || file.size > HASH_MAX_BYTES) return ''
  if (!globalThis.crypto || !globalThis.crypto.subtle) return ''
  try {
    const md = await globalThis.crypto.subtle.digest('SHA-256', await file.arrayBuffer())
    return [...new Uint8Array(md)].map(b => b.toString(16).padStart(2, '0')).join('')
  } catch { return '' }
}

async function uploadFile(file, { space, dirId, conflict, signal, onProgress, onSession }) {
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
  // 带整文件哈希:服务端据此判秒传(同空间同 hash+size 直接入位),并在合并后校验一致
  const hash = await fileSha256(file)
  const initBody = { space, dir_id: dirId, name: file.name, size: file.size, conflict }
  if (hash) initBody.hash = hash
  const init = await api.post('/filehub/upload/init', initBody)
  // init 的同名预检:ask 时提前失败,让用户先选策略,而不是把整个文件传完再撞 409
  if (init.conflicts && init.conflicts.length && conflict === 'ask') {
    const err = new Error('同名文件已存在,请选择处理方式')
    err.code = 'NAME_EXISTS'
    err.data = { code: 'NAME_EXISTS', conflicts: init.conflicts }
    throw err
  }
  if (init.instant) { onProgress(file.size); return { node_id: init.node_id, task_no: init.task_no, instant: true } }
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
  return api.post('/filehub/upload/complete', { upload_id: init.upload_id },
                  { timeout: TIMEOUT_MERGE })
}

// 原始请求(上传专用:非 JSON 体;401 仍走统一跳登录逻辑 —— 复用 client 语义的精简版)
async function rawRequest(path, { method, headers, body, signal }) {
  const resp = await fetch(`${location.protocol}//${location.hostname}:39441/zimo/api${path}`, {
    method, credentials: 'include', headers, body, signal
  })
  // 与 client.js 同一套会话语义:401 带查询串回跳、403 强制改密跳转
  if (resp.status === 401) {
    const back = location.pathname + location.search
    location.href = '/login?redirect=' + encodeURIComponent(back)
    throw new Error('会话已失效')
  }
  let data = null
  try { data = await resp.json() } catch { /* 空响应 */ }
  if (resp.status === 403 && data && data.code === 'FORCE_CHANGE_REQUIRED') {
    location.href = '/force-reset'
    const err = new Error(data.message || '需要强制重置密码')
    err.status = 403; err.code = data.code
    throw err
  }
  if (!resp.ok) {
    const err = new Error((data && data.message) || `请求失败(${resp.status})`)
    err.status = resp.status; err.code = data && data.code; err.data = data
    throw err
  }
  return data
}

// 下载:换令牌后跳直链(服务端下发绝对地址,与 Cookie 无关)
export function downloadByUrl(url) {
  if (url) location.href = url
}

// ── API 集合(路径与需求 §6 一一对应) ──
export const filehubApi = {
  // 空间与浏览
  spaces: () => api.get('/filehub/spaces'),
  list: (p) => api.get('/filehub/list' + qs(p)),
  search: (p) => api.get('/filehub/search' + qs(p)),
  node: (id) => api.get(`/filehub/node/${id}`),
  stat: (space, ids) => api.post('/filehub/stat', { space, ids }),
  // 目录与文件
  createDir: (space, parent_id, name) => api.post('/filehub/dirs', { space, parent_id, name }),
  // 批量建目录树(文件夹上传先建层级,响应 map: "a/b" → 目录 id)
  ensureBatch: (space, parent_id, paths) => api.post('/filehub/dirs/ensure_batch',
                                                     { space, parent_id, paths },
                                                     { timeout: TIMEOUT_ENSURE }),
  rename: (id, name) => api.patch(`/filehub/nodes/${id}`, { name }),
  move: (body) => api.post('/filehub/nodes/move', body),
  copy: (body) => api.post('/filehub/nodes/copy', body, { timeout: TIMEOUT_COPY }),
  remove: (ids) => api.post('/filehub/nodes/delete', { ids }),
  // 回收站
  trash: (p) => api.get('/filehub/trash' + qs(p)),
  trashRestore: (ids) => api.post('/filehub/trash/restore', { ids }),
  trashPurge: (ids) => api.post('/filehub/trash/purge', { ids }),
  trashClear: (space) => api.post('/filehub/trash/clear', { space }),
  // 上传(engine 由 store 驱动)
  upload: uploadFile,
  uploadCancel: (uploadId) => api.del(`/filehub/upload/${uploadId}`),
  // 下载与打包
  downloadToken: (ids) => api.post('/filehub/download/token', { ids }),
  pack: (space, ids) => api.post('/filehub/pack', { space, ids }),
  packClean: (taskNo) => api.post(`/filehub/pack/${taskNo}/clean`),
  // 打包产物下载:按 task_no 换直链(压缩包是缓存文件,不是 nodes 条目)
  downloadPack: (taskNo) => api.post('/filehub/download/token', { task_no: taskNo }),
  // 传输任务
  tasks: (p) => api.get('/filehub/tasks' + qs(p)),
  tasksActive: () => api.get('/filehub/tasks/active'),
  taskCancel: (no) => api.post(`/filehub/tasks/${no}/cancel`),
  taskRetry: (no) => api.post(`/filehub/tasks/${no}/retry`),
  tasksClear: (status) => api.post('/filehub/tasks/clear', { status }),
  // 分享(登录侧)
  shareCreate: (body) => api.post('/filehub/shares', body),
  shareList: (p) => api.get('/filehub/shares' + qs(p)),
  sharePatch: (id, body) => api.patch(`/filehub/shares/${id}`, body),
  shareCancel: (id) => api.del(`/filehub/shares/${id}`),
  shareResume: (id) => api.post(`/filehub/shares/${id}/resume`),
  // 彻底删除分享记录:{ids:[id]} 删指定(任意状态);{inactive:true} 清空全部非有效
  sharePurge: (body) => api.post('/filehub/shares/purge', body),
  shareLogs: (id, p) => api.get(`/filehub/shares/${id}/logs` + qs(p)),
  // 分享(公开面,免会话;提取码通过后由服务端下发 zm_share 凭证 Cookie)
  shareInfo: (token) => api.get(`/filehub/share/${token}`),
  shareVerify: (token, body) => api.post(`/filehub/share/${token}/verify`, body),
  shareListDir: (token, p) => api.get(`/filehub/share/${token}/list` + qs(p)),
  shareDownload: (token, body) => api.post(`/filehub/share/${token}/download`, body),
  // 管理端(filehubAdmin)
  admin: {
    stats: () => api.get('/filehub/admin/stats'),
    syncStart: (dryRun) => api.post('/filehub/admin/sync', { dry_run: dryRun }),
    syncStatus: () => api.get('/filehub/admin/sync'),
    syncCancel: () => api.post('/filehub/admin/sync/cancel'),
    cache: (p = {}) => api.get('/filehub/admin/cache' + qs(p)),
    cacheClean: (names) => api.post('/filehub/admin/cache/clean', { names }),
    trash: (p) => api.get('/filehub/admin/trash' + qs(p)),
    trashRestore: (ids) => api.post('/filehub/admin/trash/restore', { ids }),
    trashPurge: (ids) => api.post('/filehub/admin/trash/purge', { ids }),
    trashClean: (space) => api.post('/filehub/admin/trash/clean', { space }),
    // 整体清空:物理删除全部回收站条目(不受 30 天保留期限制)
    trashClear: (space) => api.post('/filehub/admin/trash/clear', { space }),
    tasks: (p) => api.get('/filehub/admin/tasks' + qs(p)),
    taskCancel: (no) => api.post(`/filehub/admin/tasks/${no}/cancel`),
    logs: (p) => api.get('/filehub/admin/logs' + qs(p)),
    shareLogs: (p) => api.get('/filehub/admin/share_logs' + qs(p)),
    spaces: (p = {}) => api.get('/filehub/admin/spaces' + qs(p)),
    setQuota: (space, quota) => api.patch(`/filehub/admin/spaces/${space}`, { quota })
  }
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

/// @brief 取条目的类型中文名(列表"类型"列用)
export function kindLabel(node) {
  return FILE_KINDS[kindOf(node)].label
}

/**
 * @brief 计算点击某条目后的新选中集
 *
 * 语义按点击方式区分:
 *   · 复选框(alwaysToggle)= 只增删自身,不动其它选中项;
 *   · Ctrl/Cmd = 增删自身;
 *   · Shift = 从上次落点连选到本次;
 *   · 其余(单击行)= 单选并清空其它。
 *
 * @param ids      当前列表的全部 id(按显示顺序,Shift 连选要用)
 * @param selected 当前选中集
 * @param id       本次点击的条目 id
 * @param opts     { ctrl, shift, alwaysToggle, lastIdx }
 * @return { selected: 新选中集, lastIdx: 新落点下标 }
 */
export function nextSelection(ids, selected, id, opts = {}) {
  const s = new Set(selected)
  const idx = ids.indexOf(id)
  if (opts.alwaysToggle || opts.ctrl) {
    s.has(id) ? s.delete(id) : s.add(id)
    return { selected: s, lastIdx: idx }
  }
  if (opts.shift && opts.lastIdx >= 0 && idx >= 0) {
    const a = Math.min(opts.lastIdx, idx)
    const b = Math.max(opts.lastIdx, idx)
    for (let i = a; i <= b; i++) s.add(ids[i])
    return { selected: s, lastIdx: opts.lastIdx }
  }
  return { selected: new Set([id]), lastIdx: idx }
}

/**
 * @brief 按关键词把名称切成高亮片段(搜索结果里标红命中部分)
 *
 * 大小写不敏感,逐段匹配(关键词可多次出现)。
 *
 * @param name     条目名称
 * @param keyword  搜索关键词(空 → 整段不命中)
 * @return 片段数组 [{ t: 文本, hit: 是否命中 }]
 */
export function highlightText(name, keyword) {
  const text = String(name == null ? '' : name)
  const kw = String(keyword == null ? '' : keyword).trim()
  if (!kw) return [{ t: text, hit: false }]
  const segs = []
  const lower = text.toLowerCase()
  const k = kw.toLowerCase()
  let i = 0
  while (i < text.length) {
    const p = lower.indexOf(k, i)
    if (p < 0) {
      segs.push({ t: text.slice(i), hit: false })
      break
    }
    if (p > i) segs.push({ t: text.slice(i, p), hit: false })
    segs.push({ t: text.slice(p, p + k.length), hit: true })
    i = p + k.length
  }
  return segs
}

// 人性化字节(需求 §3.1)
// 0 字节按 "0 B" 展示(空文件是合法条目);目录场景由调用方传 items 走"N 项"
export function fmtSize(bytes, items) {
  if (items !== undefined && items > 0 && !bytes) return `${items} 项`
  if (bytes === undefined || bytes === null || bytes === '') return '—'
  const n = Number(bytes)
  if (!Number.isFinite(n)) return '—'
  const u = ['B', 'KB', 'MB', 'GB', 'TB']
  let i = 0, v = n
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++ }
  return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${u[i]}`
}
export function fmtTime(sec) {
  if (!sec) return '—'
  const d = new Date(Number(sec) * 1000)
  const p = (n) => String(n).padStart(2, '0')
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}
