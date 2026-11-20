/**
 * Drafts listings from a plugin-request issue's repo (.github/workflows/plugin-request.yml), with
 * plugin-request-lib.ts, and writes them into plugins/ with their log entries. Run from web/:
 *
 *   tsx scripts/plugin-request.ts --issue 12 [--summary FILE] [--ids FILE]
 *   tsx scripts/plugin-request.ts --repo owner/repo [--firmware hundekraft|opus|op1r] [--summary FILE] [--ids FILE]
 *   tsx scripts/plugin-request.ts --watch [--dry] [--summary FILE] [--ids FILE]
 *
 * --watch looks in every repo plugins/log.json names for built plugin files not listed yet (the daily
 * plugin-updates workflow's new-plugins job); --dry only reports them.
 *
 * --summary writes the markdown for the pull request and the issue comment; --ids writes the drafted ids,
 * one per line. GH_TOKEN (or GITHUB_TOKEN) authenticates the GitHub API. Screenshots are made with
 * .claude/skills/add-plugin/screenshot.py (Python with Pillow; PYTHON picks the interpreter). Exits 0
 * whether or not it drafted anything.
 */
import { execFileSync } from 'node:child_process';
import { mkdir, mkdtemp, readdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { neutraliseMentions } from '@co-op/shared/feedback';
import { REQUEST_FIRMWARES, parseRepo, repoFromIssue, type RequestFirmware } from '@co-op/shared/plugin-request';
import { readLog, upsert, writeLog } from './plugin-log-lib';
import { draftListings, type DraftResult } from './plugin-request-lib';
import type { Remote } from './plugin-updates-lib';

const root = resolve(import.meta.dirname, '..', '..');
const pluginsDir = resolve(process.argv.includes('--plugins') ? (process.argv[process.argv.indexOf('--plugins') + 1] ?? '') : join(root, 'plugins'));
const helper = join(root, '.claude', 'skills', 'add-plugin', 'screenshot.py');
const python = process.env.PYTHON ?? (process.platform === 'win32' ? 'python' : 'python3');
const token = process.env.GH_TOKEN ?? process.env.GITHUB_TOKEN;
const today = new Date().toISOString().slice(0, 10);

const args = process.argv.slice(2);
const opt = (name: string): string | undefined => {
  const at = args.indexOf(name);
  return at >= 0 ? args[at + 1] : undefined;
};

const remote: Remote = {
  async api(path) {
    const response = await fetch(`https://api.github.com/${path}`, {
      headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'co-op-plugin-request', ...(token ? { Authorization: `Bearer ${token}` } : {}) },
    });
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`GitHub ${path}: HTTP ${response.status}`);
    return response.json();
  },
  async download(url) {
    const response = await fetch(url, { headers: { 'User-Agent': 'co-op-plugin-request' } });
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    const length = Number(response.headers.get('Content-Length'));
    // Nothing a listing needs is bigger than this; a larger image becomes a name card.
    if (Number.isFinite(length) && length > 16 * 1024 * 1024) return null;
    return new Uint8Array(await response.arrayBuffer());
  },
};

/** The repo and firmware hint, from the issue or the arguments. */
async function target(): Promise<{ repo: string; firmware: RequestFirmware | null; issue?: number } | string> {
  const issueArg = opt('--issue');
  if (issueArg !== undefined) {
    const issue = Number(issueArg);
    if (!Number.isInteger(issue) || issue <= 0) return `--issue must be an issue number, not ${issueArg}`;
    const data = (await remote.api(`repos/coldsoldr/co-op/issues/${issue}`)) as { body?: string | null } | null;
    const found = data === null ? null : repoFromIssue(data.body ?? '');
    if (found === null) return 'The issue has no link to a GitHub repo, so there is nothing to draft. A maintainer will look at it.';
    return { ...found, issue };
  }
  const repo = parseRepo(opt('--repo') ?? '');
  if (repo === null) return 'usage: --issue N | --repo owner/repo [--firmware hundekraft|opus|op1r]';
  const fw = opt('--firmware');
  return { repo, firmware: fw !== undefined && (REQUEST_FIRMWARES as readonly string[]).includes(fw) ? (fw as RequestFirmware) : null };
}

async function existing(): Promise<Map<string, Record<string, unknown>>> {
  const out = new Map<string, Record<string, unknown>>();
  for (const e of await readdir(pluginsDir, { withFileTypes: true })) {
    if (!e.isDirectory()) continue;
    try {
      out.set(e.name, JSON.parse(await readFile(join(pluginsDir, e.name, 'plugin.json'), 'utf8')) as Record<string, unknown>);
    } catch {
      out.set(e.name, {});
    }
  }
  return out;
}

/** The screenshot from the README's image, or a name card when there is none or it won't convert. */
async function screenshot(url: string | null, name: string, out: string, scratch: string): Promise<'image' | 'card'> {
  if (url !== null) {
    try {
      const bytes = await remote.download(url);
      if (bytes !== null) {
        const dir = await mkdtemp(join(scratch, 'img-'));
        const file = join(dir, 'image');
        await writeFile(file, bytes);
        execFileSync(python, ['-I', helper, file, out], { stdio: 'pipe' });
        return 'image';
      }
    } catch {
      // Falls through to the card.
    }
  }
  execFileSync(python, ['-I', helper, '--card', name, out], { stdio: 'pipe' });
  return 'card';
}

/** The pull request's body and the issue's comment. README text is neutralised so it can't ping anyone. */
function summary(result: DraftResult, shots: Map<string, 'image' | 'card'>, issue: number | undefined): string {
  // Names and authors come from the repo: no mentions, no markup. Links and notes are co-op's own.
  const safe = (s: unknown): string => neutraliseMentions(String(s)).replace(/[<>`|[\]]/g, ' ');
  const lines = [`Drafted from https://github.com/${result.repo}${issue === undefined ? '' : ` for #${issue}`}.`, ''];
  for (const d of result.drafts) {
    const m = d.manifest;
    lines.push(`### ${safe(m.name)} (\`${d.id}\`)`, '');
    const builds = (Array.isArray(m.builds) ? m.builds : [m]) as Record<string, unknown>[];
    lines.push(`- ${safe(m.kind)} by ${safe(m.author)}${m.license ? `, ${safe(m.license)}` : ''}`);
    for (const b of builds) {
      lines.push(`- ${String(b.firmware).slice(4)}${b.version ? ` ${safe(b.version)}` : ''}${b.requires ? ` (needs ${safe(b.requires)})` : ''}: ${String(b.link)}`);
      if (b.fileUrl) lines.push(`  - installs over USB from ${String(b.fileUrl)}`);
    }
    if (shots.size > 0) lines.push(`- screenshot: ${shots.get(d.id) === 'image' ? 'from the repo' : 'a name card'}`);
    for (const n of d.notes) lines.push(`- ${neutraliseMentions(n)}`);
    lines.push('');
  }
  if (result.skipped.length > 0) lines.push(`Already listed from this repo, left alone: ${result.skipped.map((s) => `\`${s}\``).join(', ')}.`, '');
  for (const p of result.problems) lines.push(neutraliseMentions(p), '');
  if (result.drafts.length > 0) {
    lines.push('The description and manual are drafts read from the README. plugins/log.json marks them, for the owner to rewrite with /polish-plugins once they are merged.');
  }
  return `${lines.join('\n').trim()}\n`;
}

/** Writes the drafts into plugins/ with their log entries; returns how each screenshot was made. */
async function writeDrafts(result: DraftResult, issue: number | undefined): Promise<Map<string, 'image' | 'card'>> {
  const scratch = await mkdtemp(join(tmpdir(), 'co-op-request-'));
  const shots = new Map<string, 'image' | 'card'>();
  let log = await readLog(pluginsDir);
  try {
    for (const d of result.drafts) {
      const dir = join(pluginsDir, d.id);
      await mkdir(dir, { recursive: true });
      await writeFile(join(dir, 'plugin.json'), `${JSON.stringify(d.manifest, null, 2)}\n`);
      await writeFile(join(dir, 'manual.md'), d.manual);
      shots.set(d.id, await screenshot(d.screenshot, String(d.manifest.name), join(dir, 'screenshot.png'), scratch));
      log = upsert(log, {
        id: d.id,
        added: today,
        source: `https://github.com/${result.repo}`,
        ...(issue === undefined ? {} : { issue }),
        draft: true,
      });
    }
  } finally {
    await rm(scratch, { recursive: true, force: true });
  }
  if (result.drafts.length > 0) await writeLog(pluginsDir, log);
  return shots;
}

/** Repos the workshop, opus and op1r refresher (listed_plugins.py) keeps; the watch leaves them to it. */
const REFRESHED_REPOS = ['hundekraft/op1-workshop', 'lizardman245/opus', 'pcppcp/op1r', 'pcppcp/op1r-src'];

/**
 * --watch: every repo plugins/log.json names, looked at for plugin files that aren't listed yet. A repo or file
 * in the log's ignore list is skipped, so a draft the owner turned down doesn't come back.
 */
async function watch(dry: boolean): Promise<{ text: string; ids: string[] }> {
  const log = await readLog(pluginsDir);
  const ignore = new Set((log.ignore ?? []).map((i) => i.toLowerCase()));
  const repos = [
    ...new Set(log.plugins.map((e) => (e.source === undefined ? null : parseRepo(e.source))).filter((r): r is string => r !== null)),
  ].filter((r) => !REFRESHED_REPOS.includes(r.toLowerCase()) && !ignore.has(r.toLowerCase()));
  const sections: string[] = [];
  const ids: string[] = [];
  const known = await existing();
  for (const repo of repos.sort((a, b) => a.localeCompare(b))) {
    let result: DraftResult;
    try {
      result = await draftListings(repo, null, remote, known, { onlyFiles: true, ignore: (path) => ignore.has(`${repo}/${path}`.toLowerCase()) });
    } catch (e) {
      sections.push(`${repo}: the check failed: ${e instanceof Error ? e.message : String(e)}`);
      continue;
    }
    console.log(`${repo}: ${result.drafts.length} new, ${result.skipped.length} listed`);
    if (result.drafts.length === 0) continue;
    const shots = dry ? new Map<string, 'image' | 'card'>() : await writeDrafts(result, undefined);
    for (const d of result.drafts) {
      ids.push(d.id);
      known.set(d.id, d.manifest);
    }
    const ignoreHow = result.drafts.map((d) => `\`${result.repo}/${d.path}\``).join(', ');
    sections.push(
      `${summary({ ...result, skipped: [], problems: [] }, shots, undefined).trim()}\n\nTo turn one down for good, close this or drop its folder, and add its path to \`ignore\` in plugins/log.json: ${ignoreHow}.`,
    );
  }
  const text =
    sections.length === 0
      ? 'No new plugins in the listed authors’ repos.\n'
      : `New plugins in repos co-op already lists, drafted the way a submission is.\n\n${sections.join('\n\n---\n\n')}\n`;
  return { text, ids };
}

const summaryFile = opt('--summary');
const idsFile = opt('--ids');
if (args.includes('--watch')) {
  const { text, ids } = await watch(args.includes('--dry'));
  console.log(text);
  if (summaryFile) await writeFile(summaryFile, text);
  if (idsFile) await writeFile(idsFile, ids.join('\n'));
} else {
  const found = await target();
  if (typeof found === 'string') {
    console.log(found);
    if (summaryFile) await writeFile(summaryFile, `${found}\n`);
    if (idsFile) await writeFile(idsFile, '');
  } else {
    const result = await draftListings(found.repo, found.firmware, remote, await existing());
    const shots = await writeDrafts(result, found.issue);
    const text = summary(result, shots, found.issue);
    console.log(text);
    if (summaryFile) await writeFile(summaryFile, text);
    if (idsFile) await writeFile(idsFile, result.drafts.map((d) => d.id).join('\n'));
  }
}
