<script setup>
// 404 页(/404 · 白名单页)
// 按会话状态切换按钮:已登录 → 返回门户;未登录 → 去登录
import { computed } from 'vue'
import { useSessionStore } from '../stores/session'
import ThemeSwitch from '../components/ThemeSwitch.vue'

const session = useSessionStore()
const loggedIn = computed(() => session.isLoggedIn === true)
</script>

<template>
  <div class="auth-page nf-page">
    <div class="auth-blob" style="width:340px;height:340px;background:rgba(236,72,153,.13);top:-80px;left:22%"></div>
    <div class="auth-blob" style="width:300px;height:300px;background:rgba(6,182,212,.16);bottom:-60px;right:18%"></div>

    <div class="auth-theme"><ThemeSwitch /></div>

    <div class="nf-box">
      <!-- 漂浮装饰体(仅 transform 动画,低 CPU) -->
      <div class="nf-shapes" aria-hidden="true"><i></i><i></i><i></i></div>
      <div class="nf-code num" aria-hidden="true">404</div>
      <h1 class="nf-title">页面走丢了</h1>
      <p class="nf-desc">你要找的页面不存在,或者已被移走。<br>检查一下地址,或者从下面继续。</p>
      <div class="nf-actions">
        <button v-if="loggedIn" class="btn btn-grad" type="button" @click="$router.push('/portal')">返回门户</button>
        <button v-else class="btn btn-grad" type="button" @click="$router.push('/login')">去登录</button>
        <button class="btn btn-secondary" type="button" @click="$router.push('/')">回到首页</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.nf-box{position:relative;z-index:1;text-align:center;max-width:460px}
.nf-shapes{display:flex;justify-content:center;gap:14px;margin-bottom:20px}
.nf-shapes i{
  display:block;border-radius:30% 70% 62% 38%/56% 34% 66% 44%;opacity:.85;
  animation:nfFloat 4s ease-in-out infinite;
}
.nf-shapes i:nth-child(1){width:44px;height:44px;background:var(--sky-400)}
.nf-shapes i:nth-child(2){width:58px;height:58px;background:var(--cyan-400);animation-delay:.7s}
.nf-shapes i:nth-child(3){width:40px;height:40px;background:var(--pink-400);animation-delay:1.4s}
@keyframes nfFloat{0%,100%{transform:translateY(0)}50%{transform:translateY(-8px)}}
.nf-code{
  font-size:110px;font-weight:800;line-height:1;letter-spacing:4px;
  background:var(--grad-hero);-webkit-background-clip:text;background-clip:text;color:transparent;
}
.nf-title{font-size:var(--fs-h1);margin-top:16px}
.nf-desc{color:var(--color-text-2);margin:10px 0 0}
.nf-actions{display:flex;gap:12px;justify-content:center;margin-top:24px}
</style>
