<script setup>
// 免登录分享页 /s/:token(meta.public,不进门户壳,§7.5)
// 状态:加载 → 提取码校验 / 需登录 → 浏览(只读+下载+打包)/ 失效
// 打包下载:服务端返回 {task_no} 表示压缩中,前端每 2s 重试同一请求直至返回 {url}(免登录无任务面板;
// 上限约 60s,超时提示失败)
import { ref, reactive, computed, onMounted, onUnmounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { filehubApi, downloadByUrl, fmtNodeSize, fmtTime, kindOf } from '../api/filehub'
import FileIcon from '../modules/filehub/FileIcon.vue'
import '../modules/filehub/filehub.css'

const route = useRoute()
const token = computed(() => String(route.params.token || ''))

const state = ref('loading')   // loading | pwd | browse | need_login | expired | unavailable | notfound
const info = reactive({ name: '', node_type: 1, owner_name: '', expire_time: 0, max_downloads: 0, download_count: 0 })
const pwd = ref('')
const pwdErr = ref('')
const pwdBusy = ref(false)
const dirId = ref(0)
const breadcrumb = ref([])
const items = ref([])
const loadingList = ref(false)
const selected = ref(new Set())
const packing = ref(false)      // 下载全部打包中
const PACK_RETRY_MAX = 30       // 2s × 30 ≈ 60s 上限,超时提示失败
let packTimer = null
let packTries = 0

async function loadInfo() {
  state.value = 'loading'
  try {
    const d = await filehubApi.shareInfo(token.value)
    Object.assign(info, d)
    if (d.need_pwd) { state.value = 'pwd'; return }
    state.value = 'browse'
    // 目录分享才需要列列表;单文件分享没有子条目,直接按分享目标本身下载(§3.12.3)
    if (Number(info.node_type) === 1) loadList()
  } catch (e) {
    if (e.status === 401 || e.code === 'NEED_LOGIN') { state.value = 'need_login'; return }
    if (e.code === 'SHARE_EXPIRED') { state.value = 'expired'; return }
    if (e.code === 'SHARE_UNAVAILABLE') { state.value = 'unavailable'; return }
    state.value = 'notfound'
  }
}
onMounted(loadInfo)
onUnmounted(stopPackRetry)

async function verify() {
  if (!pwd.value.trim()) return
  pwdBusy.value = true; pwdErr.value = ''
  try {
    const r = await filehubApi.shareVerify(token.value, { pwd: pwd.value })
    if (r.pass) { state.value = 'browse'; if (Number(info.node_type) === 1) loadList() }
    else pwdErr.value = '提取码不正确,请重试'
  } catch (e) {
    if (e.code === 'SHARE_LOCKED') pwdErr.value = '错误次数过多,请 10 分钟后再试'
    else pwdErr.value = e.message || '校验失败'
  } finally { pwdBusy.value = false }
}

async function loadList() {
  loadingList.value = true
  try {
    const d = await filehubApi.shareListDir(token.value, { dir_id: dirId.value, page: 1, size: 500 })
    items.value = d.list || []
    breadcrumb.value = d.breadcrumb || []
    selected.value = new Set()
  } catch (e) {
    if (e.code === 'SHARE_UNAVAILABLE') state.value = 'unavailable'
    else toastErr(e.message || '加载分享内容失败')
  } finally { loadingList.value = false }
}
function gotoCrumb(bc) {
  dirId.value = bc ? bc.id : 0
  loadList()
}
function toggleSel(n) {
  const s = new Set(selected.value)
  s.has(n.id) ? s.delete(n.id) : s.add(n.id)
  selected.value = s
}
// 表头全选态:当前目录条目全在选中集=全选,部分选中=半选(indeterminate)
const allChecked = computed(() => items.value.length > 0 && items.value.every(n => selected.value.has(n.id)))
const someChecked = computed(() => !allChecked.value && items.value.some(n => selected.value.has(n.id)))
function toggleAll(checked) { selected.value = checked ? new Set(items.value.map(n => n.id)) : new Set() }
function downloadOne(n) {
  if (Number(n.type) === 1) { dirId.value = n.id; loadList(); return }
  // 单文件分享的条目不在列表里,下载目标就是分享绑定的条目本身(info.node_id)
  const id = Number(n.id) || Number(info.node_id) || 0
  if (!id) { toastErr('分享目标无效,无法下载'); return }
  filehubApi.shareDownload(token.value, { ids: [id] })
    .then(r => { if (r && r.url) downloadByUrl(r.url); else toastErr('下载地址获取失败,请重试') })
    .catch(e => toastErr(e.message || '下载失败'))
}
/** 停掉打包轮询定时器(就绪、失败、超时、离开页面都要停,否则离开后仍在后台反复请求) */
function stopPackRetry() {
  if (packTimer) { clearTimeout(packTimer); packTimer = null }
}
function downloadAll() {
  const ids = selected.value.size ? [...selected.value] : items.value.map(x => x.id)
  if (!ids.length) { toastErr('当前目录没有可下载的内容'); return }
  packing.value = true
  packTries = 0
  const attempt = async () => {
    try {
      const r = await filehubApi.shareDownload(token.value, { ids })
      if (r && r.url) {
        packing.value = false; stopPackRetry()
        downloadByUrl(r.url)
        return
      }
      // {task_no} = 压缩中:免登录无任务面板,2s 后幂等重试直至就绪
      if (++packTries >= PACK_RETRY_MAX) {
        packing.value = false; stopPackRetry()
        toastErr('打包超时,请稍后在「我的分享」重试')
        return
      }
      packTimer = setTimeout(attempt, 2000)
    } catch (e) { packing.value = false; stopPackRetry(); toastErr(e.message || '打包失败') }
  }
  attempt()
}
function toastErr(msg) { toastMsg.value = msg; setTimeout(() => { toastMsg.value = '' }, 3000) }
const toastMsg = ref('')

const remain = computed(() => {
  if (!info.expire_time) return '永久有效'
  return `剩 ${Math.max(0, Math.ceil((info.expire_time * 1000 - Date.now()) / 86400000))} 天`
})
function goLogin() {
  location.href = `/login?redirect=${encodeURIComponent('/s/' + token.value)}`
}
watch(token, loadInfo)
</script>

<template>
  <div class="share-page">
    <header class="share-top">
      <div class="portal-brand"><span class="portal-logo">Z</span>ZiMo 文件中心</div>
      <span style="flex:1"></span>
      <span class="badge badge-dim">免登录访问</span>
    </header>

    <main class="share-mid">
      <!-- 加载中 -->
      <div v-if="state === 'loading'" class="card share-card" style="padding:32px">
        <div v-for="i in 3" :key="i" class="skeleton" style="height:22px;margin-bottom:14px"></div>
      </div>

      <!-- 提取码 -->
      <div v-else-if="state === 'pwd'" class="card share-card" style="padding:36px;text-align:center">
        <div class="share-owner" style="justify-content:center">
          <span class="avatar">{{ (info.owner_name || '?').charAt(0) }}</span>
          <div style="text-align:left">
            <b>{{ info.owner_name }}</b> 分享了{{ Number(info.node_type) === 1 ? '文件夹' : '文件' }}
            <div class="cap" style="color:var(--color-text-3)">来自 ZiMo 文件中心</div>
          </div>
        </div>
        <b style="font-size:19px;display:block;margin-top:20px">{{ info.name }}</b>
        <div class="form-item" style="max-width:280px;margin:24px auto 0">
          <input v-model.trim="pwd" class="input" style="height:46px;text-align:center;letter-spacing:4px;font-weight:700"
                 maxlength="8" placeholder="输入提取码" @keyup.enter="verify" />
          <span v-if="pwdErr" class="field-error" style="justify-content:center">{{ pwdErr }}</span>
          <button class="btn btn-grad btn-lg btn-block" type="button" :disabled="pwdBusy || !pwd.trim()" @click="verify">提取文件</button>
        </div>
      </div>

      <!-- 需登录 -->
      <div v-else-if="state === 'need_login'" class="card share-card" style="padding:36px;text-align:center">
        <div style="width:64px;height:64px;border-radius:20px;background:var(--color-primary-soft);color:var(--color-primary);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
        </div>
        <b style="font-size:var(--fs-h2)">此分享仅登录用户可见</b>
        <p class="cap" style="color:var(--color-text-2);margin-top:8px">登录后将回到本页面</p>
        <button class="btn btn-grad btn-lg" type="button" style="margin-top:16px;min-width:160px" @click="goLogin">去登录</button>
      </div>

      <!-- 浏览 -->
      <template v-else-if="state === 'browse'">
        <div class="card" style="width:760px;max-width:100%;padding:20px">
          <div class="share-owner">
            <span class="avatar">{{ (info.owner_name || '?').charAt(0) }}</span>
            <div style="flex:1;min-width:0">
              <div><b>{{ info.owner_name }}</b> 分享了{{ Number(info.node_type) === 1 ? '文件夹' : '文件' }} · <b>{{ info.name }}</b></div>
              <div class="cap" style="color:var(--color-text-3)">
                {{ remain }}<template v-if="info.max_downloads"> · 已下载 {{ info.download_count }}/{{ info.max_downloads }} 次</template>
              </div>
            </div>
            <button v-if="Number(info.node_type) === 1" class="btn btn-grad" type="button" :disabled="packing" @click="downloadAll">
              {{ packing ? '打包中…' : (selected.size ? `下载所选(${selected.size})` : '下载全部(zip)') }}
            </button>
            <button v-else class="btn btn-grad" type="button" @click="downloadOne({ id: info.node_id, type: 2, name: info.name })">下载</button>
          </div>
        </div>
        <!-- 单文件分享:没有子条目,直接给一个文件条目(不渲染空列表) -->
        <div v-if="Number(info.node_type) !== 1" class="card"
             style="width:760px;max-width:100%;margin-top:16px;padding:20px">
          <div class="fcell">
            <FileIcon :node="{ type: 2, name: info.name }" />
            <span class="fname">{{ info.name }}</span>
            <span style="flex:1"></span>
            <button class="btn btn-secondary btn-sm" type="button" @click="downloadOne({ id: info.node_id, type: 2, name: info.name })">下载</button>
          </div>
        </div>
        <div v-if="Number(info.node_type) === 1" class="card" style="width:760px;max-width:100%;margin-top:16px;overflow:hidden">
          <div class="crumbs">
            <template v-for="(bc, i) in breadcrumb" :key="bc.id">
              <button type="button" :class="{ here: i === breadcrumb.length - 1 }" @click="gotoCrumb(i === 0 ? null : bc)">{{ bc.name }}</button>
              <svg v-if="i < breadcrumb.length - 1" class="sep" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="m9 6 6 6-6 6"/></svg>
            </template>
          </div>
          <div style="overflow-x:auto">
            <table class="ftbl">
              <thead>
                <tr>
                  <th class="col-cb">
                    <!-- 全选:勾选=选中当前目录全部条目;部分选中显示半选态 -->
                    <input type="checkbox" class="fcheck" :checked="allChecked" :indeterminate="someChecked"
                           :aria-label="allChecked ? '取消全选' : '全选'" @change="toggleAll($event.target.checked)" />
                  </th>
                  <th style="min-width:200px">名称</th>
                  <th style="width:84px">类型</th>
                  <th style="width:96px">大小</th>
                  <th style="width:130px">修改时间</th>
                  <th style="width:90px"></th>
                </tr>
              </thead>
              <tbody>
                <tr v-if="loadingList">
                  <td :colspan="6" style="padding:16px">
                    <div v-for="i in 4" :key="i" class="skeleton" style="height:20px;margin-bottom:10px"></div>
                  </td>
                </tr>
                <template v-else>
                  <tr v-for="n in items" :key="n.id" :class="{ sel: selected.has(n.id) }"
                      @click="toggleSel(n)" @dblclick="downloadOne(n)">
                    <!-- dblclick.stop:复选框双击不能冒泡到行,否则会被当成"打开/下载" -->
                    <td class="col-cb" @click.stop @dblclick.stop>
                      <input type="checkbox" class="fcheck" :checked="selected.has(n.id)" @change="toggleSel(n)" :aria-label="`选择 ${n.name}`" />
                    </td>
                    <td>
                      <div class="fcell">
                        <FileIcon :node="n" />
                        <span class="fname" :style="Number(n.type) === 1 ? 'cursor:pointer' : ''" @click="Number(n.type) === 1 && downloadOne(n)">{{ n.name }}</span>
                      </div>
                    </td>
                    <td><span class="ftype">{{ kindOf(n) === 'dir' ? '文件夹' : '文件' }}</span></td>
                    <td><span class="num">{{ fmtNodeSize(n) }}</span></td>
                    <td><span class="num">{{ fmtTime(n.update_time) }}</span></td>
                    <td>
                      <span class="row-ops">
                        <button class="btn btn-secondary btn-sm" type="button" @click.stop="downloadOne(n)">
                          {{ Number(n.type) === 1 ? '打开' : '下载' }}
                        </button>
                      </span>
                    </td>
                  </tr>
                  <tr v-if="!items.length">
                    <td :colspan="6"><div class="empty"><div class="empty-icon">📁</div><div class="empty-title">此目录为空</div></div></td>
                  </tr>
                </template>
              </tbody>
            </table>
          </div>
        </div>
      </template>

      <!-- 失效 / 不可用 / 不存在 -->
      <div v-else class="card share-card" style="padding:36px;text-align:center">
        <div v-if="state === 'expired'" style="width:64px;height:64px;border-radius:20px;background:var(--color-err-bg);color:var(--color-err);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="m9 9 6 6M15 9l-6 6"/></svg>
        </div>
        <div v-else-if="state === 'unavailable'" style="width:64px;height:64px;border-radius:20px;background:var(--color-warn-bg);color:var(--color-warn);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 6h18v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 10h6"/></svg>
        </div>
        <div v-else style="width:64px;height:64px;border-radius:20px;background:var(--color-border-soft);color:var(--color-text-3);display:flex;align-items:center;justify-content:center;margin:0 auto 12px">
          <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5M8 11h6"/></svg>
        </div>
        <b style="font-size:var(--fs-h2)">
          {{ state === 'expired' ? '分享已失效' : state === 'unavailable' ? '分享内容暂不可用' : '分享不存在或链接有误' }}
        </b>
        <p class="cap" style="color:var(--color-text-2);margin-top:8px">
          {{ state === 'expired' ? '链接已过期、被取消或下载次数已达上限' : state === 'unavailable' ? '该文件正在回收站中;恢复后本链接将自动恢复可用' : '请向分享者确认链接是否正确' }}
        </p>
      </div>
    </main>

    <footer class="share-foot">由 ZiMo 文件中心提供 · 仅可浏览与下载</footer>
    <Teleport to="body">
      <Transition name="toast">
        <div v-if="toastMsg" class="toast toast-err"><span aria-hidden="true">✕</span><span>{{ toastMsg }}</span></div>
      </Transition>
    </Teleport>
  </div>
</template>

<style scoped>
.share-page{min-height:100vh;display:flex;flex-direction:column;
  background:var(--grad-brand-soft)}
[data-theme="dark"] .share-page{background:radial-gradient(1200px 500px at 70% -10%,rgba(56,189,248,.14),transparent 60%),radial-gradient(900px 420px at 10% 110%,rgba(236,72,153,.12),transparent 55%),var(--color-bg)}
.share-top{display:flex;align-items:center;gap:10px;padding:16px 24px}
.portal-brand{display:flex;align-items:center;gap:10px;font-weight:800;font-size:16px}
.portal-logo{width:30px;height:30px;border-radius:10px;background:var(--grad-brand);color:#fff;display:flex;align-items:center;justify-content:center;font-weight:700;box-shadow:var(--shadow-pop)}
.share-mid{flex:1;display:flex;flex-direction:column;align-items:center;padding:24px 16px 40px}
.share-card{width:640px;max-width:100%}
.share-owner{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.share-foot{text-align:center;padding:16px;color:var(--color-text-3);font-size:var(--fs-cap)}
.fcheck{width:16px;height:16px;accent-color:var(--color-primary);cursor:pointer}
</style>
