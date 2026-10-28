/** Bindings from wrangler.toml ([vars], [[ratelimits]]) and secrets set with `wrangler secret put`. */
export interface Env {
  GITHUB_TOKEN: string;
  TURNSTILE_SECRET: string;
  /** Comma-separated exact origins allowed to call the Worker. */
  ALLOWED_ORIGINS: string;
  /** 'owner/repo' that feedback issues are filed in. */
  GITHUB_REPO: string;
  /** Workers Rate Limiting binding; absent in tests and when not configured. */
  FEEDBACK_LIMIT?: { limit(o: { key: string }): Promise<{ success: boolean }> };
}
