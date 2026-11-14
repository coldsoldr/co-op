/**
 * Keeps plugins/log.json in step with plugins/ (plugin-log-lib.ts). Run from web/:
 *
 *   tsx scripts/plugin-log.ts           add every folder that has no entry, dated by the commit that added
 *                                       its plugin.json (today if git has not seen it), and drop entries
 *                                       whose folder is gone
 *   tsx scripts/plugin-log.ts drafts    list the entries whose description and manual are still drafts
 */
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { subfolders } from './packs-lib';
import { compareLog, readLog, upsert, writeLog, type LogEntry } from './plugin-log-lib';

const pluginsDir = resolve(import.meta.dirname, '..', '..', 'plugins');
const today = new Date().toISOString().slice(0, 10);

function addedOn(id: string): string {
  try {
    const out = execFileSync('git', ['log', '--diff-filter=A', '--format=%as', '--', `plugins/${id}/plugin.json`], {
      cwd: resolve(pluginsDir, '..'),
      encoding: 'utf8',
    });
    return out.trim().split('\n').filter(Boolean).at(-1) ?? today;
  } catch {
    return today;
  }
}

async function sourceOf(id: string): Promise<string | undefined> {
  try {
    const m = JSON.parse(await readFile(join(pluginsDir, id, 'plugin.json'), 'utf8')) as { source?: string; link?: string };
    const url = [m.source, m.link].find((u) => typeof u === 'string' && u.startsWith('https://github.com/'));
    const repo = url === undefined ? null : /^https:\/\/github\.com\/[^/]+\/[^/?#]+/.exec(url);
    return repo?.[0];
  } catch {
    return undefined;
  }
}

const [command] = process.argv.slice(2);
let log = await readLog(pluginsDir);
if (command === 'drafts') {
  const drafts = log.plugins.filter((e) => e.draft);
  for (const e of drafts) console.log(`${e.id}\tadded ${e.added}\t${e.issue === undefined ? '' : `issue #${e.issue}\t`}${e.source ?? ''}`);
  console.log(`${drafts.length} draft${drafts.length === 1 ? '' : 's'}`);
} else if (command === undefined) {
  const { folders } = await subfolders(pluginsDir);
  const { missing, gone } = compareLog(log, folders);
  for (const id of missing) {
    const source = await sourceOf(id);
    const entry: LogEntry = { id, added: addedOn(id), draft: false };
    if (source !== undefined) entry.source = source;
    log = upsert(log, entry);
  }
  log = { ...log, plugins: log.plugins.filter((e) => !gone.includes(e.id)) };
  await writeLog(pluginsDir, log);
  console.log(`added ${missing.length}, removed ${gone.length}; ${log.plugins.length} plugins in ${join('plugins', 'log.json')}`);
} else {
  console.error('usage: tsx scripts/plugin-log.ts [drafts]');
  process.exitCode = 2;
}
