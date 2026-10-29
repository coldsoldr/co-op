import { describe, it, expect } from 'vitest';
import type { FeedbackContext, FeedbackPayload } from '@co-op/shared/feedback';
import {
  FeedbackError,
  MAX_ISSUE_URL,
  feedbackAvailable,
  feedbackContext,
  feedbackErrorMessages,
  githubIssueUrl,
  sendFeedback,
} from './client';

const WORKER = 'https://co-op-feedback.example.workers.dev';

const context: FeedbackContext = {
  appVersion: '0.5.0',
  commit: 'abc1234',
  profile: 'op1/stock',
  store: 'fs-access',
  userAgent: 'Mozilla/5.0 Test',
  deviceMounted: true,
};

const payload: FeedbackPayload = {
  category: 'bug',
  message: 'The library does not refresh after a move.',
  context,
  turnstileToken: 'token-1',
};

interface Call {
  url: string;
  init: RequestInit | undefined;
}

function fakeFetch(respond: () => Response | Promise<Response>): { fetch: typeof fetch; calls: Call[] } {
  const calls: Call[] = [];
  const impl = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    calls.push({ url: String(input), init });
    return respond();
  };
  return { fetch: impl as typeof fetch, calls };
}

const json = (status: number, body: unknown) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });

describe('feedbackAvailable', () => {
  it('needs both the Worker URL and the Turnstile site key', () => {
    expect(feedbackAvailable({ workerUrl: WORKER, siteKey: 'key' })).toBe(true);
    expect(feedbackAvailable({ workerUrl: WORKER, siteKey: '' })).toBe(false);
    expect(feedbackAvailable({ workerUrl: '', siteKey: 'key' })).toBe(false);
    expect(feedbackAvailable({ workerUrl: undefined, siteKey: undefined })).toBe(false);
    expect(feedbackAvailable({ workerUrl: '  ', siteKey: 'key' })).toBe(false);
  });

  it('is off in a build without the variables', () => {
    expect(feedbackAvailable()).toBe(false);
  });
});

describe('feedbackContext', () => {
  it('adds the build constants and user agent to what the app knows', () => {
    const c = feedbackContext({ profile: 'op1/stock', store: 'drop', deviceMounted: true }, 'UA/1');
    expect(c).toEqual({
      appVersion: expect.stringMatching(/^\d+\.\d+\.\d+/),
      commit: expect.any(String),
      profile: 'op1/stock',
      store: 'drop',
      userAgent: 'UA/1',
      deviceMounted: true,
    });
    expect(c.commit.length).toBeGreaterThan(0);
  });

  it('caps the user agent to what the Worker accepts', () => {
    expect(feedbackContext({ profile: null, store: 'none', deviceMounted: false }, 'u'.repeat(900)).userAgent).toHaveLength(
      500,
    );
  });
});

describe('sendFeedback', () => {
  it('posts the payload as JSON to the Worker and returns the issue', async () => {
    const { fetch, calls } = fakeFetch(() => json(201, { url: 'https://github.com/coldsoldr/co-op/issues/7', number: 7 }));
    await expect(sendFeedback(payload, fetch, `${WORKER}/`)).resolves.toEqual({
      url: 'https://github.com/coldsoldr/co-op/issues/7',
      number: 7,
    });
    expect(calls).toHaveLength(1);
    expect(calls[0]?.url).toBe(`${WORKER}/feedback`);
    expect(calls[0]?.init?.method).toBe('POST');
    expect(new Headers(calls[0]?.init?.headers).get('Content-Type')).toBe('application/json');
    expect(JSON.parse(String(calls[0]?.init?.body))).toEqual(payload);
  });

  it('throws the server errors with the status', async () => {
    const { fetch } = fakeFetch(() => json(400, { errors: ['message must be 10 to 5000 characters'] }));
    const error = await sendFeedback(payload, fetch, WORKER).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(FeedbackError);
    expect((error as FeedbackError).errors).toEqual(['message must be 10 to 5000 characters']);
    expect((error as FeedbackError).status).toBe(400);
  });

  it('explains rate limiting using Retry-After', async () => {
    const { fetch } = fakeFetch(
      () =>
        new Response(JSON.stringify({ errors: ['too many requests, try again in a minute'] }), {
          status: 429,
          headers: { 'Retry-After': '60' },
        }),
    );
    const error = (await sendFeedback(payload, fetch, WORKER).catch((e: unknown) => e)) as FeedbackError;
    expect(error.status).toBe(429);
    expect(error.retryAfter).toBe(60);
  });

  it('gives a generic error for a non-JSON or unexpected answer', async () => {
    for (const response of [
      () => new Response('<html>oops</html>', { status: 500 }),
      () => json(201, { nothing: true }),
      () => json(500, { errors: 'not a list' }),
    ]) {
      const { fetch } = fakeFetch(response);
      const error = (await sendFeedback(payload, fetch, WORKER).catch((e: unknown) => e)) as FeedbackError;
      expect(error).toBeInstanceOf(FeedbackError);
      expect(error.errors).toEqual(['the feedback service gave an unexpected answer']);
    }
  });

  it('reports a network failure', async () => {
    const { fetch } = fakeFetch(() => {
      throw new TypeError('Failed to fetch');
    });
    const error = (await sendFeedback(payload, fetch, WORKER).catch((e: unknown) => e)) as FeedbackError;
    expect(error).toBeInstanceOf(FeedbackError);
    expect(error.errors).toEqual(['could not reach the feedback service']);
    expect(error.status).toBeNull();
  });
});

describe('feedbackErrorMessages', () => {
  it('passes server errors through', () => {
    expect(feedbackErrorMessages(new FeedbackError(['verification failed'], 403))).toEqual(['verification failed']);
  });

  it('uses Retry-After for rate limiting', () => {
    expect(feedbackErrorMessages(new FeedbackError(['x'], 429, 60))).toEqual([
      'Too much feedback from here just now. Try again in 60 seconds.',
    ]);
    expect(feedbackErrorMessages(new FeedbackError(['x'], 429, 1))).toEqual([
      'Too much feedback from here just now. Try again in 1 second.',
    ]);
    expect(feedbackErrorMessages(new FeedbackError(['x'], 429, null))).toEqual([
      'Too much feedback from here just now. Try again in a minute.',
    ]);
  });

  it('has a fallback for anything else', () => {
    expect(feedbackErrorMessages(new Error('boom'))).toEqual(['Something went wrong sending feedback.']);
  });
});

describe('githubIssueUrl', () => {
  const query = (url: string) => new URL(url).searchParams;

  it('prefills a new issue with labels, title and body', () => {
    const url = githubIssueUrl('idea', 'Sort by date please\nwould help a lot', context);
    expect(url.startsWith('https://github.com/coldsoldr/co-op/issues/new?labels=feedback,idea&title=')).toBe(true);
    const q = query(url);
    expect(q.get('labels')).toBe('feedback,idea');
    expect(q.get('title')).toBe('[idea] Sort by date please');
    expect(q.get('body')).toContain('> Sort by date please\n> would help a lot');
    expect(q.get('body')).toContain('| Profile | op1/stock |');
    expect(url).toContain(encodeURIComponent('Sort by date please'));
  });

  it('caps the URL by truncating the message, with a note', () => {
    const long = 'é'.repeat(4000);
    const url = githubIssueUrl('bug', long, context);
    expect(url.length).toBeLessThanOrEqual(MAX_ISSUE_URL);
    const body = query(url).get('body') ?? '';
    expect(body).toContain('The message was cut to fit in a link');
    expect(body).toContain('| Commit | abc1234 |');
    expect(body.length).toBeGreaterThan(500);
  });

  it('does not truncate a message that fits', () => {
    const url = githubIssueUrl('other', 'x'.repeat(1000), context);
    expect(query(url).get('body')).not.toContain('cut to fit');
  });

  it('never throws on lone surrogates, replacing them', () => {
    const lone = String.fromCharCode(0xd83c);
    const url = githubIssueUrl('bug', `broken ${lone} text that is long enough`, context);
    expect(url).toContain(encodeURIComponent('\uFFFD'));
    expect(() => githubIssueUrl('bug', `${lone}`.repeat(4000), context)).not.toThrow();
  });

  it('truncates on grapheme cluster boundaries', () => {
    const family = '👨\u200D👩\u200D👧';
    expect(family).toHaveLength(8);
    const url = githubIssueUrl('bug', family.repeat(2000), context);
    expect(url.length).toBeLessThanOrEqual(MAX_ISSUE_URL);
    const body = new URL(url).searchParams.get('body') ?? '';
    const quoted = body.split('\n').find((l) => l.startsWith(`> ${family}`)) ?? '';
    const kept = quoted.slice(2);
    expect(kept.length).toBeGreaterThan(0);
    expect(kept.length % family.length).toBe(0);
  });

  it('truncates on code point boundaries', () => {
    const url = githubIssueUrl('bug', '🎹'.repeat(3000), context);
    expect(url.length).toBeLessThanOrEqual(MAX_ISSUE_URL);
    expect(() => decodeURIComponent(url)).not.toThrow();
  });
});
