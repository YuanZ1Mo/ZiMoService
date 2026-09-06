<script setup>
// 注册页(/register · 白名单页)
// 仅账号注册;手机/邮箱预留置灰;账号失焦格式校验 + 密码强度实时评估
import { reactive, ref, inject } from 'vue'
import { useRouter } from 'vue-router'
import { authApi } from '../api/auth'
import { useSessionStore } from '../stores/session'
import PasswordStrength from '../components/PasswordStrength.vue'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const router = useRouter()
const session = useSessionStore()
const toast = inject('toast')

const form = reactive({ account: '', password: '', nickname: '' })
const errors = reactive({})
const topError = ref('')
const busy = ref(false)
const showPwd = ref(false)

function backLogin() { router.push('/login') }

// 本地格式校验(4-30 位 [a-z0-9_-],首尾非 _/-);服务端为唯一性权威
function validAccount(v) {
  return /^[a-z0-9_-]{4,30}$/.test(v) && !/^[_-]|[_-]$/.test(v)
}
function onAccountBlur() {
  delete errors.account
  if (form.account && !validAccount(form.account.toLowerCase())) {
    errors.account = '4-30 位小写字母/数字/_/-,首尾不能是 _ 或 -'
  }
}

async function submit() {
  Object.keys(errors).forEach(k => delete errors[k])
  topError.value = ''
  form.account = form.account.toLowerCase()
  if (!form.account) errors.account = '请输入账号'
  else if (!validAccount(form.account)) errors.account = '4-30 位小写字母/数字/_/-,首尾不能是 _ 或 -'
  if (!form.password) errors.password = '请输入密码'
  else if (form.password.length < 8 || form.password.length > 64) errors.password = '密码长度须为 8-64 位'
  if (Object.keys(errors).length) return
  busy.value = true
  try {
    const data = await authApi.register({
      account: form.account,
      password: form.password,
      nickname: form.nickname || undefined
    })
    session.setUser({ uid: data.uid, account: data.account, nickname: data.nickname, forceChange: false })
    toast('注册成功,已自动登录', 'ok')
    router.push('/portal')
  } catch (e) {
    if (e.code === 'USER_EXISTS') errors.account = '账号已存在,换一个试试'
    else if (e.code === 'INVALID_ACCOUNT') errors.account = e.message
    else if (e.code === 'INVALID_NICKNAME') errors.nickname = e.message
    else if (e.code === 'PWD_WEAK') errors.password = e.message || '密码强度过低'
    else if (e.code === 'RATE_LIMITED') topError.value = '操作过于频繁,请稍后再试'
    else if (e.network) topError.value = '网络异常,请检查连接'
    else if (e.message) topError.value = e.message
    else topError.value = '注册失败'
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div class="auth-page">
    <div class="auth-blob" style="width:420px;height:420px;background:rgba(6,182,212,.18);top:-120px;left:-80px"></div>
    <div class="auth-blob" style="width:340px;height:340px;background:rgba(236,72,153,.13);bottom:-120px;right:-40px"></div>
    <div class="auth-blob" style="width:240px;height:240px;background:rgba(2,132,199,.14);top:16%;right:16%"></div>

    <div class="auth-theme"><ThemeSwitch /></div>

    <div class="auth-card anim-fade-up">
      <div class="auth-brand"><span class="brand-logo">Z</span>ZiMo 门户</div>
      <h1 class="auth-title">创建你的账号 ✨</h1>
      <p class="auth-sub">一分钟完成注册,自动登录直达门户</p>

      <!-- 注册方式:仅账号可用,手机/邮箱预留置灰 -->
      <div class="seg" style="margin-bottom:24px" role="tablist">
        <button class="seg-item active" type="button">账号注册</button>
        <button class="seg-item" type="button" disabled title="暂未开放">手机注册</button>
        <button class="seg-item" type="button" disabled title="暂未开放">邮箱注册</button>
      </div>

      <form @submit.prevent="submit">
        <div class="form-item">
          <label class="form-label" for="acc">账号</label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
            </span>
            <input id="acc" v-model.trim="form.account" class="input" :class="{ err: errors.account }"
                   placeholder="4-30 位小写字母 / 数字 / _ / -" autocapitalize="none" spellcheck="false" autocomplete="username"
                   @blur="onAccountBlur" />
          </div>
          <span v-if="errors.account" class="field-error">{{ errors.account }}</span>
          <span v-else class="form-hint">首尾不能是 _ 或 -,注册后不可修改</span>
        </div>

        <div class="form-item">
          <label class="form-label" for="pwd">密码</label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="10" width="16" height="10" rx="3"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
            </span>
            <input id="pwd" v-model="form.password" :type="showPwd ? 'text' : 'password'" class="input"
                   :class="{ err: errors.password }" placeholder="8-64 位,不含空格" autocomplete="new-password" />
            <span class="input-suffix">
              <button class="icon-btn" type="button" :aria-label="showPwd ? '隐藏密码' : '显示密码'"
                      @click="showPwd = !showPwd">
                <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z"/><circle cx="12" cy="12" r="3"/></svg>
              </button>
            </span>
          </div>
          <PasswordStrength :password="form.password" />
          <span v-if="errors.password" class="field-error">{{ errors.password }}</span>
        </div>

        <div class="form-item">
          <label class="form-label" for="nick">昵称 <span class="opt">选填,默认与账号一致</span></label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M17 8a5 5 0 1 0-10 0c0 4-2 5-2 5h14s-2-1-2-5"/><path d="M10 20a2.2 2.2 0 0 0 4 0"/></svg>
            </span>
            <input id="nick" v-model.trim="form.nickname" class="input" :class="{ err: errors.nickname }"
                   placeholder="1-20 个字符,不含空白" />
          </div>
          <span v-if="errors.nickname" class="field-error">{{ errors.nickname }}</span>
        </div>

        <div v-if="topError" class="form-top-error"><span aria-hidden="true">✕</span><span>{{ topError }}</span></div>

        <button class="btn btn-grad btn-lg btn-block" type="submit" :disabled="busy">
          <span v-if="busy" class="skeleton" style="width:14px;height:14px;border-radius:50%"></span>
          {{ busy ? '注册中…' : '注 册' }}
        </button>
      </form>

      <div class="auth-links">
        <span class="form-hint">已有账号?<a href="/login" class="link-btn" @click.prevent="backLogin">返回登录</a></span>
        <span class="form-hint">注册即代表同意社区规范</span>
      </div>
    </div>
  </div>
</template>
