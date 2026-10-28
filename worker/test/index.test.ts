import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import worker from '../src/index';
import { ORIGIN, ctx, env, request } from './helpers';

const call = (req: Request, e = env()) => worker.fetch(req, e, ctx());

describe('worker routing and CORS', () => {
  const fetchMock = vi.fn();
  beforeEach(() => {
    fetchMock.mockReset();
    vi.stubGlobal('fetch', fetchMock);
  });
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('answers a preflight from an allowed origin on any path', async () => {
    for (const path of ['/feedback', '/op1fun/patches', '/anything']) {
      const res = await call(
        request(path, {
          method: 'OPTIONS',
          headers: { 'Access-Control-Request-Method': 'POST', 'Access-Control-Request-Headers': 'content-type' },
        }),
      );
      expect(res.status).toBe(204);
      expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
      expect(res.headers.get('Access-Control-Allow-Methods')).toBe('POST, OPTIONS');
      expect(res.headers.get('Access-Control-Allow-Headers')).toBe('Content-Type');
      expect(res.headers.get('Access-Control-Max-Age')).toBe('86400');
      expect(res.headers.get('Vary')).toBe('Origin');
    }
  });

  it('accepts every configured origin, trimming the list', async () => {
    for (const origin of ['http://localhost:5173', 'http://localhost:4173']) {
      const res = await call(request('/feedback', { method: 'OPTIONS', origin }));
      expect(res.headers.get('Access-Control-Allow-Origin')).toBe(origin);
    }
  });

  it('gives a preflight from another origin no CORS headers', async () => {
    const res = await call(request('/feedback', { method: 'OPTIONS', origin: 'https://evil.example' }));
    expect(res.status).toBe(204);
    expect(res.headers.get('Access-Control-Allow-Origin')).toBeNull();
  });

  it('refuses other origins and requests without an origin with 403', async () => {
    for (const origin of ['https://evil.example', 'https://coldsoldr.github.io.evil.example', 'null', null]) {
      const res = await call(request('/feedback', { origin, body: '{}' }));
      expect(res.status).toBe(403);
      expect(res.headers.get('Access-Control-Allow-Origin')).toBeNull();
      expect(await res.json()).toEqual({ errors: ['origin not allowed'] });
    }
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('matches origins exactly', async () => {
    const res = await call(request('/feedback', { origin: 'https://coldsoldr.github.io/', body: '{}' }));
    expect(res.status).toBe(403);
  });

  it('returns 501 for the op1.fun proxy until Phase 6', async () => {
    const res = await call(request('/op1fun/patches', { body: '{}' }));
    expect(res.status).toBe(501);
    expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
    expect(await res.json()).toEqual({ errors: ['not implemented'] });
  });

  it('returns 404 with CORS headers for unknown routes and methods', async () => {
    for (const [method, path] of [
      ['POST', '/'],
      ['GET', '/feedback'],
      ['PUT', '/feedback'],
      ['POST', '/feedback/extra'],
      ['GET', '/op1fun/patches'],
    ] as const) {
      const res = await call(request(path, { method }));
      expect(res.status, `${method} ${path}`).toBe(404);
      expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
      expect(res.headers.get('Content-Type')).toBe('application/json');
      expect(await res.json()).toEqual({ errors: ['not found'] });
    }
  });
});
