// 字符串整理的纯变换(设计 §10.3):与组件分离,便于单测
//
// 注意换行符:HTML 规范规定 textarea 的 value 会把 CRLF/CR 归一为 LF,
// 因此"从编辑框进来的文本"永远是 LF —— 「统一为 CRLF」用于产出 Windows 风格文本
// (复制到文件里生效),「统一为 LF」对这类文本是幂等的。真实字节请以导出/复制为准。
export const STRING_OPS = [
  { key: 'upper', name: '转大写' },
  { key: 'lower', name: '转小写' },
  { key: 'title', name: '每词首字母大写' },
  { key: 'trim', name: '去行首尾空白' },
  { key: 'dedup', name: '按行去重' },
  { key: 'sortAsc', name: '行升序' },
  { key: 'sortDesc', name: '行降序' },
  { key: 'toLf', name: '统一为 LF' },
  { key: 'toCrlf', name: '统一为 CRLF' }
]

/// 按行拆分(CRLF / CR / LF 都算换行)
export function splitLines(text) {
  return String(text == null ? '' : text).split(/\r\n|\r|\n/)
}

/**
 * 执行一个字符串整理操作(纯函数,不改入参)
 *
 * @param text 原文
 * @param key  操作 key(见 STRING_OPS)
 * @returns 结果文本;未知 key 返回原文
 */
export function applyStringOp(text, key) {
  const s = String(text == null ? '' : text)
  switch (key) {
    case 'upper':
      return s.toUpperCase()
    case 'lower':
      return s.toLowerCase()
    case 'title':
      return s.replace(/[A-Za-z]+/g, w => w[0].toUpperCase() + w.slice(1).toLowerCase())
    case 'trim':
      return splitLines(s).map(l => l.trim()).join('\n')
    case 'dedup': {
      const seen = new Set()
      return splitLines(s).filter(l => {
        if (seen.has(l))
          return false
        seen.add(l)
        return true
      }).join('\n')
    }
    case 'sortAsc':
      return splitLines(s).sort((a, b) => a.localeCompare(b, 'zh-Hans-CN')).join('\n')
    case 'sortDesc':
      return splitLines(s).sort((a, b) => b.localeCompare(a, 'zh-Hans-CN')).join('\n')
    case 'toLf':
      return s.replace(/\r\n|\r/g, '\n')
    case 'toCrlf':
      return s.replace(/\r\n|\r|\n/g, '\r\n')
    default:
      return s
  }
}
