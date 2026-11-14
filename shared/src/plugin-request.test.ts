import { describe, expect, it } from 'vitest';
import { parseRepo, repoFromIssue, requestBody, requestTitle, validatePluginRequest } from './plugin-request';

describe('parseRepo', () => {
  it.each([
    ['https://github.com/VolkerJunginger/OP-1', 'VolkerJunginger/OP-1'],
    ['github.com/a/b.git', 'a/b'],
    ['http://www.github.com/a/b/tree/main/x', 'a/b'],
    ['a-b/c_d.e', 'a-b/c_d.e'],
    ['https://github.com/a/b/releases/latest', 'a/b'],
    ['  https://github.com/a/b?tab=readme  ', 'a/b'],
  ])('reads %s', (input, repo) => expect(parseRepo(input)).toBe(repo));

  it.each(['', 'https://gitlab.com/a/b', 'a', 'https://github.com/a', '-a/b', 'a--b/c', 'a/..', 'evil.com/a/b', 'a/b c'])(
    'refuses %s',
    (input) => expect(parseRepo(input)).toBeNull(),
  );
});

describe('validatePluginRequest', () => {
  const ok = { repo: 'https://github.com/a/b', firmware: 'opus', turnstileToken: 't' };

  it('keeps only known keys, trimmed', () => {
    const r = validatePluginRequest({ ...ok, note: '  hi  ', contact: ' ', extra: 1 });
    expect(r).toEqual({ ok: true, value: { repo: 'a/b', firmware: 'opus', note: 'hi', turnstileToken: 't' } });
  });

  it('refuses co-op itself, a bad firmware and a missing token', () => {
    const r = validatePluginRequest({ repo: 'coldsoldr/co-op', firmware: 'op1', turnstileToken: '' });
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.errors).toHaveLength(3);
  });
});

describe('the issue', () => {
  const p = { repo: 'a/b', firmware: 'hundekraft' as const, note: '@someone see #12\n<b>x</b>', contact: '@me', turnstileToken: 't' };

  it('neutralises mentions and markup and carries the marker', () => {
    const body = requestBody(p);
    expect(requestTitle(p)).toBe('Plugin: a/b');
    expect(body).toContain('<!-- co-op-plugin-request repo=a/b firmware=hundekraft -->');
    expect(body).not.toMatch(/(^|[^\u200B])@someone/);
    expect(body).not.toContain('#12');
    expect(body).not.toContain('<b>');
    expect(body).not.toContain('turnstile');
  });

  it('reads the repo back from the marker, or from an issue form link', () => {
    expect(repoFromIssue(requestBody(p))).toEqual({ repo: 'a/b', firmware: 'hundekraft' });
    const form = '### What should it do?\n\nA delay for opus\n\n### Source\n\nhttps://github.com/c/d\n\nsee https://github.com/coldsoldr/co-op too';
    expect(repoFromIssue(form)).toEqual({ repo: 'c/d', firmware: 'opus' });
    expect(repoFromIssue('only https://github.com/coldsoldr/co-op/issues')).toBeNull();
    expect(repoFromIssue('nothing here')).toBeNull();
  });
});
