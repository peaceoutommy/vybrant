import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// In development the license API (../api) answers on :3000, and the release feed is proxied too: it only allows the
// live site's origin in a browser, a server-side proxy isn't held to that
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    proxy: {
      '/api': 'http://localhost:3000',
      '/downloads': { target: 'https://download.vybrant.studio', changeOrigin: true, rewrite: p => p.replace(/^\/downloads/, '') },
    },
  },
})
