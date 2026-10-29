import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: 'e2e',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : 'list',
  use: {
    baseURL: 'http://localhost:4173/co-op/',
    trace: 'retain-on-failure',
  },
  projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }],
  webServer: {
    // Built into dist-e2e, never dist, because this build has the test-only Worker URL below and
    // must not be deployed by hand. (Before this, a local e2e run left such a build in web/dist.)
    command:
      'pnpm exec vite build --outDir dist-e2e && pnpm exec tsx scripts/packs.ts build dist-e2e && pnpm exec vite preview --outDir dist-e2e --port 4173 --strictPort',
    url: 'http://localhost:4173/co-op/',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
    // A Worker URL that only exists inside the tests: op1fun.spec.ts answers it with page.route.
    // There is no Turnstile site key, so feedback still falls back to the GitHub issue link.
    env: { VITE_WORKER_URL: 'https://worker.e2e.test' },
  },
});
