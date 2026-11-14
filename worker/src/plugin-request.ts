/**
 * POST /plugin-request: an author's GitHub repo link from the Submit a plugin page, filed as a
 * `plugin-request` issue. The same checks as feedback (size, JSON, validation, the shared rate
 * limits, Turnstile); the plugin-request workflow takes it from the issue. Nothing is logged.
 */
import { REQUEST_LABEL, requestBody, requestTitle, validatePluginRequest } from '@co-op/shared/plugin-request';
import type { Env } from './env';
import { SUBREQUEST_TIMEOUT_MS, rateLimited, readText, verifyTurnstile } from './feedback';
import { errors, json } from './http';

export async function handlePluginRequest(request: Request, env: Env, origin: string): Promise<Response> {
  const text = await readText(request);
  if (text === null) return errors(413, ['the request is too large'], origin);

  let input: unknown;
  try {
    input = JSON.parse(text);
  } catch (_error) {
    return errors(400, ['body must be JSON'], origin);
  }

  const result = validatePluginRequest(input);
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

  const failed = () => errors(502, ['could not file the request'], origin);
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
      body: JSON.stringify({ title: requestTitle(payload), body: requestBody(payload), labels: [REQUEST_LABEL] }),
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
