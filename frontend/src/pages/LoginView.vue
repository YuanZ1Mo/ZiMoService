<script setup>
// 登录页(/login · 白名单页)
// 仅账号登录;手机/邮箱为预留入口(置灰不可点);锁定倒计时/限流/网络错误分层提示
import { reactive, ref, computed, inject, onMounted, onBeforeUnmount } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { authApi } from '../api/auth'
import { useSessionStore } from '../stores/session'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const router = useRouter()
const route = useRoute()
const session = useSessionStore()
const toast = inject('toast')

const form = reactive({ account: '', password: '' })
const errors = reactive({})
const topError = ref('')
const busy = ref(false)
const showPwd = ref(false)
const lockRetry = ref(0)
let lockTimer = null

const lockText = computed(() => {
  const s = Math.max(0, lockRetry.value)
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`
})

onMounted(() => {
  // 会话冷启动探测:带 cookie 访问登录页 → 交给守卫/接口
  if (route.query.redirect) session.redirectAfterLogin = route.query.redirect
})
onBeforeUnmount(() => { if (lockTimer) clearInterval(lockTimer) })

function gotoRegister() { router.push('/register') }
function gotoReset() { toast('找回密码功能暂未开放', 'warn') }

function startLockCountdown(sec) {
  lockRetry.value = sec
  if (lockTimer) clearInterval(lockTimer)
  lockTimer = setInterval(() => {
    lockRetry.value -= 1
    if (lockRetry.value <= 0) {
      clearInterval(lockTimer)
      lockRetry.value = 0
    }
  }, 1000)
}

async function submit() {
  Object.keys(errors).forEach(k => delete errors[k])
  topError.value = ''
  if (!form.account || !form.password) {
    if (!form.account) errors.account = '请输入账号'
    if (!form.password) errors.password = '请输入密码'
    return
  }
  busy.value = true
  try {
    const data = await authApi.login({ account: form.account, password: form.password })
    // 会话上下文(登录响应仅 forceChange;用户信息由主页接口补充)
    session.setUser({ account: form.account.toLowerCase(), forceChange: !!data.forceChange })
    if (data.forceChange) {
      toast('首次登录需要重置密码', 'info')
      router.push('/force-reset')
    } else {
      const back = session.redirectAfterLogin || '/portal'
      session.redirectAfterLogin = ''
      router.push(back)
    }
  } catch (e) {
    if (e.code === 'AUTH_LOCKED') {
      startLockCountdown(e.data && e.data.retryAfter ? Number(e.data.retryAfter) : 60)
      topError.value = ''
    } else if (e.code === 'RATE_LIMITED') {
      topError.value = '尝试过于频繁,请稍后再试'
    } else if (e.code === 'ACCOUNT_DISABLED') {
      topError.value = '账号不可登录'
    } else if (e.network) {
      topError.value = '网络异常,请检查连接'
    } else if (e.message) {
      topError.value = e.message   // 统一提示,不泄露账号是否存在
    } else {
      topError.value = '登录失败'
    }
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="auth-page">
    <!-- 低 CPU 氛围底纹:静态模糊色块 -->
    <div class="auth-blob" style="width:420px;height:420px;background:rgba(2,132,199,.18);top:-120px;right:-80px"></div>
    <div class="auth-blob" style="width:360px;height:360px;background:rgba(6,182,212,.16);bottom:-140px;left:-60px"></div>
    <div class="auth-blob" style="width:260px;height:260px;background:rgba(236,72,153,.12);bottom:10%;right:14%"></div>

    <div class="auth-theme"><ThemeSwitch /></div>

    <div class="auth-card anim-fade-up">
      <div class="auth-brand"><span class="brand-logo">Z</span>ZiMo 门户</div>
      <h1 class="auth-title">
        欢迎回来
        <svg class="title-spark" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 3-1.9 5.8a2 2 0 0 1-1.3 1.3L3 12l5.8 1.9a2 2 0 0 1 1.3 1.3L12 21l1.9-5.8a2 2 0 0 1 1.3-1.3L21 12l-5.8-1.9a2 2 0 0 1-1.3-1.3Z"/><path d="M5 3v4M3 5h4M19 17v4M17 19h4"/></svg>
      </h1>
      <p class="auth-sub">登录你的账号,继续你的旅程</p>

      <!-- 登录方式:仅账号可用,手机/邮箱预留置灰 -->
      <div class="seg" style="margin-bottom:24px" role="tablist">
        <button class="seg-item active" type="button">账号登录</button>
        <button class="seg-item" type="button" disabled title="暂未开放">手机登录</button>
        <button class="seg-item" type="button" disabled title="暂未开放">邮箱登录</button>
      </div>

      <form @submit.prevent="submit">
        <div class="form-item">
          <label class="form-label" for="acc">账号</label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
            </span>
            <input id="acc" v-model.trim="form.account" class="input" :class="{ err: errors.account }"
                   placeholder="请输入账号" autocapitalize="none" spellcheck="false" autocomplete="username" />
          </div>
          <span v-if="errors.account" class="field-error">{{ errors.account }}</span>
        </div>

        <div class="form-item">
          <label class="form-label" for="pwd">密码</label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="10" width="16" height="10" rx="3"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
            </span>
            <input id="pwd" v-model="form.password" :type="showPwd ? 'text' : 'password'" class="input"
                   :class="{ err: errors.password }" placeholder="请输入密码" autocomplete="current-password" />
            <span class="input-suffix">
              <button class="icon-btn" type="button" :aria-label="showPwd ? '隐藏密码' : '显示密码'"
                      @click="showPwd = !showPwd">
                <!-- 显隐状态切换:可见=闭眼(点击隐藏) / 隐藏=睁眼(点击显示) -->
                <svg v-if="showPwd" width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="2" y1="2" x2="22" y2="22"/></svg>
                <svg v-else width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z"/><circle cx="12" cy="12" r="3"/></svg>
              </button>
            </span>
          </div>
          <span v-if="errors.password" class="field-error">{{ errors.password }}</span>
        </div>

        <!-- 表单级错误 / 锁定倒计时 -->
        <div v-if="topError" class="form-top-error"><span aria-hidden="true">✕</span><span>{{ topError }}</span></div>
        <div v-if="lockRetry > 0" class="form-top-error form-top-warn">
          <span aria-hidden="true">⏳</span>
          <span>失败次数过多,账号已锁定<br><b>剩余 <span class="num">{{ lockText }}</span></b> 后可再次尝试</span>
        </div>

        <button class="btn btn-grad btn-lg btn-block" type="submit" :disabled="busy">
          <span v-if="busy" class="skeleton" style="width:14px;height:14px;border-radius:50%"></span>
          {{ busy ? '登录中…' : '登 录' }}
        </button>
      </form>

      <div class="auth-links">
        <span class="form-hint">还没有账号?<a href="/register" class="link-btn" @click.prevent="gotoRegister">注册账号</a></span>
        <span class="link-disabled" title="找回密码将于后续版本开放">找回密码</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* 标题装饰星:品牌色,随亮暗主题切换 */
.title-spark{color:var(--color-primary);vertical-align:-3px;margin-left:3px}
</style>
