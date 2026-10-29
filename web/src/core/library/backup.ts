import { Zip, ZipPassThrough, strFromU8, strToU8, unzipSync } from 'fflate';
import { writeAtomic } from '../store/atomic';
import { StoreError, joinPath, splitPath, type DeviceStore } from '../store/store';
import { getProfile, profiles } from '../profiles/registry';
import type { DeviceProfile } from '../profiles/types';
import { byCodePoint, bytesEqual, sha256Hex } from '../util';

/** The manifest's name inside a backup zip. */
export const MANIFEST_FILE = 'co-op-backup.json';

export interface BackupFileInfo {
  path: string;
  size: number;
  /** Lower-case hex SHA-256 of the file bytes. */
  sha256: string;
}

export interface BackupManifest {
  format: 1;
  /** ISO 8601 time the backup was made. */
  createdAt: string;
  /** The profile id the backup was made with, e.g. 'op1/stock'. */
  profile: string;
  /** The store label at backup time. */
  label: string;
  /** Every backed-up file, in code-point order of path. */
  files: BackupFileInfo[];
}

export interface Backup {
  manifest: BackupManifest;
  /** Every file listed in the manifest, verified against its size and sha256. */
  files: Map<string, Uint8Array>;
}

export interface BackupDiffEntry {
  path: string;
  /** 'new': not on the device; 'changed': on the device with different bytes (or as a folder); 'same': identical. */
  status: 'new' | 'changed' | 'same';
}

export interface BackupDiff {
  /** One entry per backed-up file, in manifest order. */
  entries: BackupDiffEntry[];
  /** Files under the covered roots that the backup does not have; restore never deletes them. */
  onDeviceOnly: string[];
}

export interface CreateBackupOptions {
  /** Called after each file with the running count. */
  onProgress?: (done: number, total: number) => void;
  /** The backup time, for createdAt and zip entry times. Defaults to the current time. */
  now?: () => Date;
  /** Back up only these files; each must be a backed-up file under the backed-up folders. */
  paths?: readonly string[];
}

export class BackupError extends Error {
  override name = 'BackupError';
}

/** A restore stopped part-way; `restored` lists the paths written before the failure. */
export class RestoreError extends BackupError {
  override name = 'RestoreError';
  readonly restored: string[];

  constructor(message: string, restored: string[], options?: ErrorOptions) {
    super(message, options);
    this.restored = restored;
  }
}

/** Every file below `root` at any depth, skipping dot-prefixed files and folders. */
async function walkFiles(store: DeviceStore, root: string, out: string[]): Promise<void> {
  for (const entry of await store.list(root)) {
    if (entry.name.startsWith('.')) continue;
    const path = joinPath(root, entry.name);
    if (entry.kind === 'dir') await walkFiles(store, path, out);
    else out.push(path);
  }
}

async function filesUnder(store: DeviceStore, roots: readonly string[]): Promise<string[]> {
  const out: string[] = [];
  for (const root of roots) await walkFiles(store, root, out);
  return [...new Set(out)].sort(byCodePoint);
}

/** The folders a profile's backups hold: its patch roots plus any opted-in other roots (opus: mods). */
function backupRoots(profile: DeviceProfile): string[] {
  return [...profile.layout.patchRoots.map((r) => r.path), ...(profile.layout.backupOtherRoots ?? [])];
}

/** Collects a streaming Zip's output; resolves once the archive is finished. */
function collectZip(): { zip: Zip; done: Promise<Uint8Array[]> } {
  const chunks: Uint8Array[] = [];
  let zip!: Zip;
  const done = new Promise<Uint8Array[]>((resolve, reject) => {
    zip = new Zip((error, data, final) => {
      if (error) {
        reject(error);
        return;
      }
      chunks.push(data);
      if (final) resolve(chunks);
    });
  });
  return { zip, done };
}

/** Adds one stored (uncompressed) entry to a streaming zip. */
function addStored(zip: Zip, path: string, bytes: Uint8Array, mtime: Date): void {
  const entry = new ZipPassThrough(path);
  entry.mtime = mtime;
  zip.add(entry);
  entry.push(bytes, true);
}

/**
 * Zips every file under the profile's patch roots and opted-in other roots such as opus's mods
 * folder (all files, not only patches, so nothing there
 * is lost; dot files skipped), or only `options.paths`, with their bytes untouched, plus a manifest
 * of sizes and SHA-256s. Files are streamed into the archive one at a time, hashed as they are
 * read, and stored uncompressed; every entry is stamped with `now()`, so the zip is deterministic
 * for a given time. Works on read-only stores.
 */
export async function createBackup(
  store: DeviceStore,
  profile: DeviceProfile,
  options: CreateBackupOptions = {},
): Promise<Blob> {
  const createdAt = (options.now ?? (() => new Date()))();
  let paths = await filesUnder(store, backupRoots(profile));
  if (options.paths) {
    const available = new Set(paths);
    const unknown = options.paths.filter((p) => !available.has(p));
    if (unknown.length > 0) {
      throw new BackupError(`cannot back up ${unknown.join(', ')}: not a file under the backed-up folders`);
    }
    paths = [...new Set(options.paths)].sort(byCodePoint);
  }

  const { zip, done } = collectZip();
  const files: BackupFileInfo[] = [];
  for (const [i, path] of paths.entries()) {
    const bytes = await store.read(path);
    files.push({ path, size: bytes.length, sha256: await sha256Hex(bytes) });
    addStored(zip, path, bytes, createdAt);
    options.onProgress?.(i + 1, paths.length);
  }
  const manifest: BackupManifest = {
    format: 1,
    createdAt: createdAt.toISOString(),
    profile: profile.id,
    label: store.label,
    files,
  };
  addStored(zip, MANIFEST_FILE, strToU8(JSON.stringify(manifest, null, 2)), createdAt);
  zip.end();
  return new Blob((await done) as Uint8Array<ArrayBuffer>[], { type: 'application/zip' });
}

/** A relative drive path with no empty, '.', '..' or dot-prefixed segment, and no backslash. */
function isSafePath(path: string): boolean {
  if (path === '' || path.startsWith('/') || path.endsWith('/') || path.includes('\\')) return false;
  return splitPath(path).length === path.split('/').length && splitPath(path).every((s) => !s.startsWith('.'));
}

/**
 * The folders a backup may hold and that diffBackup compares: the profile's backup roots, or, for a
 * profile this build does not know, the union of every known profile's backup roots, so a backup
 * from elsewhere can never write outside the folders co-op manages.
 */
function coveredRoots(manifest: BackupManifest): string[] {
  const profile = getProfile(manifest.profile);
  if (profile) return backupRoots(profile);
  const roots = new Map<string, string>();
  for (const known of profiles) for (const root of backupRoots(known)) roots.set(root.toLowerCase(), root);
  return [...roots.values()];
}

/** True when `path` is inside one of `roots` (compared ignoring case, as on the FAT drive). */
function isUnder(path: string, roots: readonly string[]): boolean {
  const lower = path.toLowerCase();
  return roots.some((root) => lower.startsWith(`${root.toLowerCase()}/`));
}

function parseManifest(text: string): BackupManifest {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch (error) {
    throw new BackupError(`${MANIFEST_FILE} is not valid JSON`, { cause: error });
  }
  const m = value as Partial<BackupManifest> | null;
  if (typeof m !== 'object' || m === null) throw new BackupError(`${MANIFEST_FILE} is not an object`);
  if (m.format !== 1) throw new BackupError(`unsupported backup format ${String(m.format)}; expected 1`);
  if (typeof m.createdAt !== 'string' || typeof m.profile !== 'string' || typeof m.label !== 'string') {
    throw new BackupError(`${MANIFEST_FILE} is missing createdAt, profile or label`);
  }
  if (!Array.isArray(m.files)) throw new BackupError(`${MANIFEST_FILE} has no files list`);
  for (const f of m.files as unknown[]) {
    const file = f as Partial<BackupFileInfo> | null;
    if (
      typeof file !== 'object' || file === null ||
      typeof file.path !== 'string' || typeof file.size !== 'number' || typeof file.sha256 !== 'string'
    ) {
      throw new BackupError(`${MANIFEST_FILE} has a malformed file entry`);
    }
  }
  const manifest = m as BackupManifest;
  const roots = coveredRoots(manifest);
  const seen = new Map<string, string>();
  for (const { path } of manifest.files) {
    if (!isSafePath(path)) throw new BackupError(`backup lists an unsafe path: ${JSON.stringify(path)}`);
    if (!isUnder(path, roots)) {
      throw new BackupError(`backup lists ${path}, which is outside the folders it covers (${roots.join(', ')})`);
    }
    const twin = seen.get(path.toLowerCase());
    if (twin !== undefined) {
      throw new BackupError(`backup lists ${twin} and ${path}, which name the same file on a case-insensitive drive`);
    }
    seen.set(path.toLowerCase(), path);
  }
  return manifest;
}

function unzip(bytes: Uint8Array, keep: (name: string) => boolean): Record<string, Uint8Array> {
  try {
    return unzipSync(bytes, { filter: (file) => keep(file.name) });
  } catch (error) {
    throw new BackupError('not a zip file', { cause: error });
  }
}

/**
 * Opens a backup zip and verifies it: a format 1 manifest, only safe relative paths inside the
 * covered folders with no two differing only in case, and every listed file present with the
 * listed size and SHA-256. Throws BackupError naming the problem. The manifest is read first and
 * only the files it lists are inflated; other entries are never extracted.
 */
export async function readBackup(input: Uint8Array | Blob): Promise<Backup> {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(await input.arrayBuffer());
  const manifestBytes = unzip(bytes, (name) => name === MANIFEST_FILE)[MANIFEST_FILE];
  if (!manifestBytes) throw new BackupError(`not a co-op backup: ${MANIFEST_FILE} is missing`);
  const manifest = parseManifest(strFromU8(manifestBytes));
  const listed = new Set(manifest.files.map((f) => f.path));
  const entries = unzip(bytes, (name) => listed.has(name));
  const files = new Map<string, Uint8Array>();
  for (const info of manifest.files) {
    const data = Object.hasOwn(entries, info.path) ? entries[info.path] : undefined;
    if (!data) throw new BackupError(`backup is incomplete: ${info.path} is missing`);
    if (data.length !== info.size || (await sha256Hex(data)) !== info.sha256.toLowerCase()) {
      throw new BackupError(`backup is damaged: ${info.path} does not match its checksum`);
    }
    files.set(info.path, data);
  }
  return { manifest, files };
}

/** Compares a backup with the device, byte for byte. */
export async function diffBackup(store: DeviceStore, backup: Backup): Promise<BackupDiff> {
  const entries: BackupDiffEntry[] = [];
  for (const { path } of backup.manifest.files) {
    if (!(await store.exists(path))) {
      entries.push({ path, status: 'new' });
      continue;
    }
    let current: Uint8Array | null = null;
    try {
      current = await store.read(path);
    } catch {
      // Present but unreadable as a file (for example a folder now has that name).
    }
    const same = current !== null && bytesEqual(current, backup.files.get(path) as Uint8Array);
    entries.push({ path, status: same ? 'same' : 'changed' });
  }
  const inBackup = new Set(backup.files.keys());
  const onDeviceOnly = (await filesUnder(store, coveredRoots(backup.manifest))).filter((p) => !inBackup.has(p));
  return { entries, onDeviceOnly };
}

/**
 * Writes each selected path from the backup to the device atomically, with the backed-up bytes
 * untouched, calling onProgress after each. Never deletes anything. Every path is checked before
 * the first write, so an unknown path or a read-only store changes nothing. A failure part-way
 * throws RestoreError listing the paths already restored.
 */
export async function applyRestore(
  store: DeviceStore,
  backup: Backup,
  paths: readonly string[],
  onProgress?: (path: string) => void,
): Promise<void> {
  if (!store.capabilities.write) throw new StoreError('read-only source: cannot restore a backup');
  const missing = paths.filter((path) => !backup.files.has(path));
  if (missing.length > 0) throw new BackupError(`not in this backup: ${missing.join(', ')}`);
  const restored: string[] = [];
  for (const path of paths) {
    try {
      await writeAtomic(store, path, backup.files.get(path) as Uint8Array);
    } catch (error) {
      const reason = error instanceof Error ? error.message : String(error);
      throw new RestoreError(
        `restoring ${path} failed after ${restored.length} of ${paths.length} files: ${reason}`,
        [...restored],
        { cause: error },
      );
    }
    restored.push(path);
    onProgress?.(path);
  }
}
