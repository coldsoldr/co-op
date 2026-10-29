/**
 * Validates the community plugins in the repository's plugins/ folder and builds their index. Used
 * by the plugins CLI (scripts/plugins.ts), the production build and the Vite dev server, so all
 * three see exactly the same index. The sibling of packs-lib.ts, whose listing helpers it shares.
 */
import { createHash } from 'node:crypto';
import { copyFile, lstat, mkdir, readFile, realpath, rm, writeFile } from 'node:fs/promises';
import { dirname, isAbsolute, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { IncomingMessage, ServerResponse } from 'node:http';
import {
  MANIFEST_FILE,
  MAX_TEXT_BYTES,
  PLUGIN_INDEX_FORMAT,
  parsePluginManifest,
  pluginIndexEntry,
  validatePluginFiles,
  type PluginIndex,
  type PluginIndexEntry,
  type PluginIssue,
} from '@co-op/shared/plugins';
import { listFiles, stripBom, subfolders } from './packs-lib';

/** The repository's plugins/ folder. It need not exist: then the index is empty. */
export const DEFAULT_PLUGINS_DIR = fileURLToPath(new URL('../../plugins', import.meta.url));

export interface LoadOptions {
  /** Stamped into the index as generatedAt. Defaults to the current time. */
  now?: Date;
}

export interface PluginsResult {
  issues: PluginIssue[];
  /** Things worth a look that do not stop a plugin, such as an author in the file that differs from plugin.json. */
  warnings: PluginIssue[];
  /** Every valid plugin, sorted by id. Plugins with issues are left out. */
  index: PluginIndex;
}

interface Loaded {
  issues: PluginIssue[];
  warnings: PluginIssue[];
  entry: PluginIndexEntry | null;
}

async function loadPlugin(pluginsDir: string, folder: string): Promise<Loaded> {
  const dir = join(pluginsDir, folder);
  const fail = (message: string): Loaded => ({
    issues: [{ plugin: folder, file: MANIFEST_FILE, message }],
    warnings: [],
    entry: null,
  });
  const { files, odd } = await listFiles(dir);
  if (odd.length > 0) {
    return { issues: odd.map(({ path, message }) => ({ plugin: folder, file: path, message })), warnings: [], entry: null };
  }
  const manifestFile = files.find((f) => f.path === MANIFEST_FILE);
  if (!manifestFile) return fail('missing');
  if (manifestFile.size > MAX_TEXT_BYTES) {
    return fail(`is ${manifestFile.size} bytes, over the 64 KB (${MAX_TEXT_BYTES} bytes) limit`);
  }
  let json: unknown;
  try {
    json = JSON.parse(stripBom(await readFile(join(dir, MANIFEST_FILE), 'utf8')));
  } catch (error) {
    return fail(`not valid JSON: ${(error as Error).message}`);
  }
  const { manifest, issues } = parsePluginManifest(json, folder);
  if (!manifest) return { issues, warnings: [], entry: null };

  const read = new Map<string, Uint8Array>();
  const result = await validatePluginFiles(manifest, files, async (path) => {
    const bytes = new Uint8Array(await readFile(join(dir, path)));
    read.set(path, bytes);
    return bytes;
  });
  if (result.issues.length > 0 || !result.info) return { issues: result.issues, warnings: result.warnings, entry: null };

  const bytes = read.get(manifest.file) as Uint8Array;
  const entry = pluginIndexEntry(
    manifest,
    { sizeBytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') },
    result.info,
  );
  return { issues: [], warnings: result.warnings, entry };
}

/** Validates every plugin in `pluginsDir` and builds the index of the valid ones. */
export async function loadPlugins(pluginsDir: string, options: LoadOptions = {}): Promise<PluginsResult> {
  const issues: PluginIssue[] = [];
  const warnings: PluginIssue[] = [];
  const plugins: PluginIndexEntry[] = [];
  const { folders, links } = await subfolders(pluginsDir);
  for (const link of links) issues.push({ plugin: link, message: 'is a link; add the real folder instead' });
  for (const folder of folders) {
    const result = await loadPlugin(pluginsDir, folder);
    issues.push(...result.issues);
    warnings.push(...result.warnings);
    if (result.entry) plugins.push(result.entry);
  }
  const generatedAt = (options.now ?? new Date()).toISOString();
  return { issues, warnings, index: { format: PLUGIN_INDEX_FORMAT, generatedAt, plugins } };
}

/** Issues (or warnings) as text, grouped by plugin in first-seen order, with a count line at the end. */
export function formatIssues(issues: readonly PluginIssue[], noun = 'issue'): string {
  const groups = new Map<string, PluginIssue[]>();
  for (const issue of issues) groups.set(issue.plugin, [...(groups.get(issue.plugin) ?? []), issue]);
  const lines: string[] = [];
  for (const [plugin, list] of groups) {
    lines.push(plugin);
    for (const issue of list) lines.push(issue.file === undefined ? `  ${issue.message}` : `  ${issue.file}: ${issue.message}`);
  }
  const plural = (n: number, word: string): string => `${n} ${word}${n === 1 ? '' : 's'}`;
  lines.push('', `${plural(issues.length, noun)} in ${plural(groups.size, 'plugin')}`);
  return lines.join('\n');
}

/** The published files of a valid plugin, relative to the plugins folder: plugin.json, the plugin file, manual and screenshot. */
function publishedPaths(plugin: PluginIndexEntry): string[] {
  return [`${plugin.id}/${MANIFEST_FILE}`, `${plugin.id}/${plugin.file}`, plugin.manual, plugin.screenshot];
}

/**
 * Validates every plugin and, only if none has an issue, replaces `<outDir>/plugins` with the four
 * files of each plugin plus the generated index.json. Without a plugins folder the index is empty.
 */
export async function buildPlugins(pluginsDir: string, outDir: string, options: LoadOptions = {}): Promise<PluginsResult> {
  const result = await loadPlugins(pluginsDir, options);
  if (result.issues.length > 0) return result;
  const target = join(outDir, 'plugins');
  await rm(target, { recursive: true, force: true });
  await mkdir(target, { recursive: true });
  for (const plugin of result.index.plugins) {
    for (const path of publishedPaths(plugin)) {
      const to = join(target, ...path.split('/'));
      await mkdir(dirname(to), { recursive: true });
      await copyFile(join(pluginsDir, ...path.split('/')), to);
    }
  }
  await writeFile(join(target, 'index.json'), `${JSON.stringify(result.index, null, 2)}\n`);
  return result;
}

export interface MiddlewareOptions {
  /** The app's base path, for example '/co-op/'. */
  base: string;
  now?: () => Date;
  /** Told about plugin issues and warnings each time the index is built. */
  warn?: (message: string) => void;
}

type Next = (error?: unknown) => void;

const contentType = (file: string): string =>
  file.endsWith('.png')
    ? 'image/png'
    : file.endsWith('.json')
      ? 'application/json'
      : file.endsWith('.md')
        ? 'text/markdown; charset=utf-8'
        : 'application/octet-stream';

/**
 * A connect-style handler for the Vite dev server. `<base>plugins/index.json` is built on the fly
 * (valid plugins only; issues and warnings go to `warn`). `<base>plugins/<id>/<file>` is served
 * only when the freshly built index has that plugin and `<file>` is one of its four published
 * files, spelled exactly, and only when the real path still lies inside the plugin folder.
 * Anything else under `<base>plugins/` is a 404; other URLs are passed on.
 */
export function pluginsMiddleware(pluginsDir: string, options: MiddlewareOptions) {
  const prefix = `${options.base.replace(/\/?$/, '/')}plugins/`;

  async function load(): Promise<PluginIndex> {
    const { issues, warnings, index } = await loadPlugins(pluginsDir, { now: options.now?.() ?? new Date() });
    if (issues.length > 0) options.warn?.(`plugin issues (these plugins are left out):\n${formatIssues(issues)}`);
    if (warnings.length > 0) options.warn?.(`plugin warnings:\n${formatIssues(warnings, 'warning')}`);
    return index;
  }

  async function handle(req: IncomingMessage, res: ServerResponse, next: Next): Promise<void> {
    const url = (req.url ?? '').split('?')[0] as string;
    if (!url.startsWith(prefix)) return next();
    const notFound = (): void => {
      res.statusCode = 404;
      res.end('not found');
    };
    const rest = url.slice(prefix.length);
    if (rest === 'index.json') {
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify(await load()));
      return;
    }

    let segments: string[];
    try {
      segments = rest.split('/').map(decodeURIComponent);
    } catch {
      return notFound();
    }
    // Belt and braces before the allow-list: no empty, dot, separator or drive-letter segments.
    const bad = (s: string): boolean => s === '' || s === '.' || s === '..' || /[\\/:]/.test(s);
    const [id, ...fileParts] = segments;
    if (id === undefined || fileParts.length === 0 || segments.some(bad)) return notFound();
    const plugin = (await load()).plugins.find((p) => p.id === id);
    if (!plugin || !publishedPaths(plugin).includes(segments.join('/'))) return notFound();

    let bytes: Buffer;
    try {
      const pluginRoot = await realpath(join(pluginsDir, id));
      const full = await realpath(join(pluginRoot, ...fileParts));
      const rel = relative(pluginRoot, full);
      if (rel === '' || isAbsolute(rel) || rel.split(sep)[0] === '..') return notFound();
      if (!(await lstat(full)).isFile()) return notFound();
      bytes = await readFile(full);
    } catch {
      return notFound();
    }
    res.setHeader('content-type', contentType(fileParts.join('/')));
    res.end(bytes);
  }

  return (req: IncomingMessage, res: ServerResponse, next: Next): void => {
    handle(req, res, next).catch(next);
  };
}
