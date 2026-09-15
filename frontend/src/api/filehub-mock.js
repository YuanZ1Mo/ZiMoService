// 文件中心 Mock 引擎 —— 严格按《2026-09-15-文件空间业务需求》§6 响应结构实现
// 用途:服务端未就绪时支撑前端全部交互;联调时由 api/filehub.js 的 USE_MOCK 开关切换
// ⚠ 本文件即接口契约的"可执行样例":字段名/结构改动必须同步需求文档 §6

// ── 内存数据模型(字段与 filehub.db 表一致,见需求 §5) ──
const now = () => Math.floor(Date.now() / 1000)
const DAY = 86400
let seqNode = 5000
let seqTask = 100
let seqShare = 50

// 当前登录者(mock 上下文,由 filehub.js 从会话同步)
export const mockMe = { uid: 10001, name: 'mock用户' }

const spaces = [
  { space: 0, name: '公共空间', quota: 0, used_size: 92_715_212_800, used_items: 1284, create_time: now() - 90 * DAY, update_time: now() },
  { space: mockMe.uid, name: '我的空间', quota: 0, used_size: 41_033_359_360, used_items: 420, create_time: now() - 60 * DAY, update_time: now() }
]

// nodes:目录与文件同表;type 1=目录 2=文件;deleted 1=在回收站
const nodes = []
function seed(space, parent, type, name, size, ext, owner, ageDays) {
  const n = {
    id: ++seqNode, space, parent_id: parent, type, name, size,
    ext: ext || '', hash: type === 2 ? `sha256-${seqNode}` : '',
    owner_uid: owner, items: 0,
    create_time: now() - ageDays * DAY, update_time: now() - ageDays * DAY,
    deleted: 0, delete_time: 0, origin_parent_id: 0, del_owner_uid: 0
  }
  nodes.push(n)
  return n
}
function seedTree() {
  const t = now()
  void t
  // 公共空间:发布件/说明文档/素材池 + 散文件
  const pub = seed(0, 0, 1, '发布件', 0, '', 10001, 30)
  const q3 = seed(0, pub.id, 1, '2026Q3', 0, '', 10002, 12)
  seed(0, q3.id, 2, '产品发布方案 v2.docx', 395_264, 'docx', 10001, 9)
  seed(0, q3.id, 2, '发布说明 v2.1.pdf', 2_516_582, 'pdf', 10002, 8)
  const mat = seed(0, q3.id, 1, '素材池', 0, '', 10002, 10)
  seed(0, mat.id, 1, '图片', 0, '', 10003, 9)
  seed(0, mat.id, 2, '宣传片-最终版.mp4', 343_940_148, 'mp4', 10001, 7)
  seed(0, q3.id, 2, '2026Q3 复盘材料.zip', 1_288_490_188, 'zip', 10002, 6)
  seed(0, pub.id, 1, '历史归档', 0, '', 10001, 60)
  seed(0, 0, 2, '平台使用指南.pdf', 8_912_896, 'pdf', 10001, 45)
  seed(0, 0, 2, '团队合影.jpg', 8_640_307, 'jpg', 10003, 20)
  seed(0, 0, 2, 'BGM-定稿.flac', 44_040_192, 'flac', 10003, 15)
  // 我的空间
  const me = spaces[1].space
  const mine = seed(me, 0, 1, '工作文件', 0, '', me, 25)
  seed(me, mine.id, 2, '周报-0912.docx', 86_016, 'docx', me, 4)
  seed(me, mine.id, 2, '面试笔记.md', 12_288, 'md', me, 3)
  const pic = seed(me, 0, 1, '相册', 0, '', me, 40)
  seed(me, pic.id, 2, '旅行-01.jpg', 4_301_068, 'jpg', me, 38)
  seed(me, pic.id, 2, '旅行-02.jpg', 5_242_880, 'jpg', me, 38)
  seed(me, 0, 2, '开发环境配置.exe', 96_525_312, 'exe', me, 2)
  // 回收站(我删的)
  const tr1 = seed(0, 0, 2, '宣传片-初稿.mp4', 1_932_735_283, 'mp4', 10001, 50)
  tr1.deleted = 1; tr1.delete_time = now() - DAY; tr1.origin_parent_id = mat.id; tr1.del_owner_uid = mockMe.uid
  const tr2 = seed(0, 0, 1, '临时-素材整理', 0, '', 10001, 55)
  tr2.items = 64
  tr2.deleted = 1; tr2.delete_time = now() - 2 * DAY; tr2.origin_parent_id = 0; tr2.del_owner_uid = mockMe.uid
  // 他人删的(用户侧不可见)
  const tr3 = seed(0, 0, 2, '他人删除-占位.txt', 2048, 'txt', 10002, 40)
  tr3.deleted = 1; tr3.delete_time = now() - 5 * DAY; tr3.origin_parent_id = 0; tr3.del_owner_uid = 10002
  recount()
}
function recount() {
  spaces.forEach(s => {
    const live = nodes.filter(n => n.space === s.space && !isHiddenByAncestor(n))
    s.used_items = live.length
    s.used_size = live.reduce((a, n) => a + n.size, 0)
  })
  nodes.filter(n => n.type === 1 && !n.deleted).forEach(n => {
    n.items = nodes.filter(c => c.parent_id === n.id && c.space === n.space && !c.deleted).length
  })
}
// 祖先链上是否有软删(可见树判定,§2.4)
function isHiddenByAncestor(n) {
  if (n.deleted) return true
  if (!n.parent_id) return false
  const p = nodes.find(x => x.id === n.parent_id && x.space === n.space)
  return p ? isHiddenByAncestor(p) : false
}
function pathOf(n) {
  const parts = []
  let cur = n
  while (cur && cur.parent_id) { parts.unshift(cur.name); cur = nodes.find(x => x.id === cur.parent_id && x.space === cur.space) }
  return parts.join('/')
}
function breadcrumbOf(n) {
  const arr = []
  let cur = n
  while (cur) { arr.unshift({ id: cur.id, name: cur.name }); cur = cur.parent_id ? nodes.find(x => x.id === cur.parent_id && x.space === cur.space) : null }
  return arr
}
const dirOf = (space, dirId) => dirId === 0 ? null : nodes.find(x => x.id === dirId && x.space === space && x.type === 1 && !x.deleted)
const nodeOf = id => nodes.find(x => x.id === Number(id))

// ── 通用校验(需求 §4.1 精简) ──
const BAD_NAME = /[<>:"/\\|?*\x00-\x1f]/
const RESERVED = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$/i
function nameValid(name) {
  return !!name && name.length <= 255 && !BAD_NAME.test(name) && !RESERVED.test(name)
      && !name.endsWith(' ') && !name.endsWith('.') && name !== '.' && name !== '..'
}
function conflictIn(space, parentId, name, excludeId) {
  return nodes.find(x => x.space === space && x.parent_id === parentId && x.deleted === 0
      && x.id !== excludeId && x.name.toLowerCase() === name.toLowerCase())
}
const enc = new TextEncoder()
const nameBytes = s => enc.encode(s).length
const deepCount = (n) => {
  // 子树统计(条目数/字节数),供审计与打包预估
  let items = 0, bytes = 0
  const walk = (id) => {
    nodes.filter(c => c.parent_id === id && c.space === n.space && !c.deleted).forEach(c => {
      items++; bytes += c.size; if (c.type === 1) walk(c.id)
    })
  }
  if (n.type === 1) walk(n.id)
  return { items, bytes }
}

// ── 传输任务(内存队列 + 进度推进) ──
const tasks = []
const timers = new Map()
export const TASK_TYPE = { 1: '上传', 2: '复制', 3: '打包下载', 4: '目录统计', 5: '一致性同步', 6: '回收站清理' }
export const TASK_STATUS = { 1: '排队中', 2: '进行中', 3: '已完成', 4: '失败', 5: '已取消', 6: '已中断' }
const rand = (n) => { const s = 'abcdefghijklmnopqrstuvwxyz0123456789'; let r = ''; for (let i = 0; i < n; i++) r += s[Math.floor(Math.random() * s.length)]; return r }
function newTask(type, name, target, size, totalItems) {
  const task = {
    task_no: 't_' + rand(24), type, uid: mockMe.uid, space: 0, name, target,
    size: size || 0, done_size: 0, total_items: totalItems || 0, done_items: 0,
    status: 1, error: '', result: '', ref_id: '',
    create_time: now(), start_time: 0, end_time: 0
  }
  tasks.unshift(task)
  return task
}
function runTask(task, msTotal, onDone) {
  task.status = 2; task.start_time = now()
  const t0 = Date.now()
  const timer = setInterval(() => {
    const p = Math.min(1, (Date.now() - t0) / msTotal)
    task.done_size = Math.round(task.size * p)
    task.done_items = Math.round(task.total_items * p)
    if (p >= 1) {
      clearInterval(timer); timers.delete(task.task_no)
      task.status = 3; task.end_time = now()
      if (onDone) onDone(task)
    }
  }, 400)
  timers.set(task.task_no, timer)
  return task
}
function taskView(t) {
  return {
    task_no: t.task_no, type: t.type, type_name: TASK_TYPE[t.type], uid: t.uid,
    space: t.space, name: t.name, target: t.target, size: t.size, done_size: t.done_size,
    total_items: t.total_items, done_items: t.done_items, status: t.status,
    status_name: TASK_STATUS[t.status], error: t.error, result: t.result,
    create_time: t.create_time, start_time: t.start_time, end_time: t.end_time
  }
}

// ── 分享(内存) ──
const shares = []
const shareLogs = []
function newShareLog(shareId, token, action, result, detail) {
  shareLogs.unshift({
    id: ++seqShare, share_id: shareId, token, action, result,
    uid: 0, node_id: 0, ip: '192.168.3.' + (10 + Math.floor(Math.random() * 40)),
    ua: 'Mozilla/5.0 (mock)', detail: detail || '', create_time: now()
  })
}
seedTree()

// ══════════ Mock 接口实现(签名与真实 HTTP 一一对应) ══════════
const sleep = (ms) => new Promise(r => setTimeout(r, ms))
const j = async (v, ms) => { await sleep(ms === undefined ? 140 + Math.random() * 120 : ms); return typeof v === 'function' ? v() : v }
const err = (code, message, status) => { const e = new Error(message); e.code = code; e.status = status; e.data = { code, message }; throw e }

export const mock = {
  // ── 空间与浏览(§6.2) ──
  async spaces() {
    return j(spaces.map(s => ({
      space: s.space, name: s.name, quota: s.quota, used_size: s.used_size, used_items: s.used_items,
      my_trash_size: nodes.filter(n => n.space === s.space && n.deleted === 1 && n.del_owner_uid === mockMe.uid).reduce((a, n) => a + n.size, 0),
      my_trash_items: nodes.filter(n => n.space === s.space && n.deleted === 1 && n.del_owner_uid === mockMe.uid).length
    })))
  },
  async list(params) {
    const { space, dir_id = 0, sort = 'name', order = 'asc', page = 1, size = 200, type, mtime_from, mtime_to } = params
    const dir = dirOf(space, dir_id)
    if (dir_id !== 0 && !dir) return j(() => err('DIR_NOT_FOUND', '目录不存在', 404))
    let list = nodes.filter(n => n.space === Number(space) && n.parent_id === Number(dir_id) && !n.deleted)
    if (type) list = list.filter(n => fileKind(n) === type)
    if (mtime_from) list = list.filter(n => n.update_time >= Number(mtime_from))
    if (mtime_to) list = list.filter(n => n.update_time <= Number(mtime_to))
    const cmp = {
      name: (a, b) => a.name.localeCompare(b.name, 'zh-Hans-CN'),
      size: (a, b) => a.size - b.size,
      mtime: (a, b) => a.update_time - b.update_time,
      type: (a, b) => (a.ext || '').localeCompare(b.ext || '')
    }[sort] || cmp.name
    list.sort((a, b) => (a.type - b.type) || (order === 'desc' ? -cmp(a, b) : cmp(a, b)))
    const total = list.length
    const pageList = list.slice((page - 1) * size, page * size).map(n => nodeView(n))
    const bc = dir ? breadcrumbOf(dir) : []
    return j({ total, page: Number(page), size: Number(size), list: pageList, breadcrumb: bc, space: Number(space) })
  },
  async search(params) {
    const { space, dir_id = 0, keyword, sort = '', order = 'desc', page = 1, size = 200 } = params
    const kw = String(keyword || '').trim().toLowerCase()
    if (!kw || kw.length > 64) return j({ total: 0, page: 1, size, truncated: false, list: [] })
    const root = dirOf(space, Number(dir_id))
    const ids = new Set()
    const walk = (pid, depth) => nodes.filter(n => n.space === Number(space) && n.parent_id === pid && !n.deleted)
      .forEach(n => { n.__depth = depth; ids.add(n.id); if (n.type === 1) walk(n.id, depth + 1) })
    walk(Number(dir_id), 0)
    let hits = nodes.filter(n => ids.has(n.id) && n.name.toLowerCase().includes(kw))
    if (sort === 'name') hits.sort((a, b) => cmpName(a, b))
    else if (sort === 'size') hits.sort((a, b) => order === 'desc' ? b.size - a.size : a.size - b.size)
    else if (sort === 'mtime') hits.sort((a, b) => order === 'desc' ? b.update_time - a.update_time : a.update_time - b.update_time)
    else hits.sort((a, b) => (a.__depth - b.__depth) || (b.update_time - a.update_time))
    const truncated = hits.length > 500
    if (truncated) hits = hits.slice(0, 500)
    const total = hits.length
    const list = hits.slice((page - 1) * size, page * size).map(n => ({ ...nodeView(n), path: pathOf(n) }))
    return j({ total, page: Number(page), size: Number(size), truncated, list })
  },
  async node(id) {
    const n = nodeOf(id)
    if (!n || n.deleted || isHiddenByAncestor(n)) return j(() => err('NODE_NOT_FOUND', '条目不存在', 404))
    return j({ ...nodeView(n), path: pathOf(n) })
  },
  async stat(params) {
    const ids = params.ids || []
    const items = ids.reduce((a, id) => { const n = nodeOf(id); return n ? a + 1 + (n.type === 1 ? deepCount(n).items : 0) : a }, 0)
    const bytes = ids.reduce((a, id) => { const n = nodeOf(id); return n ? a + n.size + (n.type === 1 ? deepCount(n).bytes : 0) : a }, 0)
    const task = newTask(4, `统计 ${ids.length} 项`, '', bytes, items)
    runTask(task, 2500, (t) => { t.result = JSON.stringify({ items: t.total_items, bytes: t.size }) })
    return j({ task_no: task.task_no })
  },

  // ── 目录与文件操作(§6.3) ──
  async createDir(body) {
    const { space, parent_id = 0, name } = body
    if (!nameValid(name)) return j(() => err('NAME_INVALID', '名称含非法字符或超长', 400))
    if (conflictIn(space, parent_id, name)) return j(() => err('NAME_EXISTS', '同名文件已存在', 409))
    const dir = dirOf(space, parent_id)
    if (parent_id !== 0 && !dir) return j(() => err('DIR_NOT_FOUND', '目录不存在', 404))
    const n = seed(Number(space), Number(parent_id), 1, name, 0, '', mockMe.uid, 0)
    recount(); touchDir(space, parent_id)
    return j({ id: n.id, name: n.name, create_time: n.create_time })
  },
  async rename(id, body) {
    const n = nodeOf(id)
    if (!n || n.deleted || isHiddenByAncestor(n)) return j(() => err('NODE_NOT_FOUND', '条目不存在', 404))
    const { name } = body
    if (!nameValid(name)) return j(() => err('NAME_INVALID', '名称含非法字符或超长', 400))
    if (conflictIn(n.space, n.parent_id, name, n.id)) return j(() => err('NAME_EXISTS', '同名文件已存在', 409))
    n.name = name; n.update_time = now(); touchDir(n.space, n.parent_id)
    return j({})
  },
  async move(body) {
    return j(() => doMoveCopy(body, 'move'))
  },
  async copy(body) {
    return j(() => doMoveCopy(body, 'copy'))
  },
  async remove(body) {
    const ids = body.ids || []
    const success = [], failed = []
    ids.forEach(id => {
      const n = nodeOf(id)
      if (!n || n.deleted || isHiddenByAncestor(n)) { failed.push({ id, name: n ? n.name : id, code: 'NODE_NOT_FOUND' }); return }
      if (n.deleted) return   // 重复删除静默跳过(不覆盖归属)
      n.deleted = 1; n.delete_time = now(); n.origin_parent_id = n.parent_id; n.del_owner_uid = mockMe.uid
      success.push(n.id); touchDir(n.space, n.parent_id)
    })
    recount()
    return j({ success, failed, count: success.length })
  },

  // ── 回收站(§6.4) ──
  async trash(params) {
    const { space, sort = 'mtime', order = 'desc', page = 1, size = 200 } = params
    let list = nodes.filter(n => n.deleted === 1 && n.del_owner_uid === mockMe.uid && (space === undefined || space === '' || n.space === Number(space)))
    list.sort((a, b) => order === 'desc' ? b.delete_time - a.delete_time : a.delete_time - b.delete_time)
    if (sort === 'name') list.sort((a, b) => cmpName(a, b))
    const total = list.length
    return j({
      total, page: Number(page), size: Number(size),
      list: list.slice((page - 1) * size, page * size).map(trashView),
      used_size: list.reduce((a, n) => a + n.size, 0), used_items: total, retain_days: 30
    })
  },
  async trashRestore(body) {
    const ids = body.ids || []
    const success = [], failed = [], restored = []
    ids.forEach(id => {
      const n = nodeOf(id)
      if (!n || n.deleted !== 1 || n.del_owner_uid !== mockMe.uid) { failed.push({ id, name: n ? n.name : id, code: 'TRASH_ITEM_NOT_FOUND' }); return }
      let parentId = n.origin_parent_id
      let name = n.name
      const parentAlive = parentId === 0 || (nodes.find(x => x.id === parentId && x.space === n.space && !x.deleted))
      if (!parentAlive) parentId = 0
      let dup = conflictIn(n.space, parentId, name)
      let i = 1
      while (dup) { name = `${n.name} (${i++})`; dup = conflictIn(n.space, parentId, name) }
      n.deleted = 0; n.parent_id = parentId; n.name = name
      n.delete_time = 0; n.origin_parent_id = 0; n.del_owner_uid = 0
      success.push(n.id)
      restored.push({ id: n.id, name: n.name, path: pathOf(n) })
    })
    recount()
    return j({ success, failed, restored })
  },
  async trashPurge(body) {
    const ids = body.ids || []
    const success = [], failed = []
    ids.forEach(id => {
      const n = nodeOf(id)
      if (!n || n.deleted !== 1 || n.del_owner_uid !== mockMe.uid) { failed.push({ id, name: n ? n.name : id, code: 'TRASH_ITEM_NOT_FOUND' }); return }
      purgeNode(n); success.push(n.id)
    })
    recount()
    return j({ success, failed })
  },
  async trashClear(body) {
    const space = Number(body.space)
    const mine = nodes.filter(n => n.deleted === 1 && n.del_owner_uid === mockMe.uid && n.space === space)
    const bytes = mine.reduce((a, n) => a + n.size, 0)
    const task = newTask(6, '清空回收站', space === 0 ? '公共空间' : '我的空间', bytes, mine.length)
    runTask(task, 4000, () => { mine.forEach(n => purgeNode(n)); recount() })
    return j({ task_no: task.task_no })
  },

  // ── 上传(§6.5;mock 不落盘,直接登记节点) ──
  async uploadSimple(up) {
    const { space, dir_id = 0, name, size, conflict = 'ask' } = up
    return j(() => finishUpload(space, dir_id, name, size, conflict))
  },
  async uploadInit(body) {
    const { space, dir_id = 0, name, size, hash = '', conflict = 'ask' } = body
    const dup = nodes.find(x => x.space === Number(space) && x.type === 2 && !x.deleted && x.hash && x.hash === hash && x.size === size)
    if (dup) {
      const r = finishUpload(space, dir_id, name, size, conflict, dup)
      return j({ upload_id: 'instant_' + rand(16), chunk_size: 8 * 1024 * 1024, chunk_total: 0, uploaded: [], instant: true, node_id: r.node_id, task_no: r.task_no, expire_time: now() + DAY })
    }
    let sess = uploadSessions.find(s => s.uid === mockMe.uid && s.space === Number(space) && s.parent_id === Number(dir_id)
      && s.name.toLowerCase() === name.toLowerCase() && s.status === 1)
    if (!sess) {
      sess = { upload_id: 'up_' + rand(20), uid: mockMe.uid, space: Number(space), parent_id: Number(dir_id), name, size, file_hash: hash, chunk_size: 8 * 1024 * 1024, chunk_total: Math.ceil(size / (8 * 1024 * 1024)), chunks: new Set(), status: 1, create_time: now(), expire_time: now() + DAY }
      uploadSessions.push(sess)
    }
    return j({ upload_id: sess.upload_id, chunk_size: sess.chunk_size, chunk_total: sess.chunk_total, uploaded: [...sess.chunks], expire_time: sess.expire_time })
  },
  async uploadChunk(body) {
    const sess = uploadSessions.find(s => s.upload_id === body.upload_id && s.status === 1)
    if (!sess) return j(() => err('UPLOAD_NOT_FOUND', '上传会话不存在或已过期', 404))
    sess.chunks.add(Number(body.chunk_index))
    return j({ received: true, chunk_done: sess.chunks.size, chunk_total: sess.chunk_total })
  },
  async uploadComplete(body) {
    const sess = uploadSessions.find(s => s.upload_id === body.upload_id && s.status === 1)
    if (!sess) return j(() => err('UPLOAD_NOT_FOUND', '上传会话不存在或已过期', 404))
    return j(() => {
      const r = finishUpload(sess.space, sess.parent_id, sess.name, sess.size, sess.conflict || 'ask')
      sess.status = 2
      return r
    })
  },
  async uploadCancel(uploadId) {
    const sess = uploadSessions.find(s => s.upload_id === uploadId)
    if (sess) sess.status = 3
    return j({})
  },
  async uploadStatus(uploadId) {
    const sess = uploadSessions.find(s => s.upload_id === uploadId)
    if (!sess) return j(() => err('UPLOAD_NOT_FOUND', '上传会话不存在或已过期', 404))
    return j({ uploaded: [...sess.chunks], chunk_total: sess.chunk_total, expire_time: sess.expire_time })
  },

  // ── 下载与打包(§6.6) ──
  async downloadToken(body) {
    const ids = body.ids || []
    if (ids.length === 1) {
      const n = nodeOf(ids[0])
      if (!n || n.type !== 2 || n.deleted || isHiddenByAncestor(n)) return j(() => err('FILE_NOT_FOUND', '文件不存在', 404))
      return j({ url: mockBlobUrl(n.name, n.size), expire_time: now() + 600 })
    }
    return j(() => mock.pack({ space: nodes.find(x => x.id === Number(ids[0])).space, ids }))
  },
  async pack(body) {
    const ids = body.ids || []
    let items = 0, bytes = 0
    const names = []
    ids.forEach(id => {
      const n = nodeOf(id); if (!n) return
      names.push(n.name)
      if (n.type === 1) { const d = deepCount(n); items += 1 + d.items; bytes += d.bytes }
      else { items += 1; bytes += n.size }
    })
    if (items > 5000 || bytes > 20 * 1024 ** 3) return err('PACK_TOO_LARGE', '打包超出条目数/大小上限,请分批', 400)
    const zipName = `${ids.length === 1 ? names[0] : '打包下载'}_${rand(6)}.zip`
    const task = newTask(3, zipName, names.join('、'), bytes, items)
    runTask(task, 5000 + Math.min(8000, bytes / 1024 / 1024 * 8), (t) => {
      const prod = { name: zipName, size: Math.round(bytes * 0.7), owner: mockMe.name, create_time: now(), access_time: now(), task_no: t.task_no }
      cacheFiles.unshift(prod)
      t.result = zipName
    })
    return j({ task_no: task.task_no })
  },
  async packClean(taskNo) {
    const i = cacheFiles.findIndex(c => c.task_no === taskNo)
    if (i >= 0) cacheFiles.splice(i, 1)
    return j({})
  },
  // 打包产物下载:按 task_no 换取直链(压缩包为缓存文件,非 nodes 条目)
  async downloadPack(taskNo) {
    const c = cacheFiles.find(x => x.task_no === taskNo)
    if (!c) return j(() => err('NODE_NOT_FOUND', '压缩包不存在或已清理', 404))
    c.access_time = now()
    return j({ url: mockBlobUrl(c.name, c.size), expire_time: now() + 600 })
  },

  // ── 传输任务(§6.7) ──
  async tasks(params) {
    const { type, status, page = 1, size = 20 } = params
    let list = tasks
    if (type) list = list.filter(t => t.type === Number(type))
    if (status) list = list.filter(t => t.status === Number(status))
    const total = list.length
    return j({ total, page: Number(page), size: Number(size), list: list.slice((page - 1) * size, page * size).map(taskView) })
  },
  async tasksActive() {
    return j(tasks.filter(t => t.status === 1 || t.status === 2).map(taskView), 60)
  },
  async taskDetail(taskNo) {
    const t = tasks.find(x => x.task_no === taskNo)
    if (!t) return j(() => err('NODE_NOT_FOUND', '任务不存在', 404))
    return j(taskView(t))
  },
  async taskCancel(taskNo) {
    const t = tasks.find(x => x.task_no === taskNo)
    if (t && (t.status === 1 || t.status === 2)) {
      const timer = timers.get(taskNo); if (timer) { clearInterval(timer); timers.delete(taskNo) }
      t.status = 5; t.end_time = now()
    }
    return j({})
  },
  async taskRetry(taskNo) {
    const t = tasks.find(x => x.task_no === taskNo)
    if (!t) return j(() => err('NODE_NOT_FOUND', '任务不存在', 404))
    t.status = 1; t.error = ''; t.done_size = 0; t.done_items = 0
    runTask(t, 3000)
    return j({ task_no: t.task_no })
  },
  async tasksClear(body) {
    const status = body && body.status
    let cleared = 0
    for (let i = tasks.length - 1; i >= 0; i--) {
      const t = tasks[i]
      if (t.status >= 3 && (!status || t.status === Number(status))) { tasks.splice(i, 1); cleared++ }
    }
    return j({ cleared })
  },

  // ── 分享(§6.8 登录侧) ──
  async shareCreate(body) {
    const n = nodeOf(body.node_id)
    if (!n || n.deleted || isHiddenByAncestor(n)) return j(() => err('NODE_NOT_FOUND', '条目不存在', 404))
    if (shares.filter(s => s.uid === mockMe.uid && s.status === 1).length >= 200) return j(() => err('TOO_MANY_SHARES', '有效分享数已达上限 200', 429))
    const share = {
      id: ++seqShare, token: rand(32), uid: mockMe.uid, space: n.space, node_id: n.id,
      node_type: n.type, name: n.name,
      pwd_hash: body.pwd_enabled ? 'hmac:' + rand(16) : '', pwd_plain: body.pwd_enabled ? genPwd() : '',
      login_only: body.login_only ? 1 : 0,
      expire_time: body.expire_days > 0 ? now() + body.expire_days * DAY : 0,
      max_downloads: body.max_downloads || 0, download_count: 0, view_count: 0,
      status: 1, create_time: now(), update_time: now()
    }
    shares.unshift(share)
    return j({ share_id: share.id, token: share.token, url: shareUrl(share.token), pwd: share.pwd_plain || undefined, expire_time: share.expire_time })
  },
  async shareList(params) {
    const { status, page = 1, size = 50 } = params
    let list = shares.filter(s => s.uid === mockMe.uid)
    if (status) list = list.filter(s => s.status === Number(status))
    list.forEach(s => { if (s.status === 1 && s.expire_time && s.expire_time < now()) s.status = 3 })
    const total = list.length
    return j({ total, list: list.slice((page - 1) * size, page * size).map(shareView) })
  },
  async sharePatch(id, body) {
    const s = shares.find(x => x.id === Number(id) && x.uid === mockMe.uid)
    if (!s) return j(() => err('SHARE_NOT_FOUND', '分享不存在', 404))
    if (body.expire_days !== undefined) s.expire_time = body.expire_days > 0 ? now() + body.expire_days * DAY : 0
    if (body.max_downloads !== undefined) s.max_downloads = body.max_downloads
    if (body.login_only !== undefined) s.login_only = body.login_only ? 1 : 0
    let pwd
    if (body.reset_pwd) { s.pwd_hash = 'hmac:' + rand(16); s.pwd_plain = genPwd(); pwd = s.pwd_plain }
    s.update_time = now()
    return j(pwd ? { pwd } : {})
  },
  async shareCancel(id) {
    const s = shares.find(x => x.id === Number(id) && x.uid === mockMe.uid)
    if (s) { s.status = 2; s.update_time = now() }
    return j({})
  },
  async shareLogs(id, params) {
    const { page = 1, size = 20 } = params
    const list = shareLogs.filter(l => l.share_id === Number(id))
    return j({ total: list.length, list: list.slice((page - 1) * size, page * size).map(l => ({ ...l, ip: maskIp(l.ip), ua: l.ua.slice(0, 24) + '…' })) })
  },

  // ── 分享公开面(§6.9,免会话) ──
  async shareInfo(token) {
    const s = shares.find(x => x.token === token)
    if (!s) return j(() => err('SHARE_NOT_FOUND', '分享不存在或 token 非法', 404))
    if (s.status === 2) return j(() => err('SHARE_EXPIRED', '分享已被取消', 410))
    if (s.status === 3 || (s.expire_time && s.expire_time < now()) || (s.max_downloads && s.download_count >= s.max_downloads))
      return j(() => err('SHARE_EXPIRED', '分享已失效', 410))
    s.view_count++
    newShareLog(s.id, token, 1, 1)
    return j({
      name: s.name, node_type: s.node_type, need_pwd: !!s.pwd_hash, expired: false,
      expire_time: s.expire_time, owner_name: '张三', login_only: !!s.login_only, need_login: false
    })
  },
  async shareVerify(token, body) {
    const s = shares.find(x => x.token === token)
    if (!s) return j(() => err('SHARE_NOT_FOUND', '分享不存在或 token 非法', 404))
    const pass = s.pwd_plain && body.pwd === s.pwd_plain
    newShareLog(s.id, token, 2, pass ? 1 : 2, pass ? '' : '提取码错误')
    if (!pass) return j({ pass: false })
    s.__cred = 'cred_' + rand(24)
    return j({ pass: true, cred: s.__cred })
  },
  async shareListDir(token, params) {
    const { dir_id = 0, sort = 'name', order = 'asc', page = 1, size = 200 } = params
    const s = shares.find(x => x.token === token)
    if (!s) return j(() => err('SHARE_NOT_FOUND', '分享不存在或 token 非法', 404))
    const root = nodeOf(s.node_id)
    if (!root) return j(() => err('SHARE_UNAVAILABLE', '分享内容暂不可用(回收站中)', 404))
    let baseId = root.id
    let bc = [{ id: 0, name: root.name }]
    if (dir_id) {
      const d = nodes.find(x => x.id === Number(dir_id))
      const inTree = (n) => { while (n) { if (n.id === root.id) return true; n = nodes.find(x => x.id === n.parent_id) } return false }
      if (!d || !inTree(d)) return j(() => err('NODE_NOT_FOUND', '目录不在分享范围内', 404))
      baseId = d.id; bc = breadcrumbOf(d)
    }
    let list = nodes.filter(n => n.parent_id === baseId && !n.deleted)
    const cmpn = { name: (a, b) => cmpName(a, b), size: (a, b) => a.size - b.size, mtime: (a, b) => a.update_time - b.update_time }[sort] || cmpName
    list.sort((a, b) => (a.type - b.type) || (order === 'desc' ? -cmpn(a, b) : cmpn(a, b)))
    const total = list.length
    return j({ total, list: list.slice((page - 1) * size, page * size).map(n => nodeView(n)), breadcrumb: bc })
  },
  async shareDownload(token, body) {
    const s = shares.find(x => x.token === token)
    if (!s) return j(() => err('SHARE_NOT_FOUND', '分享不存在或 token 非法', 404))
    const ids = body.ids || []
    if (ids.length === 1) {
      const n = nodeOf(ids[0])
      if (!n) return j(() => err('FILE_NOT_FOUND', '文件不存在', 404))
      s.download_count++
      newShareLog(s.id, token, 4, 1)
      return j({ url: mockBlobUrl(n.name, n.size), expire_time: now() + 600 })
    }
    const items = ids.reduce((a, id) => { const n = nodeOf(id); return n ? a + 1 + (n.type === 1 ? deepCount(n).items : 0) : a }, 0)
    const task = newTask(3, `${s.name}.zip`, '分享打包', ids.reduce((a, id) => { const n = nodeOf(id); return n ? a + n.size : a }, 0), items)
    runTask(task, 4000, (t) => { s.download_count++; newShareLog(s.id, token, 5, 1); t.result = `${s.name}.zip` })
    return j({ task_no: task.task_no })
  },

  // ── 管理端(§6.10) ──
  async adminStats() {
    const pubUsed = nodes.filter(n => n.space === 0).reduce((a, n) => a + n.size, 0)
    const pri = nodes.filter(n => n.space > 0)
    return j({
      public: { items: nodes.filter(n => n.space === 0 && !n.deleted).length, bytes: pubUsed, trash_bytes: nodes.filter(n => n.space === 0 && n.deleted).reduce((a, n) => a + n.size, 0) },
      personal: { users: 18, bytes: pri.reduce((a, n) => a + n.size, 0), top: [{ uid: 10001, name: 'mock用户', bytes: spaces[1].used_size }, { uid: 10002, name: '张三', bytes: 12_884_901_888 }] },
      trash: { items: nodes.filter(n => n.deleted).length, bytes: nodes.filter(n => n.deleted).reduce((a, n) => a + n.size, 0), expiring_7d: nodes.filter(n => n.deleted && n.delete_time < now() - 23 * DAY).length },
      cache: { bytes: cacheFiles.reduce((a, c) => a + c.size, 0), zips: cacheFiles.length, chunks: uploadSessions.filter(s => s.status === 1).length },
      share: { active: shares.filter(s => s.status === 1).length, today_views: 128, today_downloads: 46 },
      task: { running: tasks.filter(t => t.status === 2).length, today_ok: 21, today_fail: 2 },
      db_size: 96_811_008
    })
  },
  async adminSyncStart(body) {
    if (tasks.find(t => t.type === 5 && t.status === 2)) return err('SYNC_RUNNING', '已有一致性同步在运行', 409)
    const task = newTask(5, '全空间一致性同步', '公共 + 全部个人空间', 1880, 1880)
    syncLive = { scanned: 0, added: 0, removed: 0, fixed: 0, skipped: 0 }
    runTask(task, 12000, (t) => { t.result = JSON.stringify(syncLive) })
    const timer = setInterval(() => {
      syncLive.scanned = task.done_items
      syncLive.added = Math.round(task.done_items * 0.014)
      syncLive.removed = Math.round(task.done_items * 0.0025)
      syncLive.fixed = Math.round(task.done_items * 0.005)
      syncLive.skipped = Math.round(task.done_items * 0.006)
      if (task.status !== 2) clearInterval(timer)
    }, 400)
    return j({ task_no: task.task_no })
  },
  async adminSyncStatus() {
    const running = tasks.find(t => t.type === 5 && t.status === 2)
    const last = [...tasks].reverse().find(t => t.type === 5 && t.status === 3)
    return j({
      running: !!running,
      progress: running ? { task_no: running.task_no, scanned: syncLive.scanned, total: running.total_items, added: syncLive.added, removed: syncLive.removed, fixed: syncLive.fixed, skipped: syncLive.skipped, elapsed: now() - running.start_time } : null,
      last: last ? { task_no: last.task_no, end_time: last.end_time, report: JSON.parse(last.result || '{}') } : null
    })
  },
  async adminSyncCancel() {
    const t = tasks.find(x => x.type === 5 && x.status === 2)
    if (t) { const timer = timers.get(t.task_no); if (timer) clearInterval(timer); t.status = 5; t.end_time = now() }
    return j({})
  },
  async adminCache() {
    return j({
      total: cacheFiles.length,
      list: cacheFiles.map(c => ({ ...c, kind: c.name.endsWith('.zip') ? 1 : 2 }))
    })
  },
  async adminCacheClean(body) {
    const names = body.names || []
    let bytes = 0, deleted = 0
    for (let i = cacheFiles.length - 1; i >= 0; i--) {
      const c = cacheFiles[i]
      if (!names.length || names.includes(c.name)) { bytes += c.size; deleted++; cacheFiles.splice(i, 1) }
    }
    return j({ deleted, bytes })
  },
  async adminTrash(params) {
    const { page = 1, size = 50 } = params
    const list = nodes.filter(n => n.deleted === 1)
    return j({
      total: list.length,
      list: list.slice((page - 1) * size, page * size).map(n => ({ ...trashView(n), space: n.space, space_name: n.space === 0 ? '公共空间' : '用户' + n.space }))
    })
  },
  async adminTasks(params) {
    const { page = 1, size = 50 } = params
    const total = tasks.length
    return j({ total, list: tasks.slice((page - 1) * size, page * size).map(t => ({ ...taskView(t), uid: t.uid, uname: t.uid === mockMe.uid ? mockMe.name : '用户' + t.uid })) })
  },
  async adminLogs(params) {
    const { page = 1, size = 20, action } = params
    let list = fileLogs
    if (action) list = list.filter(l => l.action === action)
    const total = list.length
    return j({ total, list: list.slice((page - 1) * size, page * size) })
  },
  async adminShareLogs(params) {
    const { page = 1, size = 20 } = params
    return j({ total: shareLogs.length, list: shareLogs.slice((page - 1) * size, page * size).map(l => ({ ...l, ip: maskIp(l.ip) })) })
  },
  async adminSpaces() {
    return j({ total: spaces.length, list: spaces.map(s => ({ space: s.space, name: s.name, quota: s.quota, used_size: s.used_size, used_items: s.used_items })) })
  },
  async adminSetQuota(space, body) {
    const s = spaces.find(x => x.space === Number(space))
    if (s) s.quota = Number(body.quota) || 0
    return j({})
  }
}

// ══════════ 内部工具 ══════════
const uploadSessions = []
const cacheFiles = [
  { name: '10001_20260915_203412_a3f2k9.zip', size: 3_758_096_384, owner: 'mock用户', create_time: now() - 3600, access_time: now() - 600 },
  { name: '10002_20260915_101122_m8q1x4.zip', size: 1_073_741_824, owner: '张三', create_time: now() - 7200, access_time: now() - 1200 }
]
const fileLogs = [
  { id: 1, uid: 10001, account: 'mock用户', action: 'move', space: 0, node_id: 0, node_name: '发布会合影.jpg', detail: '{"to":"我的空间/图片"}', ip: '192.168.3.10', result: 1, create_time: now() - 1800 },
  { id: 2, uid: 10002, account: '张三', action: 'pack', space: 0, node_id: 0, node_name: '2026Q3-评审材料.zip', detail: '{"items":286,"bytes":3758096384}', ip: '192.168.3.22', result: 1, create_time: now() - 3600 },
  { id: 3, uid: 10002, account: '张三', action: 'download', space: 0, node_id: 0, node_name: '宣传片-最终版.mp4', detail: '{"range":"0-","bytes":343940148}', ip: '192.168.3.22', result: 1, create_time: now() - 5400 },
  { id: 4, uid: 10003, account: '李四', action: 'delete', space: 0, node_id: 0, node_name: '旧版素材合集', detail: '{"subtree":64,"bytes":1717986918}', ip: '10.8.0.7', result: 1, create_time: now() - 9000 },
  { id: 5, uid: 10004, account: '王五', action: 'upload', space: 1, node_id: 0, node_name: '母版.mp4', detail: '{"error":"QUOTA_EXCEEDED","chunk":8}', ip: '10.8.0.9', result: 2, create_time: now() - 12600 }
]
let syncLive = { scanned: 0, added: 0, removed: 0, fixed: 0, skipped: 0 }

function cmpName(a, b) { return a.name.localeCompare(b.name, 'zh-Hans-CN') }
function touchDir(space, parentId) {
  const p = nodes.find(x => x.id === Number(parentId) && x.space === Number(space))
  if (p) p.update_time = now()
}
function nodeView(n) {
  return {
    id: n.id, type: n.type, name: n.name, size: n.size, ext: n.ext, items: n.items,
    owner_uid: n.owner_uid, owner_name: n.owner_uid === 0 ? '系统' : (n.owner_uid === mockMe.uid ? mockMe.name : '用户' + n.owner_uid),
    create_time: n.create_time, update_time: n.update_time, hash: n.hash || undefined
  }
}
function trashView(n) {
  const bc = n.origin_parent_id ? (nodes.find(x => x.id === n.origin_parent_id && x.space === n.space && !x.deleted) ? pathOf({ ...n, id: n.origin_parent_id }) : '') : ''
  return {
    id: n.id, name: n.name, type: n.type, size: n.size, items: n.items,
    path: n.origin_parent_id === 0 ? '' : (bc || '原目录已删除'),
    delete_time: n.delete_time, del_owner_uid: n.del_owner_uid,
    del_owner_name: n.del_owner_uid === mockMe.uid ? mockMe.name : '用户' + n.del_owner_uid
  }
}
function shareView(s) {
  return {
    id: s.id, token: s.token, url: shareUrl(s.token), name: s.name, node_type: s.node_type,
    has_pwd: !!s.pwd_hash, login_only: !!s.login_only,
    expire_time: s.expire_time, max_downloads: s.max_downloads,
    download_count: s.download_count, view_count: s.view_count,
    status: s.status, status_name: { 1: '有效', 2: '已取消', 3: '已失效' }[s.status],
    create_time: s.create_time
  }
}
function shareUrl(token) { return `${location.protocol}//${location.host}/s/${token}` }
function genPwd() { const s = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789'; return Array.from({ length: 4 }, () => s[Math.floor(Math.random() * s.length)]).join('') }
function maskIp(ip) { return ip.split('.').slice(0, 3).join('.') + '.x' }
function purgeNode(n) {
  const rm = (id) => {
    nodes.filter(c => c.parent_id === id && c.space === n.space).forEach(c => rm(c.id))
    const i = nodes.findIndex(x => x.id === id)
    if (i >= 0) nodes.splice(i, 1)
  }
  rm(n.id)
}
function fileKind(n) {
  if (n.type === 1) return 'dir'
  const e = n.ext
  if (['doc', 'docx', 'pdf', 'xls', 'xlsx', 'ppt', 'pptx', 'md', 'txt'].includes(e)) return 'doc'
  if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'svg', 'bmp'].includes(e)) return 'img'
  if (['mp4', 'mov', 'mkv', 'avi', 'webm'].includes(e)) return 'vid'
  if (['mp3', 'flac', 'wav', 'ogg', 'm4a'].includes(e)) return 'aud'
  if (['zip', '7z', 'rar', 'gz', 'tar'].includes(e)) return 'zip'
  return 'oth'
}
// 上传入位(mock:裁决 + 建 nodes 行)
function finishUpload(space, dirId, name, size, conflict, instantSrc) {
  space = Number(space); dirId = Number(dirId)
  const dup = conflictIn(space, dirId, name)
  let finalName = name
  if (dup) {
    if (conflict === 'skip') return { node_id: 0, task_no: '' }
    if (conflict === 'rename') { let i = 1; while (conflictIn(space, dirId, finalName)) finalName = dupName(name, i++) }
    else if (conflict === 'overwrite' && dup.type === 2) { removeNode(dup) }
    else err('NAME_EXISTS', '同名文件已存在', 409)
  }
  const n = seed(space, dirId, 2, finalName, size, extOf(finalName), mockMe.uid, 0)
  n.hash = instantSrc ? instantSrc.hash : 'sha256-' + rand(16)
  recount(); touchDir(space, dirId)
  const task = newTask(1, finalName, '', size, 1)
  task.status = 3; task.done_size = size; task.done_items = 1; task.start_time = task.create_time; task.end_time = now()
  return { node_id: n.id, task_no: task.task_no }
}
function dupName(name, i) {
  const dot = name.lastIndexOf('.')
  return dot > 0 ? `${name.slice(0, dot)} (${i})${name.slice(dot)}` : `${name} (${i})`
}
function removeNode(n) {
  const i = nodes.findIndex(x => x.id === n.id)
  if (i >= 0) nodes.splice(i, 1)
}
function extOf(name) { const i = name.lastIndexOf('.'); return i > 0 ? name.slice(i + 1).toLowerCase() : '' }
// mock 下载:生成占位文本 Blob 直链(真实实现为 /filehub/dl/{token}/{filename})
function mockBlobUrl(name, size) {
  const head = `ZiMo 文件中心 mock 下载\n文件名: ${name}\n声明大小: ${size} 字节\n(服务端联调后此链接将替换为 /filehub/dl/{token}/{filename} 直链)\n`
  return URL.createObjectURL(new Blob([head], { type: 'application/octet-stream' }))
}
// 移动/复制共通(整批裁决 → 逐条执行,§3.5/§3.6)
function doMoveCopy(body, mode) {
  const { ids = [], target_space, target_dir_id = 0, conflict = 'ask' } = body
  const tSpace = Number(target_space)
  if (tSpace !== 0 && tSpace !== mockMe.uid) err('PERM_DENIED', '无目标空间写权限', 403)
  if (target_dir_id !== 0 && !dirOf(tSpace, target_dir_id)) err('DIR_NOT_FOUND', '目标目录不存在', 404)
  const tops = ids.map(nodeOf).filter(n => n && !n.deleted && !isHiddenByAncestor(n))
  if (!tops.length) err('NODE_NOT_FOUND', '条目不存在', 404)
  // 环路检测:目标在任一待移动目录子树内
  const inSubtree = (dirId, rootId) => {
    let cur = nodes.find(x => x.id === dirId)
    while (cur) { if (cur.id === rootId) return true; cur = nodes.find(x => x.id === cur.parent_id && x.space === cur.space) }
    return false
  }
  for (const n of tops) {
    if (n.type === 1 && n.space === tSpace && inSubtree(Number(target_dir_id), n.id))
      err('MOVE_INTO_SELF', '目标目录不能是自身或其子孙', 400)
    if (n.id === Number(target_dir_id)) err('MOVE_INTO_SELF', '目标目录不能是自身', 400)
  }
  // 冲突裁决(整批)
  const conflicts = []
  for (const n of tops) {
    const dup = conflictIn(tSpace, Number(target_dir_id), n.name)
    if (dup) conflicts.push({ id: n.id, name: n.name, why: '目标已存在同名' + (dup.type === 1 ? '文件夹' : '文件') })
  }
  if (conflicts.length && conflict === 'ask') { const e = err('NAME_EXISTS', '存在同名冲突,请选择处理策略', 409); e.data.conflicts = conflicts; throw e }
  // 执行(逐条;复制超阈值转任务 —— mock 简化为同步 + 任务记录)
  const ok = [], skipped = [], failed = []
  for (const n of tops) {
    const dup = conflictIn(tSpace, Number(target_dir_id), n.name)
    if (dup) {
      if (conflict === 'skip') { skipped.push(n.id); continue }
      if (conflict === 'rename') {
        let finalName = n.name, i = 1
        while (conflictIn(tSpace, Number(target_dir_id), finalName)) finalName = dupName(n.name, i++)
        if (mode === 'move') moveNode(n, tSpace, Number(target_dir_id), finalName)
        else copyNode(n, tSpace, Number(target_dir_id), finalName)
        ok.push(n.id); continue
      }
      if (conflict === 'overwrite') {
        if (dup.type === 2 && n.type === 2) { removeNode(dup) } else { skipped.push(n.id); continue }
      }
    }
    if (mode === 'move') moveNode(n, tSpace, Number(target_dir_id), n.name)
    else copyNode(n, tSpace, Number(target_dir_id), n.name)
    ok.push(n.id)
  }
  recount()
  return mode === 'move' ? { moved: ok, skipped } : { copied: ok, skipped, failed, task_no: undefined }
}
function moveNode(n, tSpace, tDir, name) {
  touchDir(n.space, n.parent_id)
  const changeSpace = (id) => nodes.filter(c => c.parent_id === id && c.space === n.space).forEach(c => { c.space = tSpace; changeSpace(c.id) })
  n.space = tSpace; n.parent_id = tDir; n.name = name; n.update_time = now()
  changeSpace(n.id)   // 子孙只改 space(§3.5)
  touchDir(tSpace, tDir)
}
function copyNode(n, tSpace, tDir, name) {
  const clone = (src, parentId) => {
    const c = { ...src, id: ++seqNode, space: tSpace, parent_id: parentId, name: src === n ? name : src.name, create_time: now(), update_time: now(), deleted: 0, delete_time: 0, origin_parent_id: 0, del_owner_uid: 0 }
    nodes.push(c)
    nodes.filter(x => x.parent_id === src.id && x.space === src.space).forEach(x => clone(x, c.id))
    return c
  }
  clone(n, tDir)
  touchDir(tSpace, tDir)
}
