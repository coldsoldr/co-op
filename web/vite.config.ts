import { configDefaults, defineConfig, type Plugin } from 'vitest/config';
import react from '@vitejs/plugin-react';
// Importing the packs library pulls the codec into the config bundle. Vite then prints a notice
// that these extensionless imports are "unsupported by configLoader: 'native'". That loader is
// opt-in today, so the notice is harmless; if a future Vite makes it the default, add file
// extensions to these imports (or load the plugin another way). Do not silence it globally with
// VITE_CONFIG_NATIVE_IGNORE_WARNING, which would hide other config warnings too.
import { DEFAULT_PACKS_DIR, packsMiddleware } from './scripts/packs-lib';

const base = '/co-op/';

/**
 * Serves `/co-op/packs/index.json` (built on the fly) and the pack files from ../packs during
 * `vite dev`, so the catalogue works without a build. The production build writes the same files
 * into dist/packs with scripts/packs.ts.
 */
function communityPacks(): Plugin {
  return {
    name: 'co-op-community-packs',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use(
        packsMiddleware(DEFAULT_PACKS_DIR, { base, warn: (message) => server.config.logger.warn(message) }),
      );
    },
  };
}

export default defineConfig({
  base,
  plugins: [react(), communityPacks()],
  test: {
    environment: 'node',
    include: ['src/**/*.test.{ts,tsx}', 'scripts/**/*.test.ts'],
    // Playwright specs run with pnpm e2e, never under vitest.
    exclude: [...configDefaults.exclude, 'e2e/**'],
  },
});
