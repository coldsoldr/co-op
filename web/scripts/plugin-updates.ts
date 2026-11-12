/**
 * The daily check for plugin updates (.github/workflows/plugin-updates.yml): reads every plugins/<id>/plugin.json,
 * applies the updates its author's repo has (plugin-updates-lib.ts), and writes a summary for the pull request.
 *
 *   pnpm --filter @co-op/web exec tsx scripts/plugin-updates.ts [--summary FILE] [plugins dir]
 *
 * GH_TOKEN (or GITHUB_TOKEN) authenticates the GitHub API; without one it runs at GitHub's anonymous rate.
 * Exits 0 whether or not it found anything; the workflow looks at git diff.
 */
import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { summary, updatePlugin, type Change, type Problem, type Remote } from './plugin-updates-lib';

const args = process.argv.slice(2);
const summaryAt = args.indexOf('--summary');
const summaryFile = summaryAt >= 0 ? args.splice(summaryAt, 2)[1] : undefined;
const pluginsDir = resolve(args[0] ?? join(import.meta.dirname, '..', '..', 'plugins'));
const token = process.env.GH_TOKEN ?? process.env.GITHUB_TOKEN;

const remote: Remote = {
  async api(path) {
    const response = await fetch(`https://api.github.com/${path}`, {
      headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'co-op-plugin-updates', ...(token ? { Authorization: `Bearer ${token}` } : {}) },
    });
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`GitHub ${path}: HTTP ${response.status}`);
    return response.json();
  },
  async download(url) {
    const response = await fetch(url);
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    return new Uint8Array(await response.arrayBuffer());
  },
};

const changes: Change[] = [];
const problems: Problem[] = [];
for (const entry of (await readdir(pluginsDir, { withFileTypes: true })).filter((e) => e.isDirectory()).sort((a, b) => a.name.localeCompare(b.name))) {
  const file = join(pluginsDir, entry.name, 'plugin.json');
  let manifest: Record<string, unknown>;
  try {
    manifest = JSON.parse(await readFile(file, 'utf8')) as Record<string, unknown>;
  } catch {
    continue;
  }
  try {
    const found = await updatePlugin(manifest, remote);
    if (found.changes.length > 0) await writeFile(file, `${JSON.stringify(manifest, null, 2)}\n`);
    changes.push(...found.changes);
    problems.push(...found.problems);
  } catch (e) {
    problems.push({ plugin: entry.name, firmware: '', message: `the check failed: ${e instanceof Error ? e.message : String(e)}` });
  }
}

const text = summary(changes, problems);
if (summaryFile) await writeFile(summaryFile, text);
console.log(text);
console.log(`${changes.length} update(s), ${problems.length} problem(s).`);
