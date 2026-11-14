import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import worker from '../src/index';
import { ORIGIN, ctx, env, request } from './helpers';

const SITEVERIFY = 'https://challenges.cloudflare.com/turnstile/v0/siteverify';
const ISSUES = 'https://api.github.com/repos/coldsoldr/co-op/issues';

const body = (overrides: Record<string, unknown> = {}): string =>
  JSON.stringify({ repo: 'https://github.com/VolkerJunginger/OP-1', firmware: 'opus', note: 'Six plugins @everyone', turnstileToken: 'TOKEN', ...overrides });

const post = (text: string) =>
  request('/plugin-request', { body: text, headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': '203.0.113.7' } });

const jsonResponse = (status: number, value: unknown) =>
  new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json' } });

describe('POST /plugin-request', () => {
  const fetchMock = vi.fn<(input: string, init: RequestInit) => Promise<Response>>();
  let verify: () => Response;
  let issue: () => Response;

  beforeEach(() => {
    verify = () => jsonResponse(200, { success: true, hostname: 'coldsoldr.github.io' });
    issue = () => jsonResponse(201, { html_url: 'https://github.com/coldsoldr/co-op/issues/7', number: 7 });
    fetchMock.mockReset();
    fetchMock.mockImplementation(async (input) => {
      if (String(input) === SITEVERIFY) return verify();
      if (String(input) === ISSUES) return issue();
      throw new Error(`unexpected fetch ${String(input)}`);
    });
    vi.stubGlobal('fetch', fetchMock);
  });
  afterEach(() => vi.unstubAllGlobals());

  it('files a plugin-request issue with the repo and the marker, and answers with its link', async () => {
    const res = await worker.fetch(post(body()), env(), ctx());
    expect(res.status).toBe(201);
    expect(await res.json()).toEqual({ url: 'https://github.com/coldsoldr/co-op/issues/7', number: 7 });
    expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
    const sent = JSON.parse(String(fetchMock.mock.calls[1]?.[1].body)) as { title: string; body: string; labels: string[] };
    expect(sent.title).toBe('Plugin: VolkerJunginger/OP-1');
    expect(sent.labels).toEqual(['plugin-request']);
    expect(sent.body).toContain('<!-- co-op-plugin-request repo=VolkerJunginger/OP-1 firmware=opus -->');
    expect(sent.body).not.toContain('TOKEN');
    expect(sent.body).not.toMatch(/(^|[^\u200B])@everyone/);
  });

  it('refuses a link that is not a GitHub repo before verifying', async () => {
    const res = await worker.fetch(post(body({ repo: 'https://example.com/x' })), env(), ctx());
    expect(res.status).toBe(400);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('refuses a failed verification without filing', async () => {
    verify = () => jsonResponse(200, { success: false, hostname: 'coldsoldr.github.io' });
    const res = await worker.fetch(post(body()), env(), ctx());
    expect(res.status).toBe(403);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('says so when GitHub refuses the issue', async () => {
    issue = () => jsonResponse(500, {});
    const res = await worker.fetch(post(body()), env(), ctx());
    expect(res.status).toBe(502);
  });

  it('is rate limited with the feedback limits', async () => {
    const limited = env({ FEEDBACK_LIMIT: { limit: async () => ({ success: false }) } });
    const res = await worker.fetch(post(body()), limited, ctx());
    expect(res.status).toBe(429);
    expect(fetchMock).not.toHaveBeenCalled();
  });
});
