/**
 * Submitting a plugin by its repo link: to the co-op Worker's /plugin-request when this build has
 * the Worker and a Turnstile site key (no GitHub account needed), otherwise as a prefilled GitHub
 * issue form. Either way a `plugin-request` issue starts the workflow that drafts the listing.
 */
import type { PluginRequestPayload, RequestFirmware } from '@co-op/shared/plugin-request';
import { FeedbackError, REPO_URL, buildConfig } from '../feedback/client';

export const REQUEST_FORM_URL = `${REPO_URL}/issues/new?template=plugin-request.yml`;

/** The GitHub issue form, with the firmware and the repo link filled in. */
export function requestIssueUrl(firmware: RequestFirmware, repoLink = ''): string {
  const params = new URLSearchParams({ firmware });
  if (repoLink.trim()) params.set('source', repoLink.trim());
  return `${REQUEST_FORM_URL}&${params.toString()}`;
}

const UNEXPECTED = 'the submission service gave an unexpected answer';

/** Sends the request to the Worker; resolves with the new issue or throws FeedbackError. */
export async function sendPluginRequest(
  payload: PluginRequestPayload,
  fetchImpl: typeof fetch = fetch,
  workerUrl: string = buildConfig.workerUrl ?? '',
): Promise<{ url: string; number: number }> {
  let res: Response;
  try {
    res = await fetchImpl(`${workerUrl.replace(/\/+$/, '')}/plugin-request`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
  } catch (_error) {
    throw new FeedbackError(['could not reach the submission service'], null);
  }
  let body: unknown;
  try {
    body = await res.json();
  } catch (_error) {
    throw new FeedbackError([UNEXPECTED], res.status);
  }
  const data = (body ?? {}) as { url?: unknown; number?: unknown; errors?: unknown };
  if (res.ok) {
    if (typeof data.url === 'string' && typeof data.number === 'number') return { url: data.url, number: data.number };
    throw new FeedbackError([UNEXPECTED], res.status);
  }
  const errors =
    Array.isArray(data.errors) && data.errors.length > 0 && data.errors.every((e) => typeof e === 'string') ? (data.errors as string[]) : [UNEXPECTED];
  const retry = Number(res.headers.get('Retry-After'));
  throw new FeedbackError(errors, res.status, res.status === 429 && Number.isFinite(retry) ? retry : null);
}
