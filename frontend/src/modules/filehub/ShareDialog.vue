<script setup>
// 分享设置弹窗(§3.12):提取码/有效期/次数上限/仅登录可见 → 创建成功态(链接+二维码)
// 二维码前端本地生成(qrcode 库),不上传链接到第三方服务
import { ref, reactive, watch, nextTick } from 'vue'
import Modal from '../../components/Modal.vue'
import QRCode from 'qrcode'
import { filehubApi, fmtSize } from '../../api/filehub'

const props = defineProps({
  show: { type: Boolean, default: false },
  node: { type: Object, default: null },      // 分享目标
  isPublic: { type: Boolean, default: true }  // 公共空间分享不提供"仅登录可见"开关(§3.12.2)
})
const emit = defineEmits(['close', 'created'])

const form = reactive({ pwd_enabled: false, expire_days: 7, max_downloads: 0, login_only: false })
const busy = ref(false)
const done = ref(null)   // 创建成功结果 {url, pwd, expire_time}

watch(() => props.show, (v) => {
  if (v) {
    form.pwd_enabled = false; form.expire_days = 7; form.max_downloads = 0; form.login_only = false
    done.value = null
  } else {
    nextTick(() => { done.value = null })
  }
})

async function create() {
  if (!props.node) return
  busy.value = true
  try {
    const r = await filehubApi.shareCreate({
      space: props.node.space, node_id: props.node.id,
      pwd_enabled: form.pwd_enabled, expire_days: Number(form.expire_days),
      max_downloads: Number(form.max_downloads), login_only: form.login_only
    })
    done.value = r
    emit('created', r)
    nextTick(drawQr)
  } finally { busy.value = false }
}
function drawQr() {
  if (!done.value) return
  const canvas = document.getElementById('fh-qr')
  if (canvas) QRCode.toCanvas(canvas, done.value.url, { width: 104, margin: 0 }, () => {})
}
async function copy(text, btn) {
  try {
    await navigator.clipboard.writeText(text)
    if (btn && btn.target) { const el = btn.target; const old = el.textContent; el.textContent = '已复制 ✓'; setTimeout(() => { el.textContent = old }, 1500) }
  } catch { /* 剪贴板不可用 */ }
}
const EXPIRES = [{ v: 0, n: '永久' }, { v: 1, n: '1 天' }, { v: 7, n: '7 天' }, { v: 30, n: '30 天' }]
</script>

<template>
  <Modal :show="show" :title="done ? '分享已创建' : `分享「${node ? node.name : ''}」`" @close="emit('close')">
    <!-- 设置态 -->
    <template v-if="!done">
      <div class="form-item">
        <label class="form-label">提取码 <span class="opt">开启后访问需输入 4 位码</span></label>
        <div class="row between" style="background:var(--color-bg);border:1px solid var(--color-border-soft);border-radius:var(--r-md);padding:10px 14px">
          <span style="font-size:var(--fs-cap);color:var(--color-text-2)">需要提取码才能访问</span>
          <button type="button" class="switch" :class="{ on: form.pwd_enabled }" aria-label="提取码开关" @click="form.pwd_enabled = !form.pwd_enabled"></button>
        </div>
        <span class="form-hint">明文仅创建后展示一次,可在「我的分享」中重置</span>
      </div>
      <div class="row" style="gap:12px;flex-wrap:wrap">
        <div class="form-item" style="flex:1;min-width:170px">
          <label class="form-label">有效期</label>
          <select v-model.number="form.expire_days" class="input">
            <option v-for="e in EXPIRES" :key="e.v" :value="e.v">{{ e.n }}</option>
          </select>
        </div>
        <div class="form-item" style="flex:1;min-width:170px">
          <label class="form-label">下载次数上限 <span class="opt">0 = 不限</span></label>
          <input v-model.number="form.max_downloads" class="input" type="number" min="0" step="1" />
        </div>
      </div>
      <div v-if="!isPublic" class="row between" style="background:var(--color-bg);border:1px solid var(--color-border-soft);border-radius:var(--r-md);padding:10px 14px">
        <div>
          <b style="font-size:var(--fs-body)">仅登录可见</b>
          <div class="form-hint">开启后未登录访客先跳登录,登录后回到分享页</div>
        </div>
        <button type="button" class="switch" :class="{ on: form.login_only }" aria-label="仅登录可见开关" @click="form.login_only = !form.login_only"></button>
      </div>
      <p class="form-hint" style="margin-top:10px">分享绑定条目而非路径:目标被移动/重命名不影响访问;目标进回收站时分享暂不可用,恢复后自动可用。</p>
    </template>

    <!-- 成功态:链接 + 提取码(一次性)+ 本地二维码 -->
    <template v-else>
      <div class="row" style="gap:16px;align-items:flex-start;flex-wrap:wrap">
        <div style="flex:1;min-width:220px;display:flex;flex-direction:column;gap:10px">
          <div class="share-url">{{ done.url }}</div>
          <div class="row" style="gap:8px;flex-wrap:wrap">
            <button class="btn btn-primary btn-sm" type="button" @click="copy(done.url, $event)">复制链接</button>
            <button v-if="done.pwd" class="btn btn-secondary btn-sm" type="button" @click="copy(done.pwd, $event)">复制提取码 {{ done.pwd }}</button>
          </div>
          <span class="form-hint">有效期 {{ form.expire_days > 0 ? form.expire_days + ' 天' : '永久' }} · {{ form.max_downloads ? `限 ${form.max_downloads} 次下载` : '下载不限次' }}{{ form.pwd_enabled ? ' · 需提取码' : '' }}</span>
        </div>
        <div class="qr-box"><canvas id="fh-qr" width="104" height="104"></canvas></div>
      </div>
    </template>

    <template #foot>
      <template v-if="!done">
        <button class="btn btn-ghost" type="button" @click="emit('close')">取消</button>
        <button class="btn btn-grad" type="button" :disabled="busy" @click="create">创建分享</button>
      </template>
      <template v-else>
        <button class="btn btn-ghost" type="button" @click="emit('close')">完成</button>
      </template>
    </template>
  </Modal>
</template>
