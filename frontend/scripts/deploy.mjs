// deploy.mjs —— 将 dist 镜像到运行时目录(exe 同级约定),并生成 .gz 孪生
// 用法: npm run deploy  (先 npm run build)   一键: npm run release
// 行为:
//   1) 目标目录与 dist 对齐(多删少补;*.gz 孪生除外,由本脚本统一管理)
//   2) 对文本类资源生成/更新 .gz 孪生(drogon gzipStatic 前置,替代旧 tools/build_www_gzip.sh)
//      增量: .gz 已存在且不早于源文件 → 跳过;确定性输出(无文件名/时间戳头)
import { cpSync, readdirSync, rmSync, existsSync, statSync, readFileSync, writeFileSync } from 'node:fs'
import { join, dirname, extname } from 'node:path'
import { gzipSync } from 'node:zlib'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))      // frontend/scripts
const frontend = dirname(here)                            // frontend/
const dist = join(frontend, 'dist')
const target = join(frontend, '..', 'Release', 'workspace', 'frontend')

if (!existsSync(dist) || !statSync(dist).isDirectory()) {
  console.error('[deploy] dist 不存在,请先执行 npm run build')
  process.exit(1)
}
if (!existsSync(target)) {
  console.error(`[deploy] 运行时目录不存在: ${target}(请确认 exe 部署位置)`)
  process.exit(1)
}

// dist 相对路径集合(判断目标条目是否还有效)
function listFiles(root, prefix = '') {
  const set = new Set()
  for (const name of readdirSync(root)) {
    const rel = prefix ? `${prefix}/${name}` : name
    const abs = join(root, name)
    if (statSync(abs).isDirectory()) {
      set.add(rel + '/')
      for (const r of listFiles(abs, rel)) set.add(r)
    } else set.add(rel)
  }
  return set
}
const distEntries = listFiles(dist)

// ── 1) 清理:目标有而 dist 没有的条目删除(*.gz 孪生除外——其源仍在 dist 时保留,由 gzip 阶段处理) ──
function clean(dir, prefix = '') {
  for (const name of readdirSync(dir)) {
    const rel = prefix ? `${prefix}/${name}` : name
    const abs = join(dir, name)
    if (statSync(abs).isDirectory()) { clean(abs, rel); continue }
    if (distEntries.has(rel)) continue
    if (rel.endsWith('.gz') && distEntries.has(rel.slice(0, -3))) continue
    rmSync(abs, { force: true })
    console.log(`[deploy] 移除过期: ${rel}`)
  }
  // dist 中已不存在的空目录也移除
  for (const name of readdirSync(dir)) {
    const rel = `${prefix ? prefix + '/' : ''}${name}`
    const abs = join(dir, name)
    if (statSync(abs).isDirectory() && !distEntries.has(rel + '/')) {
      const rest = readdirSync(abs)
      if (!rest.length) { rmSync(abs, { recursive: true, force: true }); console.log(`[deploy] 移除空目录: ${rel}`) }
    }
  }
}
clean(target)

// ── 2) 镜像拷贝(覆盖同名) ──
cpSync(dist, target, { recursive: true })

// ── 3) 生成/更新 .gz 孪生(文本类;增量;确定性) ──
const TEXT_EXT = new Set(['.html', '.htm', '.css', '.js', '.mjs', '.json', '.xml', '.svg', '.txt', '.md', '.csv'])
let gzCount = 0, gzSkip = 0, srcTotal = 0, gzTotal = 0
function makeGz(dir, prefix = '') {
  for (const name of readdirSync(dir)) {
    const abs = join(dir, name)
    if (statSync(abs).isDirectory()) { makeGz(abs, prefix ? `${prefix}/${name}` : name); continue }
    if (name.endsWith('.gz') || !TEXT_EXT.has(extname(name))) continue
    const rel = prefix ? `${prefix}/${name}` : name
    const gzAbs = abs + '.gz'
    const srcM = statSync(abs).mtimeMs
    if (existsSync(gzAbs) && statSync(gzAbs).mtimeMs >= srcM) { gzSkip++; continue }
    const src = readFileSync(abs)
    writeFileSync(gzAbs, gzipSync(src, { level: 9 }))   // 无文件名/时间戳头,确定性输出
    gzCount++
    srcTotal += src.length
    gzTotal += statSync(gzAbs).size
  }
}
makeGz(target)

console.log(`[deploy] 完成: ${dist} → ${target}`)
if (gzCount + gzSkip > 0) {
  console.log(`[deploy] gzip 孪生: 新生成 ${gzCount} 个,跳过(已最新) ${gzSkip} 个`)
  if (srcTotal > 0) {
    const pct = ((1 - gzTotal / srcTotal) * 100).toFixed(1)
    console.log(`[deploy] 本次压缩: ${srcTotal} → ${gzTotal} 字节(节省 ${pct}%)`)
  }
}
