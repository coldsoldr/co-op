import type { Env } from './env';

/** The exact origins in ALLOWED_ORIGINS, ignoring blank entries. */
export const allowedOrigins = (env: Env): string[] =>
  env.ALLOWED_ORIGINS.split(',')
    .map((o) => o.trim())
    .filter(Boolean);

/** The request's Origin if it is exactly one of ALLOWED_ORIGINS, else null. */
export function allowedOrigin(request: Request, env: Env): string | null {
  const origin = request.headers.get('Origin');
  if (!origin) return null;
  return allowedOrigins(env).includes(origin) ? origin : null;
}

/** Hostnames of ALLOWED_ORIGINS, for checking where a Turnstile token was solved. */
export function allowedHostnames(env: Env): string[] {
  const hosts: string[] = [];
  for (const origin of allowedOrigins(env)) {
    try {
      hosts.push(new URL(origin).hostname);
    } catch (_error) {
      // A malformed entry allows nothing.
    }
  }
  return hosts;
}

/** CORS headers for an allowed origin; none at all otherwise. */
export function corsHeaders(origin: string | null): Record<string, string> {
  if (origin === null) return { Vary: 'Origin' };
  return {
    'Access-Control-Allow-Origin': origin,
    'Access-Control-Allow-Methods': 'POST, OPTIONS',
    // X-User-Email and X-User-Token are for the op1.fun upload proxy (Phase 6).
    'Access-Control-Allow-Headers': 'Content-Type, X-User-Email, X-User-Token',
    'Access-Control-Expose-Headers': 'Retry-After',
    'Access-Control-Max-Age': '86400',
    Vary: 'Origin',
  };
}

export function json(
  status: number,
  body: unknown,
  origin: string | null,
  extra: Record<string, string> = {},
): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json', ...corsHeaders(origin), ...extra },
  });
}

export const errors = (status: number, messages: string[], origin: string | null, extra?: Record<string, string>) =>
  json(status, { errors: messages }, origin, extra);

/**
 * The rate limit key for a client: an IPv4 address as is, an IPv6 address by its /64 prefix
 * (one subscriber usually holds a whole /64), anything unparseable as given.
 */
export function clientKey(ip: string | null): string {
  if (ip === null || ip === '') return 'unknown';
  if (!ip.includes(':')) return ip;
  const parts = ip.toLowerCase().split('::');
  if (parts.length > 2) return ip.toLowerCase();
  const groups = (s: string | undefined) => (s ? s.split(':') : []);
  const head = groups(parts[0]);
  const tail = groups(parts[1]);
  const missing = 8 - head.length - tail.length;
  const all = parts.length === 2 ? [...head, ...Array<string>(Math.max(missing, 0)).fill('0'), ...tail] : head;
  const prefix = all.slice(0, 4);
  if (prefix.length < 4 || prefix.some((g) => !/^[0-9a-f]{1,4}$/.test(g))) return ip.toLowerCase();
  return `${prefix.map((g) => parseInt(g, 16).toString(16)).join(':')}::/64`;
}
