import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { issueBody, issueTitle, validateFeedback, type FeedbackPayload } from '@co-op/shared/feedback';
import worker from '../src/index';
import { MAX_BODY_BYTES } from '../src/feedback';
import { ORIGIN, ctx, env, request } from './helpers';
import type { Env } from '../src/env';

const SITEVERIFY = 'https://challenges.cloudflare.com/turnstile/v0/siteverify';
const ISSUES = 'https://api.github.com/repos/coldsoldr/co-op/issues';
const IP = '203.0.113.7';

const feedback = (overrides: Record<string, unknown> = {}): Record<string, unknown> => ({
  category: 'idea',
  message: 'Please add a randomise button @everyone #12',
  contact: 'someone@example.com',
  context: {
    appVersion: '0.5.0',
    commit: 'abc1234',
    profile: 'op1/stock',
    store: 'drop',
    userAgent: 'Mozilla/5.0',
    deviceMounted: false,
  },
  turnstileToken: 'TURNSTILE-TOKEN-XYZ',
  ...overrides,
});

const post = (body: string, headers: Record<string, string> = {}) =>
  request('/feedback', { body, headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': IP, ...headers } });

const jsonResponse = (status: number, body: unknown) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });

type Route = (init: RequestInit) => Response | Promise<Response>;

describe('POST /feedback', () => {
  const fetchMock = vi.fn<(input: string, init: RequestInit) => Promise<Response>>();
  let routes: Record<string, Route>;
  const logs: unknown[] = [];

  beforeEach(() => {
    routes = {
      [SITEVERIFY]: () => jsonResponse(200, { success: true }),
      [ISSUES]: () => jsonResponse(201, { html_url: 'https://github.com/coldsoldr/co-op/issues/42', number: 42 }),
    };
    fetchMock.mockReset();
    fetchMock.mockImplementation(async (input, init) => {
      const handler = routes[String(input)];
      if (!handler) throw new Error(`unexpected fetch ${String(input)}`);
      return handler(init);
    });
    vi.stubGlobal('fetch', fetchMock);
    logs.length = 0;
    for (const level of ['log', 'info', 'warn', 'error', 'debug'] as const) {
      vi.spyOn(console, level).mockImplementation((...args: unknown[]) => {
        logs.push(args);
      });
    }
  });

  afterEach(() => {
    // Nothing the Worker does may write request content to the logs.
    expect(logs).toEqual([]);
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  const call = (req: Request, e: Env = env()) => worker.fetch(req, e, ctx());
  const callsTo = (url: string) => fetchMock.mock.calls.filter(([input]) => String(input) === url);
  const expectCors = (res: Response) => expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
  const parsed = (overrides: Record<string, unknown> = {}): FeedbackPayload => {
    const result = validateFeedback(feedback(overrides));
    if (!result.ok) throw new Error('fixture invalid');
    return result.value;
  };

  it('verifies the token, files the issue and returns its url and number', async () => {
    const res = await call(post(JSON.stringify(feedback())));
    expect(res.status).toBe(201);
    expectCors(res);
    expect(await res.json()).toEqual({ url: 'https://github.com/coldsoldr/co-op/issues/42', number: 42 });
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(String(fetchMock.mock.calls[0]?.[0])).toBe(SITEVERIFY);
    expect(String(fetchMock.mock.calls[1]?.[0])).toBe(ISSUES);
  });

  it('sends the secret, token and client ip to siteverify', async () => {
    await call(post(JSON.stringify(feedback())));
    const [call0] = callsTo(SITEVERIFY);
    const init = call0?.[1];
    expect(init?.method).toBe('POST');
    const form = new URLSearchParams(String(init?.body));
    expect(Object.fromEntries(form)).toEqual({
      secret: 'turnstile-secret',
      response: 'TURNSTILE-TOKEN-XYZ',
      remoteip: IP,
    });
  });

  it('omits remoteip when the client ip is unknown', async () => {
    const res = await call(request('/feedback', { body: JSON.stringify(feedback()) }));
    expect(res.status).toBe(201);
    const form = new URLSearchParams(String(callsTo(SITEVERIFY)[0]?.[1].body));
    expect(form.has('remoteip')).toBe(false);
  });

  it('sends exactly the expected GitHub request', async () => {
    await call(post(JSON.stringify(feedback())));
    const [issueCall] = callsTo(ISSUES);
    const init = issueCall?.[1];
    expect(init?.method).toBe('POST');
    expect(Object.fromEntries(new Headers(init?.headers))).toEqual({
      authorization: 'Bearer ghp_test_token',
      accept: 'application/vnd.github+json',
      'x-github-api-version': '2022-11-28',
      'user-agent': 'co-op-feedback-worker',
      'content-type': 'application/json',
    });
    const raw = String(init?.body);
    expect(JSON.parse(raw)).toEqual({
      title: issueTitle(parsed()),
      body: issueBody(parsed()),
      labels: ['feedback', 'idea'],
    });
    expect(raw).not.toContain('ghp_test_token');
    expect(raw).not.toContain('TURNSTILE-TOKEN-XYZ');
    expect(raw).not.toContain('turnstile-secret');
    expect(raw).not.toMatch(/@everyone|#12/);
  });

  it('labels the issue with its category', async () => {
    for (const category of ['bug', 'idea', 'pack-request', 'other']) {
      fetchMock.mockClear();
      await call(post(JSON.stringify(feedback({ category }))));
      const body = JSON.parse(String(callsTo(ISSUES)[0]?.[1].body)) as { labels: string[] };
      expect(body.labels).toEqual(['feedback', category]);
    }
  });

  it('files in the configured repository', async () => {
    routes['https://api.github.com/repos/someone/fork/issues'] = routes[ISSUES]!;
    const res = await call(post(JSON.stringify(feedback())), env({ GITHUB_REPO: 'someone/fork' }));
    expect(res.status).toBe(201);
    expect(callsTo('https://api.github.com/repos/someone/fork/issues')).toHaveLength(1);
  });

  it('rejects bodies over 16 KB with 413 by declared length', async () => {
    const res = await call(post('{}', { 'Content-Length': String(MAX_BODY_BYTES + 1) }));
    expect(res.status).toBe(413);
    expectCors(res);
    expect(await res.json()).toEqual({ errors: ['feedback is too large'] });
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('rejects bodies over 16 KB with 413 by actual size', async () => {
    expect(MAX_BODY_BYTES).toBe(16 * 1024);
    const big = JSON.stringify(feedback({ message: 'é'.repeat(MAX_BODY_BYTES / 2) }));
    const res = await call(post(big));
    expect(res.status).toBe(413);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('rejects non-JSON with 400', async () => {
    for (const body of ['', 'not json', '{"category":', '<xml/>']) {
      const res = await call(post(body));
      expect(res.status).toBe(400);
      expectCors(res);
      expect(await res.json()).toEqual({ errors: ['body must be JSON'] });
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('returns validation errors with 400', async () => {
    const res = await call(post(JSON.stringify(feedback({ category: 'praise', message: 'short' }))));
    expect(res.status).toBe(400);
    expectCors(res);
    expect(await res.json()).toEqual({
      errors: ['category must be one of bug, idea, pack-request, other', 'message must be 10 to 5000 characters'],
    });
    const nullRes = await call(post('null'));
    expect(nullRes.status).toBe(400);
    expect(await nullRes.json()).toEqual({ errors: ['feedback must be an object'] });
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('rate limits by client ip with 429 and Retry-After', async () => {
    const limit = vi.fn(async () => ({ success: false }));
    const res = await call(post(JSON.stringify(feedback())), env({ FEEDBACK_LIMIT: { limit } }));
    expect(res.status).toBe(429);
    expectCors(res);
    expect(res.headers.get('Retry-After')).toBe('60');
    expect(await res.json()).toEqual({ errors: ['too many requests, try again in a minute'] });
    expect(limit).toHaveBeenCalledWith({ key: IP });
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('carries on when the rate limiter allows the request', async () => {
    const limit = vi.fn(async () => ({ success: true }));
    const res = await call(post(JSON.stringify(feedback())), env({ FEEDBACK_LIMIT: { limit } }));
    expect(res.status).toBe(201);
    expect(limit).toHaveBeenCalledTimes(1);
  });

  it('turns an unexpected failure into a quiet 500 with CORS headers', async () => {
    const limit = vi.fn(async (): Promise<{ success: boolean }> => {
      throw new Error(`limiter broke on ${IP}`);
    });
    const res = await call(post(JSON.stringify(feedback())), env({ FEEDBACK_LIMIT: { limit } }));
    expect(res.status).toBe(500);
    expectCors(res);
    expect(await res.json()).toEqual({ errors: ['internal error'] });
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('refuses a failed Turnstile check with 403 and files nothing', async () => {
    routes[SITEVERIFY] = () => jsonResponse(200, { success: false, 'error-codes': ['invalid-input-response'] });
    const res = await call(post(JSON.stringify(feedback())));
    expect(res.status).toBe(403);
    expectCors(res);
    expect(await res.json()).toEqual({ errors: ['verification failed'] });
    expect(callsTo(ISSUES)).toHaveLength(0);
  });

  it('treats an unreachable or broken siteverify as 502 and files nothing', async () => {
    for (const route of [
      () => jsonResponse(500, { success: true }),
      () => new Response('<html>', { status: 200 }),
      () => {
        throw new TypeError('network down');
      },
    ]) {
      routes[SITEVERIFY] = route;
      const res = await call(post(JSON.stringify(feedback())));
      expect(res.status).toBe(502);
      expectCors(res);
      expect(await res.json()).toEqual({ errors: ['could not verify, try again later'] });
    }
    expect(callsTo(ISSUES)).toHaveLength(0);
  });

  it('answers 502 without leaking the GitHub response', async () => {
    routes[ISSUES] = () => jsonResponse(401, { message: 'Bad credentials', documentation_url: 'https://docs.github.com' });
    const res = await call(post(JSON.stringify(feedback())));
    expect(res.status).toBe(502);
    expectCors(res);
    const text = await res.text();
    expect(JSON.parse(text)).toEqual({ errors: ['could not file feedback'] });
    expect(text).not.toContain('Bad credentials');
  });

  it('answers 502 when GitHub is unreachable or returns no issue', async () => {
    for (const route of [
      () => {
        throw new TypeError('network down');
      },
      () => jsonResponse(201, { unexpected: true }),
      () => new Response('not json', { status: 201 }),
    ]) {
      routes[ISSUES] = route;
      const res = await call(post(JSON.stringify(feedback())));
      expect(res.status).toBe(502);
      expect(await res.json()).toEqual({ errors: ['could not file feedback'] });
    }
  });

  it('ignores extra keys rather than forwarding them', async () => {
    const res = await call(post(JSON.stringify(feedback({ labels: ['security'], assignees: ['octocat'] }))));
    expect(res.status).toBe(201);
    const body = JSON.parse(String(callsTo(ISSUES)[0]?.[1].body)) as Record<string, unknown>;
    expect(Object.keys(body).sort()).toEqual(['body', 'labels', 'title']);
    expect(body.labels).toEqual(['feedback', 'idea']);
  });

  it('still refuses disallowed origins before reading the body', async () => {
    const res = await call(
      request('/feedback', { origin: 'https://evil.example', body: JSON.stringify(feedback()) }),
    );
    expect(res.status).toBe(403);
    expect(fetchMock).not.toHaveBeenCalled();
  });
});
