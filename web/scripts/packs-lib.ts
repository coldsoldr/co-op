/**
 * Validates the community packs in the repository's packs/ folder and builds their index. Used by
 * the packs CLI (scripts/packs.ts), the production build and the Vite dev server, so all three
 * see exactly the same index.
 */
import { createHash } from 'node:crypto';
import { copyFile, lstat, mkdir, readFile, readdir, realpath, rm, writeFile } from 'node:fs/promises';
import { dirname, isAbsolute, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Dirent } from 'node:fs';
import type { IncomingMessage, ServerResponse } from 'node:http';
import {
  INDEX_FORMAT,
  MANIFEST_FILE,
  MAX_TEXT_BYTES,
  README_FILE,
  parseManifest,
  validatePackFiles,
  type PackIndex,
  type PackIndexEntry,
  type PackIndexPatch,
  type PackIssue,
  type PackKind,
  type ParsedPackPatch,
} from '@co-op/shared/packs';
import { readPatch } from '../src/core/codec/patch';
import { profiles } from '../src/core/profiles/registry';

/** The repository's packs/ folder. */
export const DEFAULT_PACKS_DIR = fileURLToPath(new URL('../../packs', import.meta.url));

export interface LoadOptions {
  /** Stamped into the index as generatedAt. Defaults to the current time. */
  now?: Date;
  /** Profile ids a pack may name. Defaults to every registered profile. */
  firmware?: readonly string[];
}

export interface PacksResult {
  issues: PackIssue[];
  /** Every valid pack, sorted by slug. Packs with issues are left out. */
  index: PackIndex;
}

const byCodePoint = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

const isMissing = (error: unknown): boolean => (error as NodeJS.ErrnoException).code === 'ENOENT';

interface Listing {
  /** Regular files, by '/'-separated path relative to the pack folder. */
  files: { path: string; size: number }[];
  /** Links and other entries that are neither a regular file nor a folder. */
  odd: { path: string; message: string }[];
}

/**
 * Every regular file below `dir` with its size, never following a link: a link (or a device,
 * socket or pipe) is listed in `odd` instead, so a pack can never pull in a file from elsewhere.
 */
async function listFiles(dir: string, prefix = '', out: Listing = { files: [], odd: [] }): Promise<Listing> {
  const entries = await readdir(dir, { withFileTypes: true });
  for (const entry of entries.sort((a, b) => byCodePoint(a.name, b.name))) {
    const path = prefix === '' ? entry.name : `${prefix}/${entry.name}`;
    const full = join(dir, entry.name);
    const info = await lstat(full);
    if (info.isSymbolicLink()) out.odd.push({ path, message: 'is a link; add the real file instead' });
    else if (info.isDirectory()) await listFiles(full, path, out);
    else if (info.isFile()) out.files.push({ path, size: info.size });
    else out.odd.push({ path, message: 'is not a regular file' });
  }
  return out;
}

/** The entries of `packsDir`, sorted: pack folders, and links (reported, never followed). */
async function packFolders(packsDir: string): Promise<{ folders: string[]; links: string[] }> {
  let entries: Dirent[];
  try {
    entries = await readdir(packsDir, { withFileTypes: true });
  } catch (error) {
    if (isMissing(error)) return { folders: [], links: [] };
    throw error;
  }
  const names = (keep: (e: Dirent) => boolean) => entries.filter(keep).map((e) => e.name).sort(byCodePoint);
  // Loose files (a README about packs) are ignored.
  return { folders: names((e) => e.isDirectory()), links: names((e) => e.isSymbolicLink()) };
}

/** Notepad and some other Windows editors start UTF-8 files with a byte order mark, which JSON.parse rejects. */
const stripBom = (text: string): string => (text.startsWith('\uFEFF') ? text.slice(1) : text);

/** Reads a patch file for the validator and the index. */
function describePatch(bytes: Uint8Array): ParsedPackPatch {
  const patch = readPatch(bytes);
  const engine = patch.kind === 'drum' ? 'drum' : typeof patch.meta?.type === 'string' ? patch.meta.type : '';
  return { kind: patch.kind, name: patch.name, engine };
}

async function loadPack(
  packsDir: string,
  folder: string,
  firmware: readonly string[],
): Promise<{ issues: PackIssue[]; entry: PackIndexEntry | null }> {
  const dir = join(packsDir, folder);
  const fail = (message: string): { issues: PackIssue[]; entry: null } => ({
    issues: [{ pack: folder, file: MANIFEST_FILE, message }],
    entry: null,
  });
  const { files, odd } = await listFiles(dir);
  if (odd.length > 0) return { issues: odd.map(({ path, message }) => ({ pack: folder, file: path, message })), entry: null };
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
  const { manifest, issues } = parseManifest(json, folder, firmware);
  if (!manifest) return { issues, entry: null };

  const parsed = new Map<string, { info: ParsedPackPatch; bytes: Uint8Array }>();
  const fileIssues = await validatePackFiles(manifest, files, async (path) => {
    const bytes = new Uint8Array(await readFile(join(dir, path)));
    const info = describePatch(bytes);
    parsed.set(path, { info, bytes });
    return info;
  });
  if (fileIssues.length > 0) return { issues: fileIssues, entry: null };

  const patches: PackIndexPatch[] = manifest.patches.map(({ file }) => {
    const { info, bytes } = parsed.get(file) as { info: ParsedPackPatch; bytes: Uint8Array };
    return {
      file,
      kind: info.kind as PackKind,
      name: info.name,
      engine: info.engine,
      sizeBytes: bytes.length,
      sha256: createHash('sha256').update(bytes).digest('hex'),
    };
  });
  const entry: PackIndexEntry = {
    slug: manifest.slug,
    name: manifest.name,
    author: manifest.author,
    description: manifest.description,
    license: manifest.license,
    firmware: manifest.firmware,
    tags: manifest.tags,
    sizeBytes: patches.reduce((sum, p) => sum + p.sizeBytes, 0),
    patches,
  };
  return { issues: [], entry };
}

/** Validates every pack in `packsDir` and builds the index of the valid ones. */
export async function loadPacks(packsDir: string, options: LoadOptions = {}): Promise<PacksResult> {
  const firmware = options.firmware ?? profiles.map((p) => p.id);
  const issues: PackIssue[] = [];
  const packs: PackIndexEntry[] = [];
  const { folders, links } = await packFolders(packsDir);
  for (const link of links) issues.push({ pack: link, message: 'is a link; add the real folder instead' });
  for (const folder of folders) {
    const result = await loadPack(packsDir, folder, firmware);
    issues.push(...result.issues);
    if (result.entry) packs.push(result.entry);
  }
  const generatedAt = (options.now ?? new Date()).toISOString();
  return { issues, index: { format: INDEX_FORMAT, generatedAt, packs } };
}

/** Issues as text, grouped by pack in first-seen order, with a count line at the end. */
export function formatIssues(issues: readonly PackIssue[]): string {
  const groups = new Map<string, PackIssue[]>();
  for (const issue of issues) groups.set(issue.pack, [...(groups.get(issue.pack) ?? []), issue]);
  const lines: string[] = [];
  for (const [pack, list] of groups) {
    lines.push(pack);
    for (const issue of list) lines.push(issue.file === undefined ? `  ${issue.message}` : `  ${issue.file}: ${issue.message}`);
  }
  const plural = (n: number, word: string): string => `${n} ${word}${n === 1 ? '' : 's'}`;
  lines.push('', `${plural(issues.length, 'issue')} in ${plural(groups.size, 'pack')}`);
  return lines.join('\n');
}

/** The files of a valid pack that may be published: pack.json, README.md and the listed patches. */
function publishedFiles(pack: PackIndexEntry): string[] {
  return [MANIFEST_FILE, README_FILE, ...pack.patches.map((p) => p.file)];
}

/**
 * Validates every pack and, only if none has an issue, replaces `<outDir>/packs` with pack.json,
 * README.md (when present) and the listed patches of each pack, plus the generated index.json.
 */
export async function buildPacks(packsDir: string, outDir: string, options: LoadOptions = {}): Promise<PacksResult> {
  const result = await loadPacks(packsDir, options);
  if (result.issues.length > 0) return result;
  const target = join(outDir, 'packs');
  await rm(target, { recursive: true, force: true });
  await mkdir(target, { recursive: true });
  for (const pack of result.index.packs) {
    for (const file of publishedFiles(pack)) {
      const from = join(packsDir, pack.slug, file);
      if (file === README_FILE && !(await exists(from))) continue;
      const to = join(target, pack.slug, file);
      await mkdir(dirname(to), { recursive: true });
      await copyFile(from, to);
    }
  }
  await writeFile(join(target, 'index.json'), `${JSON.stringify(result.index, null, 2)}\n`);
  return result;
}

async function exists(path: string): Promise<boolean> {
  try {
    await lstat(path);
    return true;
  } catch (error) {
    if (isMissing(error)) return false;
    throw error;
  }
}

export interface MiddlewareOptions {
  /** The app's base path, for example '/co-op/'. */
  base: string;
  firmware?: readonly string[];
  now?: () => Date;
  /** Told about pack issues each time the index is built. */
  warn?: (message: string) => void;
}

type Next = (error?: unknown) => void;

const contentType = (file: string): string =>
  /\.aiff?$/i.test(file) ? 'audio/aiff' : file.endsWith('.json') ? 'application/json' : file.endsWith('.md') ? 'text/markdown; charset=utf-8' : 'application/octet-stream';

/**
 * A connect-style handler for the Vite dev server. `<base>packs/index.json` is built on the fly
 * (valid packs only; issues go to `warn`). `<base>packs/<slug>/<file>` is served only when the
 * freshly built index has that pack and `<file>` is its pack.json, README.md or a listed patch,
 * spelled exactly, and only when the real path still lies inside the pack folder. Anything else
 * under `<base>packs/` is a 404; other URLs are passed on.
 */
export function packsMiddleware(packsDir: string, options: MiddlewareOptions) {
  const prefix = `${options.base.replace(/\/?$/, '/')}packs/`;

  async function load(): Promise<PackIndex> {
    const { issues, index } = await loadPacks(packsDir, {
      ...(options.firmware ? { firmware: options.firmware } : {}),
      now: options.now?.() ?? new Date(),
    });
    if (issues.length > 0) options.warn?.(`pack issues (these packs are left out):\n${formatIssues(issues)}`);
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
    const [slug, ...fileParts] = segments;
    if (slug === undefined || fileParts.length === 0 || segments.some(bad)) return notFound();
    const file = fileParts.join('/');
    const pack = (await load()).packs.find((p) => p.slug === slug);
    if (!pack || !publishedFiles(pack).includes(file)) return notFound();

    let bytes: Buffer;
    try {
      const packRoot = await realpath(join(packsDir, slug));
      const full = await realpath(join(packRoot, ...fileParts));
      const rel = relative(packRoot, full);
      if (rel === '' || isAbsolute(rel) || rel.split(sep)[0] === '..') return notFound();
      if (!(await lstat(full)).isFile()) return notFound();
      bytes = await readFile(full);
    } catch {
      return notFound();
    }
    res.setHeader('content-type', contentType(file));
    res.end(bytes);
  }

  return (req: IncomingMessage, res: ServerResponse, next: Next): void => {
    handle(req, res, next).catch(next);
  };
}
