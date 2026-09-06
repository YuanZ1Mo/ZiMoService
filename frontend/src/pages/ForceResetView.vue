<script setup>
// 强制重置密码页(/force-reset · 需会话,force_change=1 边界)
// 表单仅新密码;成功后服务端吊销旧会话并签发新会话 → 跳门户
import { reactive, ref, inject } from 'vue'
import { useRouter } from 'vue-router'
import { authApi } from '../api/auth'
import { useSessionStore } from '../stores/session'
import PasswordStrength from '../components/PasswordStrength.vue'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const router = useRouter()
const session = useSessionStore()
const toast = inject('toast')

const form = reactive({ pwd: '' })
const errors = reactive({})
const topError = ref('')
const busy = ref(false)
const showPwd = ref(false)

async function submit() {
  Object.keys(errors).forEach(k => delete errors[k])
  topError.value = ''
  if (!form.pwd) errors.pwd = '请输入新密码'
  else if (form.pwd.length < 8 || form.pwd.length > 64) errors.pwd = '密码长度须为 8-64 位'
  if (Object.keys(errors).length) return
  busy.value = true
  try {
    await authApi.forceReset(form.pwd)
    session.setForceChange(false)
    toast('密码已更新', 'ok')
    router.push('/portal')
  } catch (e) {
    if (e.code === 'PWD_WEAK') errors.pwd = e.message || '密码强度过低'
    else if (e.code === 'FORCE_CHANGE_NOT_REQUIRED') {
      toast('当前无需强制重置', 'info')
      router.push('/portal')
    } else if (e.network) topError.value = '网络异常,请检查连接'
    else if (e.message) topError.value = e.message
    else topError.value = '重置失败'
  } finally {
    busy.value = false
  }
}

async function doLogout() {
  try { await authApi.logout() } catch { /* 会话可能已失效,照常登出 */ }
  session.clear()
  router.push('/login')
}
</script>

<template>
  <div class="auth-page">
    <div class="auth-blob" style="width:380px;height:380px;background:rgba(245,158,11,.14);top:-100px;right:-60px"></div>
    <div class="auth-blob" style="width:320px;height:320px;background:rgba(2,132,199,.15);bottom:-120px;left:-40px"></div>

    <div class="auth-theme"><ThemeSwitch /></div>

    <div class="auth-card anim-fade-up">
      <div class="auth-brand"><span class="brand-logo">Z</span>ZiMo 门户</div>

      <!-- 警示横幅:说明强制改密原因 -->
      <div class="reset-banner">
        <span class="rb-ico" aria-hidden="true">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 9v4m0 4h.01M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/></svg>
        </span>
        <span>
          <b>需要重置密码</b>
          <span class="rb-sub">管理员已重置你的账号密码。为保障安全,请先设置新密码,再继续使用门户。</span>
        </span>
      </div>

      <form @submit.prevent="submit">
        <div class="form-item">
          <label class="form-label" for="npwd">新密码</label>
          <div class="input-wrap">
            <span class="input-prefix" aria-hidden="true">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="4" y="10" width="16" height="10" rx="3"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>
            </span>
            <input id="npwd" v-model="form.pwd" :type="showPwd ? 'text' : 'password'" class="input"
                   :class="{ err: errors.pwd }" placeholder="8-64 位,不含空格" autocomplete="new-password" />
            <span class="input-suffix">
              <button class="icon-btn" type="button" :aria-label="showPwd ? '隐藏密码' : '显示密码'"
                      @click="showPwd = !showPwd">
                <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z"/><circle cx="12" cy="12" r="3"/></svg>
              </button>
            </span>
          </div>
          <PasswordStrength :password="form.pwd" />
          <span v-if="errors.pwd" class="field-error">{{ errors.pwd }}</span>
        </div>

        <div v-if="topError" class="form-top-error"><span aria-hidden="true">✕</span><span>{{ topError }}</span></div>

        <button class="btn btn-grad btn-lg btn-block" type="submit" :disabled="busy">
          <span v-if="busy" class="skeleton" style="width:14px;height:14px;border-radius:50%"></span>
          {{ busy ? '重置中…' : '重置并进入门户' }}
        </button>
        <button class="btn btn-ghost btn-block" style="margin-top:12px" type="button" @click="doLogout">退出登录</button>
      </form>
    </div>
  </div>
</template>

<style scoped>
.reset-banner{
  display:flex;gap:12px;align-items:flex-start;padding:13px 15px;margin:20px 0 22px;
  border-radius:var(--r-md);background:var(--color-warn-bg);
  border:1px solid rgba(245,158,11,.25);color:var(--color-warn);font-size:var(--fs-body);
}
.rb-ico{flex:none;width:34px;height:34px;border-radius:10px;background:rgba(245,158,11,.15);display:flex;align-items:center;justify-content:center}
.rb-ico svg{display:block}
.reset-banner b{display:block;margin-bottom:2px}
.rb-sub{color:var(--color-text-2);font-size:var(--fs-cap);line-height:18px}
</style>
