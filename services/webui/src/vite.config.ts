import { defineConfig } from 'vite';

export default defineConfig({
  root: '.',
  base: '/voice/static/',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
});
