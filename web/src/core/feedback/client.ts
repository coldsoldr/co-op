/**
 * Sending feedback: to the co-op Worker when this build knows its URL and a Turnstile site key,
 * otherwise as a prefilled GitHub issue link the user submits themselves.
 */
import {
  CONTEXT_LIMITS,
  issueBody,
  issueTitle,
  type FeedbackCategory,
  type FeedbackContext,
  type FeedbackPayload,
  type FeedbackStore,
} from '@co-op/shared/feedback';

export const REPO_URL = 'https://github.com/coldsoldr/co-op';
/** Longest prefilled issue link; GitHub and browsers cope with this comfortably. */
export const MAX_ISSUE_URL = 6000;
const TRUNCATED_NOTE = '\n\n(The message was cut to fit in a link. Paste the rest below.)';

export interface FeedbackConfig {
  workerUrl: string | undefined;
  siteKey: string | undefined;
}

/** The build's configuration, from VITE_WORKER_URL and VITE_TURNSTILE_SITE_KEY. */
export const buildConfig: FeedbackConfig = {
  workerUrl: import.meta.env.VITE_WORKER_URL,
  siteKey: import.meta.env.VITE_TURNSTILE_SITE_KEY,
};

/** True when feedback can go through the Worker; false means the GitHub link fallback. */
export function feedbackAvailable(config: FeedbackConfig = buildConfig): boolean {
  return Boolean(config.workerUrl?.trim() && config.siteKey?.trim());
}

/** What the app knows about itself, plus build constants and the user agent. */
export function feedbackContext(
  app: { profile: string | null; store: FeedbackStore; deviceMounted: boolean },
  userAgent: string = typeof navigator === 'undefined' ? '' : navigator.userAgent,
): FeedbackContext {
  return {
    appVersion: __APP_VERSION__,
    commit: __COMMIT__,
    profile: app.profile,
    store: app.store,
    userAgent: userAgent.slice(0, CONTEXT_LIMITS.userAgent),
    deviceMounted: app.deviceMounted,
  };
}

export class FeedbackError extends Error {
  constructor(
    readonly errors: string[],
    /** HTTP status, or null when the Worker could not be reached. */
    readonly status: number | null,
    /** Seconds from Retry-After on a 429. */
    readonly retryAfter: number | null = null,
  ) {
    super(errors.join('; '));
    this.name = 'FeedbackError';
  }
}

const UNEXPECTED = 'the feedback service gave an unexpected answer';

/** Sends feedback to the Worker; resolves with the new issue or throws FeedbackError. */
export async function sendFeedback(
  payload: FeedbackPayload,
  fetchImpl: typeof fetch = fetch,
  workerUrl: string = buildConfig.workerUrl ?? '',
): Promise<{ url: string; number: number }> {
  let res: Response;
  try {
    res = await fetchImpl(`${workerUrl.replace(/\/+$/, '')}/feedback`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
  } catch (_error) {
    throw new FeedbackError(['could not reach the feedback service'], null);
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
    Array.isArray(data.errors) && data.errors.length > 0 && data.errors.every((e) => typeof e === 'string')
      ? (data.errors as string[])
      : [UNEXPECTED];
  const retry = Number(res.headers.get('Retry-After'));
  throw new FeedbackError(errors, res.status, res.status === 429 && Number.isFinite(retry) ? retry : null);
}

function issueUrl(category: FeedbackCategory, message: string, context: FeedbackContext): string {
  const p: FeedbackPayload = { category, message, context, turnstileToken: '' };
  return (
    `${REPO_URL}/issues/new?labels=feedback,${category}` +
    `&title=${encodeURIComponent(issueTitle(p))}&body=${encodeURIComponent(issueBody(p))}`
  );
}

/**
 * A prefilled new-issue link, for builds without the Worker. Labels only apply when the user can
 * triage the repo; the [category] title prefix tells the owner either way. Links longer than
 * MAX_ISSUE_URL keep as much of the message as fits, cut on a code point, with a note.
 */
export function githubIssueUrl(category: FeedbackCategory, message: string, context: FeedbackContext): string {
  const text = message.trim();
  const full = issueUrl(category, text, context);
  if (full.length <= MAX_ISSUE_URL) return full;
  const points = Array.from(text);
  let lo = 0;
  let hi = points.length;
  while (lo < hi) {
    const mid = Math.ceil((lo + hi) / 2);
    const url = issueUrl(category, points.slice(0, mid).join('') + TRUNCATED_NOTE, context);
    if (url.length <= MAX_ISSUE_URL) lo = mid;
    else hi = mid - 1;
  }
  return issueUrl(category, points.slice(0, lo).join('') + TRUNCATED_NOTE, context);
}
