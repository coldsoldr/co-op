import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import worker from '../src/index';
import { MAX_PROXY_BYTES, MAX_UPLOAD_BYTES, OP1FUN_PATCHES } from '../src/op1fun';
import { BASE, ORIGIN, ctx, env, request } from './helpers';
import type { Env } from '../src/env';

const IP = '203.0.113.7';
const S3 = 'https://op1fun.s3.amazonaws.com/patches/kick.aif';
const AIFF_URI = 'data:audio/x-aiff;base64,Rk9STQ==';

const upload = (overrides: Record<string, unknown> = {}) => ({
  data: {
    type: 'patches',
    attributes: { name: 'Kick', public: true, description: '', license: 'CC BY', file: AIFF_URI, ...overrides },
  },
});

const creds = { 'X-User-Email': 'me@example.com', 'X-User-Token': 'secret-token' };

const post = (body: string, headers: Record<string, string> = creds) =>
  request('/op1fun/patches', {
    body,
    headers: { 'Content-Type': 'application/json', 'CF-Connecting-IP': IP, ...headers },
  });

const getFile = (url: string, headers: Record<string, string> = {}) =>
  request(`/op1fun/file?url=${encodeURIComponent(url)}`, { method: 'GET', headers: { 'CF-Connecting-IP': IP, ...headers } });

/** A finite stream of `count` chunks of `size` bytes that records whether it was cancelled. */
function chunks(count: number, size: number) {
  const state = { pulled: 0, cancelled: false };
  const chunk = new Uint8Array(size).fill(0x61);
  const stream = new ReadableStream<Uint8Array>({
    pull(controller) {
      if (state.pulled >= count) {
        controller.close();
        return;
      }
      state.pulled += 1;
      controller.enqueue(chunk);
    },
    cancel() {
      state.cancelled = true;
    },
  });
  return { stream, state };
}

describe('op1.fun routes', () => {
  const fetchMock = vi.fn<(input: string, init: RequestInit) => Promise<Response>>();
  const logs: unknown[] = [];

  beforeEach(() => {
    fetchMock.mockReset();
    vi.stubGlobal('fetch', fetchMock);
    logs.length = 0;
    for (const level of ['log', 'info', 'warn', 'error', 'debug'] as const) {
      vi.spyOn(console, level).mockImplementation((...args: unknown[]) => {
        logs.push(args);
      });
    }
  });

  afterEach(() => {
    // Nothing here may write credentials or bodies to the logs.
    expect(logs).toEqual([]);
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  const call = (req: Request, e: Env = env()) => worker.fetch(req, e, ctx());
  const expectCors = (res: Response) => expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);

  describe('POST /op1fun/patches', () => {
    it('forwards the body and only the two credential headers plus Content-Type, and passes the answer back', async () => {
      const answer = { data: [{ id: '99', type: 'patches', attributes: { name: 'Kick' } }] };
      fetchMock.mockResolvedValue(
        new Response(JSON.stringify(answer), { status: 201, headers: { 'Content-Type': 'application/vnd.api+json', 'Set-Cookie': 'x=1' } }),
      );
      const body = JSON.stringify(upload());
      const res = await call(post(body, { ...creds, Cookie: 'session=1', Authorization: 'Bearer other', 'X-Extra': 'no' }));
      expect(res.status).toBe(201);
      expectCors(res);
      expect(res.headers.get('Content-Type')).toBe('application/vnd.api+json');
      expect(res.headers.get('Set-Cookie')).toBeNull();
      expect(await res.json()).toEqual(answer);

      expect(fetchMock).toHaveBeenCalledTimes(1);
      const [url, init] = fetchMock.mock.calls[0]!;
      expect(url).toBe(OP1FUN_PATCHES);
      expect(OP1FUN_PATCHES).toBe('https://api.op1.fun/v1/patches');
      expect(init.method).toBe('POST');
      expect(init.redirect).toBe('error');
      const headers = new Headers(init.headers);
      expect([...headers.keys()].sort()).toEqual(['content-type', 'x-user-email', 'x-user-token']);
      expect(headers.get('X-User-Email')).toBe('me@example.com');
      expect(headers.get('X-User-Token')).toBe('secret-token');
      expect(headers.get('Content-Type')).toBe('application/json');
      expect(new TextDecoder().decode(init.body as Uint8Array)).toBe(body);
    });

    it('passes upstream errors through unchanged', async () => {
      fetchMock.mockResolvedValue(new Response('{"errors":[{"title":"Unauthorized"}]}', { status: 401 }));
      const res = await call(post(JSON.stringify(upload())));
      expect(res.status).toBe(401);
      expectCors(res);
      expect(await res.text()).toBe('{"errors":[{"title":"Unauthorized"}]}');
    });

    it('requires both credential headers', async () => {
      for (const headers of [{}, { 'X-User-Email': 'me@example.com' }, { 'X-User-Token': 't' }, { 'X-User-Email': '', 'X-User-Token': 't' }]) {
        const res = await call(post(JSON.stringify(upload()), headers));
        expect(res.status).toBe(401);
        expectCors(res);
        expect(await res.json()).toEqual({ errors: ['X-User-Email and X-User-Token are required'] });
      }
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('validates the payload', async () => {
      const bad = [
        'not json',
        '[]',
        JSON.stringify({}),
        JSON.stringify({ data: { type: 'packs', attributes: { file: AIFF_URI } } }),
        JSON.stringify({ data: { type: 'patches' } }),
        JSON.stringify(upload({ file: 'data:audio/mpeg;base64,AAAA' })),
        JSON.stringify(upload({ file: 42 })),
        JSON.stringify(upload({ file: 'https://evil.example/x.aif' })),
      ];
      for (const body of bad) {
        const res = await call(post(body));
        expect(res.status, body).toBe(400);
        expectCors(res);
      }
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('refuses bodies over 13 MiB by declared length and while streaming', async () => {
      expect(MAX_UPLOAD_BYTES).toBe(13 * 1024 * 1024);
      const declared = await call(post('{}', { ...creds, 'Content-Length': String(MAX_UPLOAD_BYTES + 1) }));
      expect(declared.status).toBe(413);
      expectCors(declared);

      const { stream, state } = chunks(20, 1024 * 1024);
      const req = new Request(`${BASE}/op1fun/patches`, {
        method: 'POST',
        headers: { Origin: ORIGIN, 'CF-Connecting-IP': IP, ...creds },
        body: stream,
        duplex: 'half',
      } as RequestInit);
      const res = await call(req);
      expect(res.status).toBe(413);
      expect(state.cancelled).toBe(true);
      expect(state.pulled).toBeLessThanOrEqual(16);
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('answers 502 without detail when op1.fun cannot be reached', async () => {
      fetchMock.mockRejectedValue(new TypeError('network secret-token'));
      const res = await call(post(JSON.stringify(upload())));
      expect(res.status).toBe(502);
      expectCors(res);
      expect(await res.json()).toEqual({ errors: ['could not reach op1.fun'] });
    });

    it('rate limits with its own key prefix', async () => {
      const limit = vi.fn(async () => ({ success: false }));
      const res = await call(post(JSON.stringify(upload())), env({ OP1FUN_LIMIT: { limit } }));
      expect(res.status).toBe(429);
      expect(res.headers.get('Retry-After')).toBe('60');
      expect(limit).toHaveBeenCalledWith({ key: `op1fun-upload:${IP}` });
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('refuses other origins', async () => {
      const res = await call(request('/op1fun/patches', { origin: 'https://evil.example', body: '{}', headers: creds }));
      expect(res.status).toBe(403);
      expect(fetchMock).not.toHaveBeenCalled();
    });
  });

  describe('GET /op1fun/file', () => {
    it('streams an allowed file as an attachment, with no credentials or redirects', async () => {
      fetchMock.mockResolvedValue(
        new Response(new Uint8Array([1, 2, 3]), { status: 200, headers: { 'Content-Type': 'audio/aiff', 'Set-Cookie': 'x=1' } }),
      );
      const res = await call(getFile(S3, { 'X-User-Token': 'secret-token' }));
      expect(res.status).toBe(200);
      expectCors(res);
      expect(res.headers.get('Content-Type')).toBe('audio/aiff');
      expect(res.headers.get('Set-Cookie')).toBeNull();
      expect(res.headers.get('Content-Disposition')).toBe('attachment');
      expect(res.headers.get('Content-Security-Policy')).toBe("default-src 'none'; sandbox");
      expect(res.headers.get('X-Content-Type-Options')).toBe('nosniff');
      expect(new Uint8Array(await res.arrayBuffer())).toEqual(new Uint8Array([1, 2, 3]));
      const [url, init] = fetchMock.mock.calls[0]!;
      expect(url).toBe(S3);
      expect(init.redirect).toBe('manual');
      expect([...new Headers(init.headers).keys()]).toEqual([]);
    });

    it('accepts the allowed hosts and extensions, ignoring case', async () => {
      const ok = [
        S3,
        'https://op1fun.s3.us-east-1.amazonaws.com/a/b.AIFF',
        'https://OP1FUN.S3.AMAZONAWS.COM/a/b.Aif',
        'https://s3.amazonaws.com/op1fun/patches/kick.aif',
        'https://s3.eu-west-1.amazonaws.com/op1fun/previews/kick.mp3',
        'https://op1.fun/files/preview.mp3',
        'https://www.op1.fun/files/kick.aiff?X-Amz-Signature=abc',
      ];
      for (const url of ok) {
        fetchMock.mockResolvedValueOnce(new Response(new Uint8Array([1]), { status: 200 }));
        const res = await call(getFile(url));
        expect(res.status, url).toBe(200);
        expect(res.headers.get('Content-Type')).toBe('application/octet-stream');
      }
    });

    it('passes only audio Content-Types and labels anything else as octet-stream', async () => {
      const cases: [string | null, string][] = [
        ['audio/x-aiff', 'audio/x-aiff'],
        ['Audio/AIFF; charset=binary', 'audio/aiff'],
        ['audio/mpeg', 'audio/mpeg'],
        ['text/html', 'application/octet-stream'],
        ['image/svg+xml', 'application/octet-stream'],
        [null, 'application/octet-stream'],
      ];
      for (const [upstream, expected] of cases) {
        const headers: Record<string, string> = upstream === null ? {} : { 'Content-Type': upstream };
        fetchMock.mockResolvedValueOnce(new Response(new Uint8Array([1]), { status: 200, headers }));
        const res = await call(getFile(S3));
        expect(res.headers.get('Content-Type'), String(upstream)).toBe(expected);
      }
    });

    it('rejects other hosts, schemes, paths and malformed urls with 400', async () => {
      const bad = [
        'https://evil.com/kick.aif',
        'https://amazonaws.com.evil.com/kick.aif',
        'https://bucket.s3.amazonaws.com/kick.aif',
        'https://op1fun-evil.s3.amazonaws.com/kick.aif',
        'https://evilop1fun.s3.amazonaws.com/kick.aif',
        'https://op1fun.s3.amazonaws.com.evil.com/kick.aif',
        'https://ec2.amazonaws.com/kick.aif',
        'https://s3.amazonaws.com/other/kick.aif',
        'https://s3.amazonaws.com/op1funny/kick.aif',
        'https://s3.amazonaws.com/kick.aif',
        'https://op1fun.s3.amazonaws.com/pack.zip',
        'https://op1.fun/pack.zip',
        'https://amazonaws.com/kick.aif',
        'https://evilamazonaws.com/kick.aif',
        'https://op1.fun.evil.com/kick.aif',
        'https://evilop1.fun/kick.aif',
        'http://op1fun.s3.amazonaws.com/kick.aif',
        'ftp://op1fun.s3.amazonaws.com/kick.aif',
        'https://op1fun.s3.amazonaws.com/kick.wav',
        'https://op1fun.s3.amazonaws.com/kick.aif/',
        'https://op1fun.s3.amazonaws.com/kick.html?x=.aif',
        'https://user:pass@op1fun.s3.amazonaws.com/kick.aif',
        'https://op1fun.s3.amazonaws.com:8443/kick.aif',
        'not a url',
        '',
      ];
      for (const url of bad) {
        const res = await call(getFile(url));
        expect(res.status, url).toBe(400);
        expectCors(res);
      }
      const missing = await call(request('/op1fun/file', { method: 'GET' }));
      expect(missing.status).toBe(400);
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('refuses upstream redirects', async () => {
      for (const status of [301, 302, 307, 308]) {
        fetchMock.mockResolvedValueOnce(new Response(null, { status, headers: { Location: 'https://evil.example/x' } }));
        const res = await call(getFile(S3));
        expect(res.status).toBe(502);
        expectCors(res);
        expect(await res.json()).toEqual({ errors: ['op1.fun redirected the file; not followed'] });
      }
      expect(fetchMock).toHaveBeenCalledTimes(4);
    });

    it('reports upstream failures with 502', async () => {
      fetchMock.mockResolvedValueOnce(new Response('nope', { status: 404 }));
      const res = await call(getFile(S3));
      expect(res.status).toBe(502);
      expect(await res.json()).toEqual({ errors: ['op1.fun answered HTTP 404'] });
      fetchMock.mockRejectedValueOnce(new TypeError('network'));
      const down = await call(getFile(S3));
      expect(down.status).toBe(502);
      expect(await down.json()).toEqual({ errors: ['could not reach op1.fun'] });
    });

    it('refuses a declared Content-Length over 20 MB', async () => {
      expect(MAX_PROXY_BYTES).toBe(20 * 1024 * 1024);
      const { stream, state } = chunks(1, 16);
      fetchMock.mockResolvedValue(
        new Response(stream, { status: 200, headers: { 'Content-Length': String(MAX_PROXY_BYTES + 1) } }),
      );
      const res = await call(getFile(S3));
      expect(res.status).toBe(413);
      expectCors(res);
      expect(state.cancelled).toBe(true);
    });

    it('cuts a stream that passes 20 MB without a Content-Length', async () => {
      const { stream, state } = chunks(30, 1024 * 1024);
      fetchMock.mockResolvedValue(new Response(stream, { status: 200 }));
      const res = await call(getFile(S3));
      expect(res.status).toBe(200);
      await expect(res.arrayBuffer()).rejects.toThrow();
      expect(state.pulled).toBeLessThanOrEqual(23);
      expect(state.cancelled).toBe(true);
    });

    it('passes a stream of exactly 20 MB', async () => {
      const { stream } = chunks(20, 1024 * 1024);
      fetchMock.mockResolvedValue(new Response(stream, { status: 200 }));
      const res = await call(getFile(S3));
      expect((await res.arrayBuffer()).byteLength).toBe(MAX_PROXY_BYTES);
    });

    it('rate limits with its own key prefix', async () => {
      const limit = vi.fn(async () => ({ success: false }));
      const res = await call(getFile(S3), env({ OP1FUN_LIMIT: { limit } }));
      expect(res.status).toBe(429);
      expectCors(res);
      expect(limit).toHaveBeenCalledWith({ key: `op1fun-file:${IP}` });
      expect(fetchMock).not.toHaveBeenCalled();
    });

    it('does not use the feedback limiters', async () => {
      const feedback = vi.fn(async () => ({ success: false }));
      fetchMock.mockResolvedValue(new Response('x', { status: 200 }));
      const res = await call(getFile(S3), env({ FEEDBACK_LIMIT: { limit: feedback }, FEEDBACK_GLOBAL_LIMIT: { limit: feedback } }));
      expect(res.status).toBe(200);
      expect(feedback).not.toHaveBeenCalled();
    });

    it('refuses other origins', async () => {
      const res = await call(
        request(`/op1fun/file?url=${encodeURIComponent(S3)}`, { method: 'GET', origin: 'https://evil.example' }),
      );
      expect(res.status).toBe(403);
      expect(fetchMock).not.toHaveBeenCalled();
    });
  });
});
