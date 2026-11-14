/**
 * plugins/log.json: every plugin in plugins/, when it was added, and whether its description and manual
 * are still the plugin-request workflow's draft. The owner's /polish-plugins skill rewrites the drafts
 * and stamps `polished`; `pnpm --filter @co-op/web plugins:log` adds any plugin that is missing, with the
 * date git first saw it.
 *
 * {
 *   "format": 1,
 *   "plugins": [
 *     { "id": "bubbler", "added": "2026-11-14", "source": "https://github.com/VolkerJunginger/OP-1", "draft": false }
 *     { "id": "x", "added": "2026-11-20", "source": "...", "issue": 12, "draft": true }
 *     { "id": "x", "added": "2026-11-20", "source": "...", "issue": 12, "draft": false, "polished": "2026-11-22" }
 *   ]
 * }
 */
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

export const LOG_FILE = 'log.json';
export const LOG_FORMAT = 1;

export interface LogEntry {
  id: string;
  /** YYYY-MM-DD: when the listing was added (drafted, for a submission). */
  added: string;
  /** The plugin's repo, when it has one. */
  source?: string;
  /** The plugin-request issue it came from. */
  issue?: number;
  /** True while the description and manual are the workflow's draft. */
  draft: boolean;
  /** YYYY-MM-DD: when the draft was last rewritten by hand. */
  polished?: string;
}

export interface PluginLog {
  format: number;
  plugins: LogEntry[];
}

const DATE = /^\d{4}-\d{2}-\d{2}$/;

/** The log's entries, checked; a missing file is an empty log. Throws on a malformed one. */
export async function readLog(pluginsDir: string): Promise<PluginLog> {
  let text: string;
  try {
    text = await readFile(join(pluginsDir, LOG_FILE), 'utf8');
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return { format: LOG_FORMAT, plugins: [] };
    throw error;
  }
  const log = JSON.parse(text.replace(/^\uFEFF/, '')) as PluginLog;
  if (log.format !== LOG_FORMAT || !Array.isArray(log.plugins)) throw new Error(`${LOG_FILE}: not a format ${LOG_FORMAT} log`);
  for (const e of log.plugins) {
    if (typeof e.id !== 'string' || typeof e.added !== 'string' || !DATE.test(e.added) || typeof e.draft !== 'boolean') {
      throw new Error(`${LOG_FILE}: a bad entry: ${JSON.stringify(e)}`);
    }
  }
  return log;
}

/** Sorted by date added, then id, so new plugins land at the end and the diff stays small. */
export function sortLog(log: PluginLog): PluginLog {
  const plugins = [...log.plugins].sort((a, b) => a.added.localeCompare(b.added) || a.id.localeCompare(b.id));
  return { format: LOG_FORMAT, plugins };
}

export async function writeLog(pluginsDir: string, log: PluginLog): Promise<void> {
  await writeFile(join(pluginsDir, LOG_FILE), `${JSON.stringify(sortLog(log), null, 2)}\n`);
}

/** Adds or replaces the entry with this id. */
export function upsert(log: PluginLog, entry: LogEntry): PluginLog {
  return { format: LOG_FORMAT, plugins: [...log.plugins.filter((e) => e.id !== entry.id), entry] };
}

/** Folders with no entry, and entries with no folder. */
export function compareLog(log: PluginLog, folders: readonly string[]): { missing: string[]; gone: string[] } {
  const logged = new Set(log.plugins.map((e) => e.id));
  const present = new Set(folders);
  return { missing: folders.filter((f) => !logged.has(f)), gone: [...logged].filter((id) => !present.has(id)) };
}
