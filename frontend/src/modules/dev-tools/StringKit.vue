<script setup>
// 字符串整理(二期 A):大小写 / 去空白 / 去重 / 排序 / 换行符转换
// 一律"输入 → 输出"(不就地改原文),输出可回填;纯变换在 textops.js,便于单测
import { ref, inject } from 'vue'
import ToolShell from './ToolShell.vue'
import { copyText, useHeavyInput, fmtCount, lineCount } from './toolbox'
import { STRING_OPS, applyStringOp } from './textops'

const toast = inject('toast')

const src = ref('')
const out = ref('')
const outEl = ref(null)
/// 原文改动后旧结果是否已过期(保留内容但明确标记,不静默清空)
const stale = ref(false)

const { paused } = useHeavyInput(src)

const OPS = STRING_OPS

function applyOp(key) {
  if (!src.value) {
    toast('请先输入内容', 'warn')
    return
  }
  out.value = applyStringOp(src.value, key)
  stale.value = false
}
function onInput(e) {
  src.value = e.target.value
  stale.value = out.value !== ''
}
function fillBack() {
  if (!out.value) {
    toast('没有可回填的内容', 'warn')
    return
  }
  src.value = out.value
  out.value = ''
  stale.value = false
}
async function doCopy() {
  const r = await copyText(out.value, outEl.value)
  if (r === 'ok')
    toast('已复制到剪贴板', 'ok')
  else if (r === 'fail')
    toast('没有可复制的内容', 'warn')
  else
    toast('已选中,请手动复制(Ctrl+C)', 'warn')
}
</script>

<template>
  <ToolShell>
    <template #bar>
      <button v-for="op in OPS" :key="op.key" type="button" class="btn btn-ghost btn-sm"
              @click="applyOp(op.key)">{{ op.name }}</button>
      <span class="sp"></span>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="fillBack">输出回填</button>
      <button type="button" class="btn btn-ghost btn-sm" :disabled="!out" @click="doCopy">复制结果</button>
    </template>

    <template #notice>
      <div v-if="paused" class="dt-warn">
        <span aria-hidden="true">⚠</span>
        <span>内容较大({{ fmtCount(src.length) }} 字符)</span>
      </div>
    </template>

    <div class="dt-cols">
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输入</span>
          <span class="dt-cap">原文不会被修改</span>
        </div>
        <textarea class="dt-ta" spellcheck="false" placeholder="在此粘贴多行文本…" :value="src"
                  @input="onInput"></textarea>
      </div>
      <div class="dt-pane">
        <div class="dt-pane-head">
          <span class="dt-pane-title">输出</span>
          <span v-if="stale" class="badge badge-warn">已过期</span>
          <span class="dt-cap">{{ stale ? '原文已改动,结果对应旧内容' : '点上方任一操作生成' }}</span>
        </div>
        <textarea ref="outEl" class="dt-ta wrap" readonly :value="out" placeholder="结果会显示在这里"></textarea>
      </div>
    </div>

    <template #status>
      <span>输入:<b>{{ lineCount(src) }}</b> 行 · <b>{{ fmtCount(src.length) }}</b> 字符</span>
      <span v-if="out">输出:<b>{{ lineCount(out) }}</b> 行<span v-if="stale">(已过期)</span></span>
      <span class="sp"></span>
      <span class="dt-hint">排序按中文拼音(localeCompare zh-Hans-CN);编辑框内的换行会被浏览器统一为 LF,需要 Windows 风格文本时用「统一为 CRLF」</span>
    </template>
  </ToolShell>
</template>
