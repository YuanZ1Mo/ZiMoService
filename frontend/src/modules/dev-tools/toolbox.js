// 二期工具共用基座逻辑(设计 §10.2):复制回退、大内容降级调度
// 各工具只写自己的转换逻辑;输入节流、空闲渲染、超限暂停与复制都从这里取,
// 避免 12 份重复实现(口径见设计 §10.4 决策 28/29/30)。
import { computed, onBeforeUnmount } from 'vue'

/// 空闲渲染阈值(字符数,决策 28 口径):超过则把计算挪到空闲时段
export const IDLE_CHARS = 100 * 1024
/// 超限暂停阈值(字符数):A 组工具的转换成本远低于 Markdown 渲染,阈值放宽到 100 万
export const PAUSE_CHARS = 1024 * 1024

/**
 * 复制到剪贴板:剪贴板 API → execCommand → 选中文本(三级回退)
 *
 * @param text       要复制的文本
 * @param fallbackEl 兜底时用于 select() 的元素(textarea/input)
 * @returns 'ok'(已复制) | 'manual'(已选中,需用户手动复制) | 'fail'(无内容)
 */
export async function copyText(text, fallbackEl) {
  if (!text)
    return 'fail'
  try {
    await navigator.clipboard.writeText(text)
    return 'ok'
  } catch { /* 被拒或环境不支持:走回退 */ }
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.style.position = 'fixed'
    ta.style.opacity = '0'
    document.body.appendChild(ta)
    ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    if (ok)
      return 'ok'
  } catch { /* 继续兜底 */ }
  if (fallbackEl && typeof fallbackEl.select === 'function') {
    fallbackEl.select()
    return 'manual'
  }
  return 'fail'
}

const hasIdle = typeof window !== 'undefined' && typeof window.requestIdleCallback === 'function'

/**
 * 大内容降级调度:防抖 + (空闲渲染 / 立即执行)+ 超限暂停
 *
 * @param src           源文本 ref
 * @param opts.delay    防抖毫秒(默认 300)
 * @param opts.idleChars 超过则走 requestIdleCallback(带 timeout,不会无限等)
 * @param opts.pauseChars 超过则暂停自动计算,由调用方提供「刷新结果」手动触发 runNow
 * @returns { paused, schedule, runNow, stop }
 */
export function useHeavyInput(src, opts = {}) {
  const delay = opts.delay ?? 300
  const idleChars = opts.idleChars ?? IDLE_CHARS
  const pauseChars = opts.pauseChars ?? PAUSE_CHARS

  const paused = computed(() => src.value.length > pauseChars)
  let timer = null
  let idleHandle = null

  function stop() {
    if (timer) {
      clearTimeout(timer)
      timer = null
    }
    if (idleHandle) {
      if (hasIdle)
        window.cancelIdleCallback(idleHandle)
      else
        clearTimeout(idleHandle)
      idleHandle = null
    }
  }
  /// 排一次计算(暂停态下不排,等用户手动刷新)
  function schedule(fn) {
    stop()
    if (paused.value)
      return
    timer = setTimeout(() => {
      timer = null
      if (src.value.length > idleChars) {
        idleHandle = hasIdle
          ? window.requestIdleCallback(() => {
            idleHandle = null
            fn()
          }, { timeout: 500 })
          : setTimeout(() => {
            idleHandle = null
            fn()
          }, 0)
      } else {
        fn()
      }
    }, delay)
  }
  /// 立即算一次(手动「刷新结果」、载入示例/导入后)
  function runNow(fn) {
    stop()
    fn()
  }
  onBeforeUnmount(stop)
  return { paused, schedule, runNow, stop }
}

/// 空内容判定(全空白视为空)
export function isBlank(text) {
  return !String(text || '').trim()
}

/// 数字千分位(统计展示用)
export function fmtCount(n) {
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',')
}

/// 行数(与一期口径一致:CRLF 算一个换行;空内容 0 行)
/// 单遍扫描、零分配:状态行每次渲染都会调用它,split 会为每次按键分配整份行数组
export function lineCount(text) {
  const s = String(text || '')
  const n = s.length
  if (n === 0)
    return 0
  let lines = 1
  for (let i = 0; i < n; i++) {
    const c = s.charCodeAt(i)
    if (c === 10) {
      lines++
    } else if (c === 13) {
      lines++
      if (i + 1 < n && s.charCodeAt(i + 1) === 10)
        i++
    }
  }
  return lines
}
