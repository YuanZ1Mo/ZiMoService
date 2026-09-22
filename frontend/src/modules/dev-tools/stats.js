// 文本统计口径(需求 §4.5 / 设计 §5.7):两个工具共用,避免口径漂移
//
//   chars  按 Unicode 码点计(含空白与换行;代理对算 1,组合 emoji 会拆成多个码点)
//   lines  按换行符切分的段数(\r\n 算一个换行);空内容为 0
//   words  中日韩汉字/假名/谚文逐字计 1,连续的拉丁字母与数字串计 1 个词,其余不计
//
// 实现是**单遍扫描、零分配**:正则 /g 的 match 会为每个命中分配一个字符串(5MB 中文约
// 260 万个),Array.from 再分配等长数组 —— 实测慢 3~20 倍。本函数在每次按键后都会重算,
// 必须足够便宜。
export function countStats(text) {
  const s = String(text == null ? '' : text)
  const n = s.length
  if (n === 0)
    return { chars: 0, lines: 0, words: 0 }

  let chars = 0
  let lines = 1
  let words = 0
  let inLatin = false
  for (let i = 0; i < n; i++) {
    const c = s.charCodeAt(i)
    // 代理对:高低位成对时算 1 个码点,跳过低位
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
      const d = s.charCodeAt(i + 1)
      if (d >= 0xDC00 && d <= 0xDFFF)
        i++
    }
    chars++
    if (c === 0x0A) {
      lines++
      inLatin = false
      continue
    }
    if (c === 0x0D) {
      lines++
      inLatin = false
      if (i + 1 < n && s.charCodeAt(i + 1) === 0x0A) {
        i++      // \r\n 只算一个换行
        chars++  // 但两个字符都要计入字符数
      }
      continue
    }
    if ((c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A)) {
      if (!inLatin) {
        words++
        inLatin = true
      }
      continue
    }
    inLatin = false
    // 中日韩汉字/假名/谚文(含扩展 A 与兼容区)逐字计 1
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF)
      || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF))
      words++
  }
  return { chars, lines, words }
}
