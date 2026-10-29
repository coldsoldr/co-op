/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** The deployed feedback Worker, e.g. https://co-op-feedback.<account>.workers.dev. Unset until deployed. */
  readonly VITE_WORKER_URL?: string;
  /** The Turnstile widget's site key (public). Unset until the owner creates the widget. */
  readonly VITE_TURNSTILE_SITE_KEY?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}

/** The web package version, injected by vite.config.ts. */
declare const __APP_VERSION__: string;
/** Short git sha of the build (GITHUB_SHA in CI), or 'dev'. */
declare const __COMMIT__: string;
