<script setup>
// 找回密码页(/reset · 本期预留,不可用)
import { inject } from 'vue'
import { useRouter } from 'vue-router'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const router = useRouter()
const toast = inject('toast')

function notify() {
  toast('找回密码功能暂未开放', 'warn')
}
</script>

<template>
  <div class="auth-page">
    <div class="auth-blob" style="width:420px;height:420px;background:rgba(2,132,199,.18);top:-120px;right:-80px"></div>
    <div class="auth-blob" style="width:360px;height:360px;background:rgba(6,182,212,.16);bottom:-140px;left:-60px"></div>

    <div class="auth-theme"><ThemeSwitch /></div>

    <div class="auth-card anim-fade-up">
      <div class="auth-brand"><span class="brand-logo">Z</span>ZiMo 门户</div>
      <h1 class="auth-title">找回密码</h1>
      <p class="auth-sub">预留入口 · 暂未开放</p>

      <div class="form-item">
        <label class="form-label">账号 / 邮箱 / 手机号</label>
        <input class="input" disabled placeholder="暂不可用" />
      </div>
      <button class="btn btn-primary btn-block" type="button" @click="notify">发送重置链接</button>

      <div class="auth-links">
        <a href="/login" class="link-btn" @click.prevent="router.push('/login')">返回登录</a>
      </div>
    </div>
  </div>
</template>
