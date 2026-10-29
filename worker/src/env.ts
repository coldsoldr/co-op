/** Bindings from wrangler.toml ([vars], [[ratelimits]]) and secrets set with `wrangler secret put`. */
export interface Env {
  GITHUB_TOKEN: string;
  TURNSTILE_SECRET: string;
  /** Comma-separated exact origins allowed to call the Worker. */
  ALLOWED_ORIGINS: string;
  /** 'owner/repo' that feedback issues are filed in. */
  GITHUB_REPO: string;
  /** Per-client Workers Rate Limiting binding; absent in tests and when not configured. */
  FEEDBACK_LIMIT?: RateLimiter;
  /** Shared limit across all clients, checked with the single key 'global'. */
  FEEDBACK_GLOBAL_LIMIT?: RateLimiter;
  /** Per-client limit for the op1.fun routes, keyed op1fun-upload:<client> and op1fun-file:<client>. */
  OP1FUN_LIMIT?: RateLimiter;
}

export interface RateLimiter {
  limit(o: { key: string }): Promise<{ success: boolean }>;
}
