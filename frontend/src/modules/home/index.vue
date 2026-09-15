<script setup>
// 用户主页(模块 code=home,index=1)
// 渐变欢迎横幅 + 概览统计 + 拥有的模块卡片(可点击进入) + 特效区占位
// 数据来自门户壳透传的 /portal/home 结果
import { computed, inject } from 'vue'
import { useRouter } from 'vue-router'

const props = defineProps({ home: { type: Object, default: null } })
const router = useRouter()
const toast = inject('toast')

const info = computed(() => props.home || {})
const roleName = computed(() => (info.value.role && info.value.role.name) || info.value.roleCode || '—')
const roleCode = computed(() => (info.value.role && info.value.role.code) || info.value.roleCode || '')
const roleLevel = computed(() => (info.value.role && info.value.role.level) || 0)
const modules = computed(() => info.value.modules || [])

// 时段问候
const greeting = computed(() => {
  const h = new Date().getHours()
  if (h < 6) return '夜深了'
  if (h < 11) return '早上好'
  if (h < 14) return '中午好'
  if (h < 18) return '下午好'
  return '晚上好'
})
const displayName = computed(() => info.value.nickname || info.value.account || '…')

// 角色徽章配色(code 语义:developer 蓝→青 / admin 粉 / 其余中性)
const roleBadgeClass = computed(() => {
  const c = roleCode.value
  if (c === 'developer') return 'badge-role-developer'
  if (c === 'admin') return 'badge-role-admin'
  return 'badge-role-user'
})

// 模块图标:与侧边导航同源(按 code 语义映射)
function icoPath(code) {
  if (code === 'home') return '<path d="m3 10 9-7 9 7v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/><path d="M9 22V12h6v10"/>'
  if (code === 'systemManager') return '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.9 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.9.3h.1a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5h.1a1.7 1.7 0 0 0 1.9-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.9v.1a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1Z"/>'
  return '<rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><rect x="14" y="14" width="7" height="7" rx="2"/>'
}

function enter(m) {
  router.push(m.url || `/portal/${m.code}`)
}

function fxHint() {
  toast('特效区建设中,敬请期待', 'info')
}
</script>

<template>
  <div>
    <!-- 欢迎横幅 -->
    <section class="hero anim-fade-up">
      <div class="hero-body">
        <div class="hero-text">
          <h1>{{ greeting }},{{ displayName }} 👋</h1>
          <p>今天也是元气满满的一天,欢迎回到 ZiMo。</p>
        </div>
        <div class="hero-chips">
          <span class="h-chip role">
            <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="m12 2 2.4 4.9 5.4.8-3.9 3.8.9 5.4-4.8-2.5-4.8 2.5.9-5.4L4.2 7.7l5.4-.8Z"/></svg>
            {{ roleName }}
          </span>
          <span v-if="roleLevel" class="h-chip num">等级 LV.{{ roleLevel }}</span>
        </div>
      </div>
    </section>

    <!-- 概览统计 -->
    <section v-if="props.home" class="stat-grid anim-stagger">
      <div class="card hover-lift stat-card">
        <div class="stat-ico" style="background:var(--color-info-bg);color:var(--color-info)">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
        </div>
        <div class="stat-meta">
          <div class="lbl">账号</div>
          <div class="val num">{{ info.account || '…' }}</div>
          <div class="sub num">UID {{ info.uid || '…' }}</div>
        </div>
      </div>
      <div class="card hover-lift stat-card">
        <div class="stat-ico" style="background:var(--color-ok-bg);color:var(--color-ok)">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path d="M12 22s8-3.5 8-10V5l-8-3-8 3v7c0 6.5 8 10 8 10Z"/><path d="m9 12 2 2 4-4"/></svg>
        </div>
        <div class="stat-meta">
          <div class="lbl">角色定位</div>
          <div class="val" style="font-size:20px">
            {{ roleName }}
            <small v-if="roleLevel" style="font-size:var(--fs-body);font-weight:400;color:var(--color-text-2)">Lv.{{ roleLevel }}</small>
          </div>
          <div class="sub">决定你可访问的模块与操作</div>
        </div>
      </div>
      <div class="card hover-lift stat-card">
        <div class="stat-ico" style="background:var(--color-primary-soft);color:var(--color-primary)">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><rect x="14" y="14" width="7" height="7" rx="2"/></svg>
        </div>
        <div class="stat-meta">
          <div class="lbl">拥有的模块</div>
          <div class="val num">{{ modules.length }} <small>个</small></div>
          <div class="sub">由权限推导,随策略变更自动刷新</div>
        </div>
      </div>
    </section>

    <!-- 数据未返回:骨架屏 -->
    <section v-else class="stat-grid" aria-busy="true">
      <div v-for="i in 3" :key="i" class="card stat-card">
        <div class="skeleton" style="width:38px;height:38px;border-radius:12px"></div>
        <div style="flex:1;display:flex;flex-direction:column;gap:8px">
          <div class="skeleton" style="width:56px;height:12px"></div>
          <div class="skeleton" style="width:110px;height:22px"></div>
        </div>
      </div>
    </section>

    <!-- 拥有的模块 -->
    <section v-if="props.home" class="sec-block">
      <div class="sec-head">
        <h2>我的模块</h2>
        <span class="sec-tip">点击卡片进入 · 切换不丢失运行状态</span>
      </div>
      <div v-if="modules.length" class="mod-grid anim-stagger">
        <button v-for="m in modules" :key="m.code" type="button" class="card hover-lift mod-card" @click="enter(m)">
          <span class="mod-ico" :class="m.index > 0 ? 'g1' : 'g2'">
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true" v-html="icoPath(m.code)"></svg>
          </span>
          <span class="grow" style="text-align:left">
            <span class="mod-name">
              {{ m.name }}
              <span class="badge badge-dim num">index {{ m.index }}</span>
            </span>
            <span class="mod-url num">{{ m.url || '/portal/' + m.code }}</span>
          </span>
          <svg class="mod-arr" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M5 12h14M13 6l6 6-6 6"/></svg>
        </button>
      </div>
      <div v-else class="card">
        <div class="empty">
          <div class="empty-icon">🗂️</div>
          <div class="empty-title">暂无可用模块</div>
          <div class="empty-sub">请联系管理员为你授予模块权限</div>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
/* 欢迎横幅 */
.hero{
  position:relative;overflow:hidden;border-radius:var(--r-xl);
  padding:32px;color:#fff;background:var(--grad-hero);background-size:180% 180%;
  box-shadow:var(--shadow-pop);
}
.hero::after{
  content:"";position:absolute;width:340px;height:340px;border-radius:50%;
  background:rgba(255,255,255,.14);top:-140px;right:-80px;pointer-events:none;
}
.hero-body{position:relative;z-index:1;display:flex;gap:16px;align-items:flex-end;justify-content:space-between;flex-wrap:wrap}
.hero-text h1{font-size:28px;line-height:38px;font-weight:800}
.hero-text p{opacity:.92;margin:6px 0 0;font-size:var(--fs-body)}
.hero-chips{display:flex;flex-direction:column;gap:8px;align-items:flex-end}
.h-chip{
  display:inline-flex;align-items:center;gap:6px;height:24px;padding:0 10px;
  border-radius:var(--r-pill);background:rgba(255,255,255,.22);
  font-size:var(--fs-cap);font-weight:700;white-space:nowrap;
}

/* 统计卡 */
.stat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:16px;margin-top:24px}
.stat-card{padding:20px;display:flex;gap:14px;align-items:flex-start}
.stat-ico{width:38px;height:38px;border-radius:12px;display:flex;align-items:center;justify-content:center;flex:none}
.stat-ico svg{display:block}
.stat-meta{min-width:0}
.lbl{font-size:var(--fs-cap);color:var(--color-text-3);font-weight:600}
.val{font-family:var(--font-num);font-size:var(--fs-h1);line-height:30px;font-weight:700;margin-top:2px}
.val small{font-size:var(--fs-body);color:var(--color-text-2);font-family:var(--font-ui);font-weight:400}
.sub{font-size:var(--fs-cap);color:var(--color-text-3);margin-top:2px}

/* 区块 */
.sec-block{margin-top:32px}
.sec-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:16px}
.sec-head h2{font-size:var(--fs-h1);line-height:var(--lh-h1)}
.sec-tip{font-size:var(--fs-cap);color:var(--color-text-3)}

/* 模块卡片 */
.mod-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:16px}
.mod-card{display:flex;gap:14px;align-items:flex-start;padding:20px;cursor:pointer;text-align:left;color:inherit;position:relative}
.mod-arr{position:absolute;right:16px;top:18px;color:var(--color-text-3);opacity:0;transform:translateX(-6px);transition:all var(--dur) var(--ease)}
.mod-card:hover .mod-arr{opacity:1;transform:none;color:var(--color-primary)}
.mod-ico{
  width:46px;height:46px;border-radius:14px;display:flex;align-items:center;justify-content:center;
  color:#fff;flex:none;box-shadow:var(--shadow-sm);
}
.mod-ico svg{display:block}
.mod-ico.g1{background:linear-gradient(135deg,#0284C7,#06B6D4)}
.mod-ico.g2{background:linear-gradient(135deg,#EC4899,#F97316)}
.mod-name{display:flex;align-items:center;gap:8px;font-weight:700;font-size:15px}
.mod-url{display:block;font-size:11px;color:var(--color-text-3);margin-top:4px}

@media (max-width:768px){
  .hero{padding:24px}
  .hero-text h1{font-size:22px;line-height:30px}
  .hero-chips{align-items:flex-start}
}
</style>
