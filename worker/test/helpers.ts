import type { Env } from '../src/env';

export const ORIGIN = 'https://coldsoldr.github.io';
export const BASE = 'https://co-op-feedback.example.workers.dev';

export const env = (overrides: Partial<Env> = {}): Env => ({
  GITHUB_TOKEN: 'ghp_test_token',
  TURNSTILE_SECRET: 'turnstile-secret',
  ALLOWED_ORIGINS: 'https://coldsoldr.github.io, http://localhost:5173,http://localhost:4173',
  GITHUB_REPO: 'coldsoldr/co-op',
  ...overrides,
});

export const ctx = (): ExecutionContext =>
  ({ waitUntil: () => undefined, passThroughOnException: () => undefined }) as unknown as ExecutionContext;

export const request = (
  path: string,
  init: { method?: string; origin?: string | null; headers?: Record<string, string>; body?: string } = {},
): Request => {
  const headers = new Headers(init.headers);
  const origin = init.origin === undefined ? ORIGIN : init.origin;
  if (origin !== null) headers.set('Origin', origin);
  const options: RequestInit = { method: init.method ?? 'POST', headers };
  if (init.body !== undefined) options.body = init.body;
  return new Request(`${BASE}${path}`, options);
};
