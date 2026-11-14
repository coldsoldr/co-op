import { describe, expect, it, vi } from 'vitest';
import { FeedbackError } from '../feedback/client';
import { requestIssueUrl, sendPluginRequest } from './request';

const payload = { repo: 'a/b', firmware: 'opus' as const, turnstileToken: 't' };
const answer = (status: number, body: unknown, headers: Record<string, string> = {}) =>
  vi.fn(async () => new Response(JSON.stringify(body), { status, headers }));

describe('plugin requests', () => {
  it('prefills the issue form with the firmware and the link', () => {
    const url = new URL(requestIssueUrl('hundekraft', ' https://github.com/a/b '));
    expect(url.searchParams.get('template')).toBe('plugin-request.yml');
    expect(url.searchParams.get('firmware')).toBe('hundekraft');
    expect(url.searchParams.get('source')).toBe('https://github.com/a/b');
    expect(new URL(requestIssueUrl('opus')).searchParams.has('source')).toBe(false);
  });

  it('posts to the Worker and returns the issue', async () => {
    const fetchImpl = answer(201, { url: 'https://github.com/coldsoldr/co-op/issues/9', number: 9 });
    await expect(sendPluginRequest(payload, fetchImpl, 'https://w.test/')).resolves.toEqual({ url: 'https://github.com/coldsoldr/co-op/issues/9', number: 9 });
    expect(fetchImpl).toHaveBeenCalledWith('https://w.test/plugin-request', expect.objectContaining({ method: 'POST' }));
  });

  it('passes on the Worker’s errors and the wait on a 429', async () => {
    await expect(sendPluginRequest(payload, answer(400, { errors: ['repo must be a GitHub repository link'] }), 'https://w.test')).rejects.toMatchObject({
      errors: ['repo must be a GitHub repository link'],
      status: 400,
    });
    const limited = sendPluginRequest(payload, answer(429, { errors: ['too many'] }, { 'Retry-After': '60' }), 'https://w.test');
    await expect(limited).rejects.toBeInstanceOf(FeedbackError);
    await expect(limited).rejects.toMatchObject({ retryAfter: 60 });
  });
});
