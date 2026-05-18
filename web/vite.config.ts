import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'

export default defineConfig({
  plugins: [svelte()],
  server: {
    proxy: {
      '/api/snapshot': { target: 'ws://localhost:7800', ws: true },
      '/api': 'http://localhost:7800',
    },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
})
