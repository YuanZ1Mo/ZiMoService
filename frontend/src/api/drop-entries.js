// 拖入项展开:把系统拖入的文件/文件夹整理成"待上传文件 + 各自相对路径 + 途中目录"
// 背景:拖入文件夹时浏览器只给一个目录条目(文件对象 size=0),必须用 entry API 递归读出内容,
//       否则会把文件夹当成 0 字节文件上传(层级与内容全丢)。
// 抽成独立模块是为了可单测:readEntries 每次只返回一批(约 100 个),
// **必须循环读到空数组** —— 只读一次会静默丢文件,这一点在浏览器里很难复现。
//
// 返回:{ files: File[], rels: string[], dirs: string[] }
//   rels[i] = files[i] 所在目录的相对路径(含顶层文件夹名,与 <input webkitdirectory> 的
//             webkitRelativePath 同义);dirs = 递归途中遇到的全部目录(含空目录)

/** 目录层级上限(与后端的路径深度限制对齐,防异常结构把页面拖死) */
const MAX_DEPTH = 32

/**
 * 递归读取一个拖入项
 *
 * @param entry  FileSystemEntry(文件或目录);为空直接返回
 * @param prefix 该 entry 所属目录的相对路径(顶层为空串)
 * @param out    收集结果(原地写入)
 * @returns {Promise<void>} 读完(读失败按空处理,不抛出)
 */
function readEntry(entry, prefix, out) {
  return new Promise((resolve) => {
    if (!entry) return resolve()
    if (entry.isFile) {
      entry.file((f) => {
        out.files.push(f)
        out.rels.push(prefix)
        resolve()
      }, () => resolve())
      return
    }
    if (entry.isDirectory && prefix.split('/').filter(Boolean).length < MAX_DEPTH) {
      const dir = prefix ? `${prefix}/${entry.name}` : entry.name
      out.dirs.push(dir)
      const reader = entry.createReader()
      const nextBatch = () => reader.readEntries(async (entries) => {
        if (!entries.length) return resolve()   // 读到空数组才算读完
        for (const en of entries) await readEntry(en, dir, out)
        nextBatch()
      }, () => resolve())
      nextBatch()
      return
    }
    resolve()
  })
}

/**
 * 展开一次拖放的数据
 *
 * 必须在 drop 事件处理内同步调用:webkitGetAsEntry 要在事件上下文里取,取到的 entry 可异步读。
 *
 * @param dataTransfer 事件的 dataTransfer
 * @returns {Promise<{files: File[], rels: string[], dirs: string[]}>}
 */
export async function collectDropItems(dataTransfer) {
  const out = { files: [], rels: [], dirs: [] }
  const items = dataTransfer && dataTransfer.items
  const entries = []
  if (items && items.length) {
    for (const it of items) {
      const en = it.webkitGetAsEntry ? it.webkitGetAsEntry() : null
      if (en) entries.push(en)
    }
  }
  if (entries.length) {
    for (const en of entries) await readEntry(en, '', out)
    return out
  }
  // 退化路径:浏览器不支持 entry API 时按平铺文件处理
  for (const f of [...((dataTransfer && dataTransfer.files) || [])]) {
    out.files.push(f)
    out.rels.push('')
  }
  return out
}
