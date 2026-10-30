import {
  HUNDEKRAFT_ID,
  MAX_TEXT_BYTES,
  OPUS_ID,
  PLUGIN_FIRMWARE,
  PLUGIN_INDEX_FORMAT,
  PLUGIN_KINDS,
  type PluginFirmware,
  type PluginIndex,
  type PluginIndexEntry,
  type PluginKind,
} from '@co-op/shared/plugins';
import { CatalogueError, fetchOk, withSlash } from '../catalogue/catalogue';
import { sha256Hex } from '../util';

const reason = (error: unknown): string => (error instanceof Error ? error.message : String(error));
const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const isStringList = (value: unknown): boolean => Array.isArray(value) && value.every((v) => typeof v === 'string');
const oneOf = (list: readonly string[], value: unknown): boolean => typeof value === 'string' && list.includes(value);
const SHA256 = /^[0-9a-f]{64}$/i;
/** A file name inside a plugin folder: no separators, never `.` or `..`. */
const FILE_NAME = /^[A-Za-z0-9_][A-Za-z0-9_.-]*$/;
const HTTPS = /^https:\/\/\S+$/;

/** True when `path` is `<id>/<name>` for a plain file name: the plugin's own folder and nothing else. */
const inFolder = (id: string, path: unknown): boolean =>
  typeof path === 'string' && path.startsWith(`${id}/`) && FILE_NAME.test(path.slice(id.length + 1));

/**
 * A shape check, so the UI never trips over a field of the wrong type, plus the checks the page
 * relies on for safety: every path stays in the plugin's folder and the source link is https.
 */
function isIndexEntry(value: unknown): value is PluginIndexEntry {
  if (!isRecord(value)) return false;
  const strings = ['id', 'name', 'author', 'version', 'description', 'license', 'file'].every((k) => typeof value[k] === 'string');
  if (!strings || !oneOf(PLUGIN_FIRMWARE, value.firmware) || !oneOf(PLUGIN_KINDS, value.kind)) return false;
  const id = value.id as string;
  if (!(value.firmware === 'op1/hundekraft' ? HUNDEKRAFT_ID : OPUS_ID).test(id)) return false;
  if (!FILE_NAME.test(value.file as string) || !inFolder(id, value.screenshot) || !inFolder(id, value.manual)) return false;
  if (typeof value.sizeBytes !== 'number' || typeof value.sha256 !== 'string' || !SHA256.test(value.sha256)) return false;
  if (!isStringList(value.tags)) return false;
  const optional = ['requires', 'credits', 'video', 'savedName', 'headerAuthor', 'headerVersion'];
  if (!optional.every((k) => value[k] === undefined || typeof value[k] === 'string')) return false;
  return value.source === undefined || (typeof value.source === 'string' && HTTPS.test(value.source));
}

/**
 * Loads `<baseUrl>plugins/index.json` (baseUrl is the app's base, as for the pack index) and checks
 * that it is an index this version understands. It changes with every deploy, so it is always
 * revalidated with the server.
 */
export async function fetchPluginIndex(baseUrl: string, fetchImpl: typeof fetch = fetch): Promise<PluginIndex> {
  const response = await fetchOk(`${withSlash(baseUrl)}plugins/index.json`, 'load the plugin index', fetchImpl, {
    cache: 'no-cache',
  });
  let json: unknown;
  try {
    json = JSON.parse(await response.text());
  } catch (error) {
    throw new CatalogueError(`the plugin index is not valid JSON: ${reason(error)}`, { cause: error });
  }
  if (!isRecord(json) || json.format !== PLUGIN_INDEX_FORMAT || !Array.isArray(json.plugins)) {
    throw new CatalogueError(`unsupported plugin index (expected format ${PLUGIN_INDEX_FORMAT})`);
  }
  json.plugins.forEach((entry: unknown, i: number) => {
    if (!isIndexEntry(entry)) throw new CatalogueError(`plugin index entry ${i + 1} is malformed`);
  });
  return json as unknown as PluginIndex;
}

export interface PluginFilter {
  /** Every whitespace-separated word must appear (ignoring case) in the name, id, author, description, a tag or the credits. */
  text?: string;
  kind?: PluginKind;
  firmware?: PluginFirmware;
  tag?: string;
}

export interface AuthorGroup {
  author: string;
  plugins: PluginIndexEntry[];
}

/** Plugins by author, authors in alphabetical order ignoring case; each keeps the order it was given. */
export function groupByAuthor(plugins: readonly PluginIndexEntry[]): AuthorGroup[] {
  const groups = new Map<string, PluginIndexEntry[]>();
  for (const plugin of plugins) groups.set(plugin.author, [...(groups.get(plugin.author) ?? []), plugin]);
  return [...groups]
    .map(([author, list]) => ({ author, plugins: list }))
    .sort((a, b) => a.author.localeCompare(b.author, 'en', { sensitivity: 'base' }));
}

/** The plugins that match every given criterion, in index order. */
export function filterPlugins(index: PluginIndex, filter: PluginFilter): PluginIndexEntry[] {
  const words = (filter.text ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  return index.plugins.filter((plugin) => {
    if (filter.kind !== undefined && plugin.kind !== filter.kind) return false;
    if (filter.firmware !== undefined && plugin.firmware !== filter.firmware) return false;
    if (filter.tag !== undefined && !plugin.tags.includes(filter.tag)) return false;
    if (words.length === 0) return true;
    const haystack = [plugin.name, plugin.id, plugin.author, plugin.description, ...plugin.tags, plugin.credits ?? '']
      .join('\n')
      .toLowerCase();
    return words.every((word) => haystack.includes(word));
  });
}

/** The URL of a path relative to the plugins folder, such as an index entry's `screenshot`. */
export function pluginFileUrl(baseUrl: string, path: string): string {
  return `${withSlash(baseUrl)}plugins/${path.split('/').map(encodeURIComponent).join('/')}`;
}

/** The URL of a plugin's folder, with a trailing slash: the base its manual's relative links resolve against. */
export function pluginFolderUrl(baseUrl: string, id: string): string {
  return `${pluginFileUrl(baseUrl, id)}/`;
}

/**
 * Downloads a plugin file and checks it against the size and SHA-256 from the index, so a stale
 * cache or a broken download is never saved or installed. A mismatch is retried once bypassing
 * the HTTP cache, which covers a file cached from before a deploy.
 */
export async function fetchPluginBytes(
  baseUrl: string,
  plugin: PluginIndexEntry,
  fetchImpl: typeof fetch = fetch,
): Promise<Uint8Array> {
  const label = `${plugin.id}/${plugin.file}`;
  if (!FILE_NAME.test(plugin.file)) throw new CatalogueError(`${JSON.stringify(plugin.file)} is not a plugin file name`);
  if (!SHA256.test(plugin.sha256)) throw new CatalogueError(`${label} has no valid SHA-256 in the plugin index`);
  const url = pluginFileUrl(baseUrl, label);
  for (const init of [undefined, { cache: 'no-cache' } as RequestInit]) {
    const buffer = await (await fetchOk(url, `download ${label}`, fetchImpl, init)).arrayBuffer();
    if (buffer.byteLength === plugin.sizeBytes && (await sha256Hex(buffer)) === plugin.sha256.toLowerCase()) {
      return new Uint8Array(buffer);
    }
  }
  throw new CatalogueError(`${label} does not match the plugin index; reload the page and try again`);
}

/** Fetches a plugin's manual.md as text. */
export async function fetchManual(baseUrl: string, plugin: PluginIndexEntry, fetchImpl: typeof fetch = fetch): Promise<string> {
  if (!inFolder(plugin.id, plugin.manual)) throw new CatalogueError(`${plugin.manual} is not in the plugin folder`);
  const response = await fetchOk(pluginFileUrl(baseUrl, plugin.manual), `load the manual of ${plugin.name}`, fetchImpl);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.length > MAX_TEXT_BYTES) throw new CatalogueError(`the manual of ${plugin.name} is too large`);
  return new TextDecoder().decode(bytes);
}
