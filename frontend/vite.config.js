import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 单页应用:base 恒 '/' (资源一律根绝对路径,由服务端托管)
export default defineConfig({
  plugins: [vue()],
  base: '/',
  build: {
    outDir: 'dist',
    assetsDir: 'assets',
    emptyOutDir: true
  }
})
