/**
 * The daily check for listed plugins that changed in their repos (plugin-changes-lib.ts), run from web/:
 *
 *   tsx scripts/plugin-changes.ts [--dry] [--summary FILE]
 *
 * Marks them in plugins/log.json for /polish-plugins (unless --dry) and writes the pull request's body.
 * GH_TOKEN (or GITHUB_TOKEN) authenticates the GitHub API.
 */
import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { changedSummary, findChanged, markChanged } from './plugin-changes-lib';
import { readLog, writeLog } from './plugin-log-lib';
import type { Remote } from './plugin-updates-lib';

const pluginsDir = resolve(import.meta.dirname, '..', '..', 'plugins');
const args = process.argv.slice(2);
const summaryAt = args.indexOf('--summary');
const summaryFile = summaryAt >= 0 ? args[summaryAt + 1] : undefined;
const token = process.env.GH_TOKEN ?? process.env.GITHUB_TOKEN;

const remote: Remote = {
  async api(path) {
    const response = await fetch(`https://api.github.com/${path}`, {
      headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'co-op-plugin-changes', ...(token ? { Authorization: `Bearer ${token}` } : {}) },
    });
    if (response.status === 404 || response.status === 409) return null;
    if (!response.ok) throw new Error(`GitHub ${path}: HTTP ${response.status}`);
    return response.json();
  },
  async download() {
    return null;
  },
};

const manifests = new Map<string, Record<string, unknown>>();
for (const e of await readdir(pluginsDir, { withFileTypes: true })) {
  if (!e.isDirectory()) continue;
  try {
    manifests.set(e.name, JSON.parse(await readFile(join(pluginsDir, e.name, 'plugin.json'), 'utf8')) as Record<string, unknown>);
  } catch {
    // A broken folder is the validator's to report.
  }
}

const log = await readLog(pluginsDir);
const changed = await findChanged(log, manifests, remote);
if (!args.includes('--dry') && changed.length > 0) await writeLog(pluginsDir, markChanged(log, changed));
const text = changedSummary(changed);
console.log(text);
if (summaryFile) await writeFile(summaryFile, text);
