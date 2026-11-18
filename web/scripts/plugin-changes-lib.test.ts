import { describe, expect, it } from 'vitest';
import { changedSummary, findChanged, markChanged, scopeOf } from './plugin-changes-lib';
import type { PluginLog } from './plugin-log-lib';
import type { Remote } from './plugin-updates-lib';

const commit = (date: string, message: string) => ({ html_url: `https://github.com/o/r/commit/${date}`, commit: { message, committer: { date: `${date}T10:00:00Z` } } });

/** A fake GitHub that answers commit queries by path; it records what was asked. */
function remote(answers: Record<string, unknown[]>): Remote & { asked: string[] } {
  const asked: string[] = [];
  return {
    asked,
    api: async (path) => {
      asked.push(path);
      return answers[path] ?? [];
    },
    download: async () => null,
  };
}

const log: PluginLog = {
  format: 1,
  plugins: [
    { id: 'trio', added: '2026-11-14', source: 'https://github.com/o/r', draft: false },
    { id: 'fluke', added: '2026-11-14', source: 'https://github.com/o/r', draft: false, polished: '2026-11-18' },
    { id: 'hail', added: '2026-10-29', source: 'https://github.com/Hundekraft/op1-workshop', draft: false },
    { id: 'mine', added: '2026-10-29', source: 'https://github.com/coldsoldr/x', draft: false },
    { id: 'waiting', added: '2026-11-14', source: 'https://github.com/o/r', draft: true },
  ],
};
const manifests = new Map<string, Record<string, unknown>>([
  ['trio', { link: 'https://github.com/o/r/tree/main/Delay-Trio', source: 'https://github.com/o/r' }],
  ['fluke', { link: 'https://github.com/o/r/tree/main/Fluke' }],
  ['hail', { link: 'https://hundekraft.github.io/op1-workshop/', source: 'https://github.com/pcppcp/op1r' }],
  ['mine', { link: 'https://github.com/coldsoldr/x/releases/latest' }],
  ['waiting', { link: 'https://github.com/o/r' }],
]);
const trioQuery = `repos/o/r/commits?since=2026-11-15T00:00:00Z&per_page=20&path=${encodeURIComponent('Delay-Trio')}`;
const flukeQuery = `repos/o/r/commits?since=2026-11-19T00:00:00Z&per_page=20&path=Fluke`;

describe('listings whose plugin changed in its repo', () => {
  it('reads the folder a listing links, else the whole repo', () => {
    expect(scopeOf('https://github.com/o/r/tree/main/Delay%20Trio/', undefined)).toEqual({ repo: 'o/r', path: 'Delay Trio' });
    expect(scopeOf('https://o.github.io/r/', 'https://github.com/o/r')).toEqual({ repo: 'o/r', path: '' });
    expect(scopeOf('https://example.com', undefined)).toBeNull();
  });

  it('asks for commits to its folder from the day after it was written, and skips the refresher’s, the owner’s and waiting ones', async () => {
    const r = remote({ [trioQuery]: [commit('2026-11-18', 'Update eight devices'), commit('2026-11-16', 'Fix @someone #3')] });
    const changed = await findChanged(log, manifests, r);
    expect(r.asked.sort()).toEqual([flukeQuery, trioQuery].sort());
    expect(changed).toHaveLength(1);
    expect(changed[0]).toMatchObject({ id: 'trio', repo: 'o/r', path: 'Delay-Trio', since: '2026-11-14' });

    const marked = markChanged(log, changed);
    expect(marked.plugins.find((e) => e.id === 'trio')).toMatchObject({ draft: true, changed: '2026-11-18' });
    expect(marked.plugins.find((e) => e.id === 'fluke')).toMatchObject({ draft: false });

    const text = changedSummary(changed);
    expect(text).toContain('### o/r');
    expect(text).toContain('[Update eight devices](https://github.com/o/r/commit/2026-11-18)');
    expect(text).not.toMatch(/(^|[^\u200B])@someone/);
    expect(text).not.toContain('#3');
  });

  it('says so when nothing changed', async () => {
    expect(changedSummary(await findChanged(log, manifests, remote({})))).toMatch(/^No listed plugin changed/);
  });
});
