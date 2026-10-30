import { describe, expect, it } from 'vitest';
import { RELEASES, parseRelease, parseReleases } from './releases';

describe('parseRelease', () => {
  it('splits a notes file into its parts', () => {
    const text = '# co-op V0.8.0\r\n\r\nReleased 2026-10-30.\r\n\r\n## Plugins\r\n\r\n- One.\r\n';
    expect(parseRelease('a/v0.8.0.md', text)).toEqual({ version: '0.8.0', title: 'V0.8.0', date: '2026-10-30', body: '## Plugins\n\n- One.' });
  });

  it('keeps a heading with more to say, and copes with no date', () => {
    expect(parseRelease('v0.5.0.md', '# co-op V0.5.0 and earlier\n\nThe first versions.\n')).toEqual({
      version: '0.5.0',
      title: 'V0.5.0 and earlier',
      date: null,
      body: 'The first versions.',
    });
  });

  it('ignores a file that is not named for a version', () => {
    expect(parseRelease('notes.md', '# Hello')).toBeNull();
  });
});

describe('parseReleases', () => {
  it('puts the newest first, comparing numbers rather than text', () => {
    const files = { 'v0.9.0.md': '# V0.9.0', 'v0.10.0.md': '# V0.10.0', 'v0.2.1.md': '# V0.2.1', 'v1.0.0.md': '# V1.0.0' };
    expect(parseReleases(files).map((r) => r.version)).toEqual(['1.0.0', '0.10.0', '0.9.0', '0.2.1']);
  });
});

describe('RELEASES', () => {
  it('has notes for every release, each with a date, and the site version on top', () => {
    expect(RELEASES.at(0)?.version).toBe(__APP_VERSION__.replace('-dev', ''));
    expect(RELEASES.map((r) => r.version)).toEqual(expect.arrayContaining(['0.5.0', '0.6.0', '0.7.0', '0.8.0']));
    expect(RELEASES.every((r) => r.date !== null && r.body !== '')).toBe(true);
  });
});
