/**
 * Finds listed plugins whose author changed them since co-op last wrote their listing: commits in the
 * plugin's repo (its own folder, when the listing links one) after the log entry's `polished` date, else
 * its `added` date. Each one found is marked in plugins/log.json (`draft: true`, `changed: <date>`) for the
 * owner's /polish-plugins skill, and listed with its commits for the pull request.
 *
 * Left out: listings the workshop and opus refresher (listed_plugins.py) keeps, the owner's own repos, and
 * entries already waiting for a rewrite. Commit messages are someone else's text, shown as data only.
 */
import { neutraliseMentions } from '@co-op/shared/feedback';
import type { LogEntry, PluginLog } from './plugin-log-lib';
import type { Remote } from './plugin-updates-lib';

/** Kept by listed_plugins.py. */
export const REFRESHED_PREFIXES = ['https://hundekraft.github.io/op1-workshop/', 'https://github.com/LizardMan245/opus/releases'];
/** The owner's own repos: their changes come with their own commits to co-op. */
const OWN = /^coldsoldr\//i;

export interface Commit {
  date: string;
  message: string;
  url: string;
}

export interface Changed {
  id: string;
  repo: string;
  /** The repo folder the commits were looked for in, '' for the whole repo. */
  path: string;
  since: string;
  commits: Commit[];
}

/** owner/repo and the folder a listing's link points at: /tree/<branch>/<folder>, else the whole repo. */
export function scopeOf(link: string | undefined, source: string | undefined): { repo: string; path: string } | null {
  const tree = link === undefined ? null : /^https:\/\/github\.com\/([^/]+\/[^/]+)\/tree\/[^/]+\/(.+?)\/?$/.exec(link);
  if (tree?.[1] && tree[2]) return { repo: tree[1], path: decodeURIComponent(tree[2]) };
  for (const url of [source, link]) {
    const repo = url === undefined ? null : /^https:\/\/github\.com\/([^/]+\/[^/?#]+)/.exec(url);
    if (repo?.[1]) return { repo: repo[1].replace(/\.git$/, ''), path: '' };
  }
  return null;
}

/** The day after a date, as the start of the window: changes on the day a listing was written are in it. */
const dayAfter = (date: string): string => new Date(Date.parse(`${date}T00:00:00Z`) + 86_400_000).toISOString().replace(/\.\d+Z$/, 'Z');

/**
 * The listings with commits since they were last written. `manifests` maps ids to their plugin.json, for the
 * link that says which folder the plugin lives in.
 */
export async function findChanged(log: PluginLog, manifests: ReadonlyMap<string, Record<string, unknown>>, remote: Remote): Promise<Changed[]> {
  const out: Changed[] = [];
  const asked = new Map<string, Commit[]>();
  for (const entry of log.plugins) {
    if (entry.draft) continue;
    const m = manifests.get(entry.id);
    if (m === undefined) continue;
    const builds = (Array.isArray(m.builds) ? m.builds : [m]) as Record<string, unknown>[];
    const links = builds.map((b) => (typeof b.link === 'string' ? b.link : typeof m.link === 'string' ? m.link : undefined));
    if (links.every((l) => l === undefined || REFRESHED_PREFIXES.some((r) => l.startsWith(r)))) continue;
    const link = links.find((l) => l !== undefined && !REFRESHED_PREFIXES.some((r) => l.startsWith(r)));
    const scope = scopeOf(link, typeof m.source === 'string' ? m.source : entry.source);
    if (scope === null || OWN.test(scope.repo)) continue;
    const since = entry.polished ?? entry.added;
    const key = `${scope.repo}|${scope.path}|${since}`;
    if (!asked.has(key)) {
      const query = `repos/${scope.repo}/commits?since=${dayAfter(since)}&per_page=20${scope.path ? `&path=${encodeURIComponent(scope.path)}` : ''}`;
      const commits = ((await remote.api(query)) ?? []) as { html_url?: string; commit?: { message?: string; committer?: { date?: string } } }[];
      asked.set(
        key,
        commits.map((c) => ({
          date: (c.commit?.committer?.date ?? '').slice(0, 10),
          message: (c.commit?.message ?? '').split('\n')[0] ?? '',
          url: c.html_url ?? '',
        })),
      );
    }
    const commits = asked.get(key) ?? [];
    if (commits.length > 0) out.push({ id: entry.id, repo: scope.repo, path: scope.path, since, commits });
  }
  return out;
}

/** The log with each changed listing marked for a rewrite. */
export function markChanged(log: PluginLog, changed: readonly Changed[]): PluginLog {
  const byId = new Map(changed.map((c) => [c.id, c]));
  const plugins = log.plugins.map((e): LogEntry => {
    const c = byId.get(e.id);
    if (c === undefined) return e;
    const newest = c.commits.map((x) => x.date).sort().at(-1) ?? c.since;
    return { ...e, draft: true, changed: newest };
  });
  return { ...log, plugins };
}

/** The pull request's body: by repo, the listings and the commits that changed them. */
export function changedSummary(changed: readonly Changed[]): string {
  if (changed.length === 0) return 'No listed plugin changed in its repo since its listing was written.\n';
  const safe = (s: string): string => neutraliseMentions(s).replace(/[<>`|[\]]/g, ' ');
  const lines = [
    'These plugins changed in their authors’ repos since co-op last wrote their listings. plugins/log.json now marks them for a rewrite: merge this, then run /polish-plugins to bring their descriptions and manuals up to date.',
    '',
  ];
  const repos = new Map<string, Changed[]>();
  for (const c of changed) repos.set(c.repo, [...(repos.get(c.repo) ?? []), c]);
  for (const [repo, list] of repos) {
    lines.push(`### ${repo}`, '');
    for (const c of list) {
      lines.push(`- \`${c.id}\`${c.path ? ` (${safe(c.path)}/)` : ''}, written ${c.since}:`);
      for (const commit of c.commits.slice(0, 8)) lines.push(`  - ${commit.date} [${safe(commit.message).slice(0, 120)}](${commit.url})`);
      if (c.commits.length > 8) lines.push(`  - and ${c.commits.length - 8} more`);
    }
    lines.push('');
  }
  return `${lines.join('\n').trim()}\n`;
}
