import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// In development the license API (../api) answers on :3000; same-origin /api keeps CORS out of the way
export default defineConfig({
  plugins: [react()],
  server: { proxy: { '/api': 'http://localhost:3000' } },
});
