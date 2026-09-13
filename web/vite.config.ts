import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'
import { API_PREFIX, daemonPath } from './src/api/apiPath'

export default defineConfig({
  base: '/',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    host: true,
    proxy: {
      [API_PREFIX]: {
        target: 'http://127.0.0.1:8080',
        changeOrigin: false,
        rewrite: daemonPath,
      },
    },
  },
})
