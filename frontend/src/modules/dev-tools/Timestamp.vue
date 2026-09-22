<script setup>
// 时间戳转换(二期 A):Unix 秒/毫秒 ⇄ 本地/UTC 时间,多行批量、双向
// 双向输入不用 watcher(会互相触发死循环):两侧各挂 @input,由"用户改哪侧"决定换算方向
import { ref, computed, inject } from 'vue'
import ToolShell from './ToolShell.vue'
import { copyText, useHeavyInput, fmtCount, lineCount } from './toolbox'

const toast = inject('toast')

const tsText = ref('')
const dateText = ref('')
const unit = ref('auto')       // auto | s | ms
const tz = ref('local')        // local | utc
const direction = ref('ts2date')
const errMsg = ref('')
const okCount = ref(0)
const failCount = ref(0)

/// 被编辑的那一侧(大内容降级按它判长度)
const src = computed(() => (direction.value === 'ts2date' ? tsText.value : dateText.value))
const { paused, schedule, runNow } = useHeavyInput(src)

const outText = computed(() => (direction.value === 'ts2date' ? dateText.value : tsText.value))

function pad(n, w = 2) {
  return String(n).padStart(w, '0')
}
function fmtDate(ms, useUtc) {
  const d = new Date(ms)
  if (Number.isNaN(d.getTime()))
    return ''
  const g = useUtc
    ? [d.getUTCFullYear(), d.getUTCMonth() + 1, d.getUTCDate(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds()]
    : [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()]
  return `${g[0]}-${pad(g[1])}-${pad(g[2])} ${pad(g[3])}:${pad(g[4])}:${pad(g[5])}`
}
/// 时间戳 → 毫秒:auto 按位数判(≥13 位当毫秒),可手动指定
function tsToMs(text) {
  const t = text.trim()
  if (!/^-?\d+$/.test(t))
    return NaN
  const digits = t.replace(/^-/, '').length
  const u = unit.value === 'auto' ? (digits >= 13 ? 'ms' : 's') : unit.value
  return u === 'ms' ? Number(t) : Number(t) * 1000
}
/// 时间 → 毫秒:支持 ISO 与 "YYYY-MM-DD HH:mm:ss";UTC 模式按 UTC 解释
function dateToMs(text) {
  const t = text.trim()
  if (!t)
    return NaN
  let norm = /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}(:\d{2})?$/.test(t) ? t.replace(' ', 'T') : t
  if (tz.value === 'utc' && /^\d{4}-\d{2}-\d{2}T/.test(norm) && !/[Zz]|[+-]\d{2}:?\d{2}$/.test(norm))
    norm += 'Z'
  const d = new Date(norm)
  return d.getTime()
}

function convert() {
  const fromTs = direction.value === 'ts2date'
  const text = fromTs ? tsText.value : dateText.value
  if (!text.trim()) {
    if (fromTs)
      dateText.value = ''
    else
      tsText.value = ''
    errMsg.value = ''
    okCount.value = 0
    failCount.value = 0
    return
  }
  const lines = text.split(/\r\n|\r|\n/)
  const out = []
  let ok = 0
  let fail = 0
  let firstErr = ''
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i]
    if (!raw.trim()) {
      out.push('')
      continue
    }
    if (fromTs) {
      const ms = tsToMs(raw)
      if (Number.isNaN(ms)) {
        // 失败行留空:若把"第 N 行无法识别"写进结果,用户一旦改另一侧,这段提示文字会被
        // 当成输入再解析一遍,反而把原始输入冲掉。定位与原因由错误条 + 失败计数承担。
        out.push('')
        fail++
        if (!firstErr)
          firstErr = `第 ${i + 1} 行:无法识别的时间戳「${raw.trim()}」`
        continue
      }
      out.push(fmtDate(ms, tz.value === 'utc') + (tz.value === 'utc' ? ' UTC' : ''))
      ok++
    } else {
      const ms = dateToMs(raw)
      if (Number.isNaN(ms)) {
        out.push('')
        fail++
        if (!firstErr)
          firstErr = `第 ${i + 1} 行:无法识别的时间「${raw.trim()}」`
        continue
      }
      out.push(unit.value === 's' ? String(Math.floor(ms / 1000)) : String(ms))
      ok++
    }
  }
  if (fromTs)
    dateText.value = out.join('\n')
  else
    tsText.value = out.join('\n')
  errMsg.value = firstErr
  okCount.value = ok
  failCount.value = fail
}

function onTsInput(e) {
  tsText.value = e.target.value
  direction.value = 'ts2date'
  schedule(convert)
}
function onDateInput(e) {
  dateText.value = e.target.value
  direction.value = 'date2ts'
  schedule(convert)
}
function onChangeOption() {
  runNow(convert)
}
function fillNow() {
  tsText.value = unit.value === 'ms' ? String(Date.now()) : String(Math.floor(Date.now() / 1000))
  direction.value = 'ts2date'
  runNow(convert)
}
async function doCopy() {
  const r = await copyText(outText.value, null)
  if (r === 'ok')
    toast('已复制到剪贴板', 'ok')
  else if (r === 'fail')
    toast('没有可复制的内容', 'warn')
  else
    toast('已选中,请手动复制(Ctrl+C)', 'warn')
}
</script>

<template>
  <ToolShell :error="errMsg">
    <template #bar>
      <div class="seg dt-seg" role="group" aria-label="时间戳单位">
        <button type="button" class="seg-item" :class="{ active: unit === 'auto' }"
                :aria-pressed="unit === 'auto'" @click="unit = 'auto'; onChangeOption()">自动</button>
        <button type="button" class="seg-item" :class="{ active: unit === 's' }"
                :aria-pressed="unit === 's'" @click="unit = 's'; onChangeOption()">秒</button>
        <button type="button" class="seg-item" :class="{ active: unit === 'ms' }"
                :aria-pressed="unit === 'ms'" @click="unit = 'ms'; onChangeOption()">毫秒</button>
      </div>
      <div class="seg dt-seg" role="group" aria-label="时区">
        <button type="button" class="seg-item" :class="{ active: tz === 'local' }"
                :aria-pressed="tz === 'local'" @click="tz = 'local'; onChangeOption()">本地</button>
        <button type="button" class="seg-item" :class="{ active: tz === 'utc' }"
                :aria-pressed="tz === 'utc'" @click="tz = 'utc'; onChangeOption()">UTC</button>
      </div>
      <span class="sp"></span>
      <button type="button" class="btn btn-ghost btn-sm" @click="fillNow">填入当前</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!outText" @click="doCopy">复制结果</button>
    </template>

    <template #notice>
      <div v-if="paused" class="dt-warn">
        <span aria-hidden="true">⚠</span>
        <span>内容较大({{ fmtCount(src.length) }} 字符),已暂停自动换算</span>
        <button type="button" class="btn btn-secondary btn-sm" style="margin-left:auto" @click="runNow(convert)">刷新结果</button>
      </div>
    </template>

    <div class="dt-cols">
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">时间戳</span>
          <span class="dt-cap">每行一个;改这里 → 右侧出时间</span>
        </div>
        <textarea class="dt-ta" spellcheck="false" placeholder="1700000000" :value="tsText"
                  @input="onTsInput"></textarea>
      </div>
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">时间({{ tz === 'utc' ? 'UTC' : '本地' }})</span>
          <span class="dt-cap">改这里 → 左侧出时间戳</span>
        </div>
        <textarea class="dt-ta" spellcheck="false" placeholder="2026-09-23 01:00:00" :value="dateText"
                  @input="onDateInput"></textarea>
      </div>
    </div>

    <template #status>
      <span>输入:<b>{{ lineCount(src) }}</b> 行</span>
      <span>成功:<b>{{ okCount }}</b></span>
      <span v-if="failCount">失败:<b>{{ failCount }}</b></span>
      <span class="sp"></span>
      <span class="dt-hint">自动:≥13 位按毫秒,否则按秒;时间可写 ISO 或 YYYY-MM-DD HH:mm:ss</span>
    </template>
  </ToolShell>
</template>
