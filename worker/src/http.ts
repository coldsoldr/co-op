import type { Env } from './env';

/** The request's Origin if it is exactly one of ALLOWED_ORIGINS, else null. */
export function allowedOrigin(request: Request, env: Env): string | null {
  const origin = request.headers.get('Origin');
  if (origin === null) return null;
  const allowed = env.ALLOWED_ORIGINS.split(',').map((o) => o.trim());
  return allowed.includes(origin) ? origin : null;
}

/** CORS headers for an allowed origin; none at all otherwise. */
export function corsHeaders(origin: string | null): Record<string, string> {
  if (origin === null) return { Vary: 'Origin' };
  return {
    'Access-Control-Allow-Origin': origin,
    'Access-Control-Allow-Methods': 'POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
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
