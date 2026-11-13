// Admin console build. Development: `npm run admin:dev` serves it on http://localhost:4200 (the redirect the
// organization Keycloak client accepts) and proxies the API to the control plane on SKOWORK_CP (default :8080).
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { fileURLToPath } from 'node:url';

const cp = process.env.SKOWORK_CP ?? 'http://localhost:8080';

export default defineConfig(({ command }) => ({
  root: fileURLToPath(new URL('.', import.meta.url)),
  base: command === 'build' ? '/admin/' : '/',
  plugins: [react()],
  build: { outDir: fileURLToPath(new URL('../dist/admin', import.meta.url)), emptyOutDir: true },
  server: {
    port: 4200,
    strictPort: true,
    // IPv4 loopback: "localhost" can resolve to ::1 only, which browsers that use 127.0.0.1 for localhost miss.
    host: '127.0.0.1',
    proxy: { '/api': cp, '/llm': cp },
  },
}));
