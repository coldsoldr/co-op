import { configDefaults, defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: '/co-op/',
  plugins: [react()],
  test: {
    environment: 'node',
    include: ['src/**/*.test.{ts,tsx}'],
    // Playwright specs run with pnpm e2e, never under vitest.
    exclude: [...configDefaults.exclude, 'e2e/**'],
  },
});
