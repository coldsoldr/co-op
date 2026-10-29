/**
 * POST /feedback: size and JSON checks, shared validation, rate limits, Turnstile, then a GitHub
 * issue. Upstream responses are never passed back to the caller and nothing is logged.
 */
import { issueBody, issueTitle, validateFeedback } from '@co-op/shared/feedback';
import type { Env } from './env';
import { allowedHostnames, clientKey, errors, json } from './http';

export const MAX_BODY_BYTES = 16 * 1024;
export const SUBREQUEST_TIMEOUT_MS = 10_000;
const SITEVERIFY = 'https://challenges.cloudflare.com/turnstile/v0/siteverify';

/**
 * The body as text, or null once it passes MAX_BODY_BYTES. A declared Content-Length over the
 * cap is refused without reading; otherwise the stream is counted as it arrives and cancelled as
 * soon as it goes over, so a missing or lying Content-Length cannot make the Worker buffer more.
 */
async function readCapped(request: Request): Promise<string | null> {
  const declared = Number(request.headers.get('Content-Length'));
  if (Number.isFinite(declared) && declared > MAX_BODY_BYTES) return null;
  if (request.body === null) return '';
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > MAX_BODY_BYTES) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return new TextDecoder().decode(bytes);
}

/** Both limiters, when bound: per client (IPv4 address or IPv6 /64), then one shared key. */
async function rateLimited(env: Env, ip: string | null): Promise<boolean> {
  if (env.FEEDBACK_LIMIT && !(await env.FEEDBACK_LIMIT.limit({ key: clientKey(ip) })).success) return true;
  if (env.FEEDBACK_GLOBAL_LIMIT && !(await env.FEEDBACK_GLOBAL_LIMIT.limit({ key: 'global' })).success) return true;
  return false;
}

/**
 * True only for success: true on a hostname of ALLOWED_ORIGINS. Throws when siteverify cannot
 * answer (network, timeout, non-2xx, non-JSON, or its own internal-error).
 */
async function verifyTurnstile(token: string, ip: string | null, env: Env): Promise<boolean> {
  const form = new URLSearchParams({ secret: env.TURNSTILE_SECRET, response: token });
  if (ip !== null) form.set('remoteip', ip);
  const res = await fetch(SITEVERIFY, {
    method: 'POST',
    body: form,
    signal: AbortSignal.timeout(SUBREQUEST_TIMEOUT_MS),
  });
  if (!res.ok) throw new Error('siteverify unavailable');
  const outcome = (await res.json()) as { success?: unknown; hostname?: unknown; 'error-codes'?: unknown };
  const codes = Array.isArray(outcome['error-codes']) ? outcome['error-codes'] : [];
  if (codes.includes('internal-error')) throw new Error('siteverify internal error');
  return (
    outcome.success === true &&
    typeof outcome.hostname === 'string' &&
    allowedHostnames(env).includes(outcome.hostname)
  );
}

export async function handleFeedback(request: Request, env: Env, origin: string): Promise<Response> {
  const text = await readCapped(request);
  if (text === null) return errors(413, ['feedback is too large'], origin);

  let input: unknown;
  try {
    input = JSON.parse(text);
  } catch (_error) {
    return errors(400, ['body must be JSON'], origin);
  }

  const result = validateFeedback(input);
  if (!result.ok) return errors(400, result.errors, origin);
  const payload = result.value;

  const ip = request.headers.get('CF-Connecting-IP');
  if (await rateLimited(env, ip)) {
    return errors(429, ['too many requests, try again in a minute'], origin, { 'Retry-After': '60' });
  }

  let verified: boolean;
  try {
    verified = await verifyTurnstile(payload.turnstileToken, ip, env);
  } catch (_error) {
    return errors(502, ['could not verify, try again later'], origin);
  }
  if (!verified) return errors(403, ['verification failed'], origin);

  const failed = () => errors(502, ['could not file feedback'], origin);
  try {
    const res = await fetch(`https://api.github.com/repos/${env.GITHUB_REPO}/issues`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${env.GITHUB_TOKEN}`,
        Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'User-Agent': 'co-op-feedback-worker',
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        title: issueTitle(payload),
        body: issueBody(payload),
        labels: ['feedback', payload.category],
      }),
      signal: AbortSignal.timeout(SUBREQUEST_TIMEOUT_MS),
    });
    if (!res.ok) return failed();
    const issue = (await res.json()) as { html_url?: unknown; number?: unknown };
    if (typeof issue.html_url !== 'string' || typeof issue.number !== 'number') return failed();
    return json(201, { url: issue.html_url, number: issue.number }, origin);
  } catch (_error) {
    return failed();
  }
}
