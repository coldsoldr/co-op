/**
 * Validates the community packs in the repository's packs/ folder and builds their index. Used by
 * the packs CLI (scripts/packs.ts), the production build and the Vite dev server, so all three
 * see exactly the same index.
 */
import { createHash } from 'node:crypto';
import { cp, mkdir, readFile, readdir, rm, stat, writeFile } from 'node:fs/promises';
import { join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { IncomingMessage, ServerResponse } from 'node:http';
import {
  INDEX_FORMAT,
  MANIFEST_FILE,
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

/** Every file below `dir`, by '/'-separated path relative to it, with its size. */
async function listFiles(dir: string, prefix = ''): Promise<{ path: string; size: number }[]> {
  const out: { path: string; size: number }[] = [];
  const entries = await readdir(dir, { withFileTypes: true });
  for (const entry of entries.sort((a, b) => byCodePoint(a.name, b.name))) {
    const path = prefix === '' ? entry.name : `${prefix}/${entry.name}`;
    const full = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...(await listFiles(full, path)));
    else out.push({ path, size: (await stat(full)).size });
  }
  return out;
}

/** The pack folder names in `packsDir`, sorted; loose files there are ignored. */
async function packFolders(packsDir: string): Promise<string[]> {
  try {
    const entries = await readdir(packsDir, { withFileTypes: true });
    return entries.filter((e) => e.isDirectory()).map((e) => e.name).sort(byCodePoint);
  } catch (error) {
    if (isMissing(error)) return [];
    throw error;
  }
}

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
  let text: string;
  try {
    text = await readFile(join(dir, MANIFEST_FILE), 'utf8');
  } catch (error) {
    if (isMissing(error)) return fail('missing');
    throw error;
  }
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch (error) {
    return fail(`not valid JSON: ${(error as Error).message}`);
  }
  const { manifest, issues } = parseManifest(json, folder, firmware);
  if (!manifest) return { issues, entry: null };

  const files = await listFiles(dir);
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
  for (const folder of await packFolders(packsDir)) {
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

/**
 * Validates every pack and, only if none has an issue, replaces `<outDir>/packs` with a copy of
 * each pack folder and the generated index.json.
 */
export async function buildPacks(packsDir: string, outDir: string, options: LoadOptions = {}): Promise<PacksResult> {
  const result = await loadPacks(packsDir, options);
  if (result.issues.length > 0) return result;
  const target = join(outDir, 'packs');
  await rm(target, { recursive: true, force: true });
  await mkdir(target, { recursive: true });
  for (const pack of result.index.packs) {
    await cp(join(packsDir, pack.slug), join(target, pack.slug), { recursive: true });
  }
  await writeFile(join(target, 'index.json'), `${JSON.stringify(result.index, null, 2)}\n`);
  return result;
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

/**
 * A connect-style handler for the Vite dev server: `<base>packs/index.json` is built on the fly
 * (valid packs only; issues go to `warn`) and `<base>packs/<path>` serves a file from `packsDir`.
 * Anything else is passed on.
 */
export function packsMiddleware(packsDir: string, options: MiddlewareOptions) {
  const prefix = `${options.base.replace(/\/?$/, '/')}packs/`;
  const root = resolve(packsDir);

  async function handle(req: IncomingMessage, res: ServerResponse, next: Next): Promise<void> {
    const url = (req.url ?? '').split('?')[0] as string;
    if (!url.startsWith(prefix)) return next();
    const rest = url.slice(prefix.length);
    const notFound = (): void => {
      res.statusCode = 404;
      res.end('not found');
    };

    if (rest === 'index.json') {
      const { issues, index } = await loadPacks(packsDir, {
        ...(options.firmware ? { firmware: options.firmware } : {}),
        now: options.now?.() ?? new Date(),
      });
      if (issues.length > 0) options.warn?.(`pack issues (these packs are left out):\n${formatIssues(issues)}`);
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify(index));
      return;
    }

    let segments: string[];
    try {
      segments = rest.split('/').map(decodeURIComponent);
    } catch {
      return notFound();
    }
    if (segments.some((s) => s === '' || s === '.' || s === '..' || s.includes('\\') || s.includes('/'))) return notFound();
    const full = resolve(root, ...segments);
    if (!relative(root, full).split(sep).every((s) => s !== '..') || full === root) return notFound();
    let bytes: Buffer;
    try {
      if (!(await stat(full)).isFile()) return notFound();
      bytes = await readFile(full);
    } catch {
      return notFound();
    }
    res.setHeader('content-type', /\.aiff?$/i.test(full) ? 'audio/aiff' : full.endsWith('.json') ? 'application/json' : 'application/octet-stream');
    res.end(bytes);
  }

  return (req: IncomingMessage, res: ServerResponse, next: Next): void => {
    handle(req, res, next).catch(next);
  };
}
