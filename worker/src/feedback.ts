/**
 * POST /feedback: size and JSON checks, shared validation, per-IP rate limit, Turnstile, then a
 * GitHub issue. Upstream responses are never passed back to the caller and nothing is logged.
 */
import { issueBody, issueTitle, validateFeedback } from '@co-op/shared/feedback';
import type { Env } from './env';
import { errors, json } from './http';

export const MAX_BODY_BYTES = 16 * 1024;
const SITEVERIFY = 'https://challenges.cloudflare.com/turnstile/v0/siteverify';

const tooLarge = (origin: string) => errors(413, ['feedback is too large'], origin);

/** True only for a 2xx siteverify answer with success: true; throws when it cannot be asked. */
async function verifyTurnstile(token: string, ip: string | null, env: Env): Promise<boolean> {
  const form = new URLSearchParams({ secret: env.TURNSTILE_SECRET, response: token });
  if (ip !== null) form.set('remoteip', ip);
  const res = await fetch(SITEVERIFY, { method: 'POST', body: form });
  if (!res.ok) throw new Error('siteverify unavailable');
  const outcome = (await res.json()) as { success?: unknown };
  return outcome.success === true;
}

export async function handleFeedback(request: Request, env: Env, origin: string): Promise<Response> {
  const declared = Number(request.headers.get('Content-Length') ?? '0');
  if (declared > MAX_BODY_BYTES) return tooLarge(origin);
  const text = await request.text();
  if (new TextEncoder().encode(text).byteLength > MAX_BODY_BYTES) return tooLarge(origin);

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
  if (env.FEEDBACK_LIMIT) {
    const { success } = await env.FEEDBACK_LIMIT.limit({ key: ip ?? 'unknown' });
    if (!success) {
      return errors(429, ['too many requests, try again in a minute'], origin, { 'Retry-After': '60' });
    }
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
    });
    if (!res.ok) return failed();
    const issue = (await res.json()) as { html_url?: unknown; number?: unknown };
    if (typeof issue.html_url !== 'string' || typeof issue.number !== 'number') return failed();
    return json(201, { url: issue.html_url, number: issue.number }, origin);
  } catch (_error) {
    return failed();
  }
}
