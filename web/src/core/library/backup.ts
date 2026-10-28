import { strFromU8, strToU8, unzipSync, zipSync, type Zippable } from 'fflate';
import { writeAtomic } from '../store/atomic';
import { StoreError, joinPath, splitPath, type DeviceStore } from '../store/store';
import { getProfile } from '../profiles/registry';
import type { DeviceProfile } from '../profiles/types';

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
  /** 'new': not on the device; 'changed': on the device with different bytes; 'same': identical. */
  status: 'new' | 'changed' | 'same';
}

export interface BackupDiff {
  /** One entry per backed-up file, in manifest order. */
  entries: BackupDiffEntry[];
  /** Files under the patch roots that the backup does not have; restore never deletes them. */
  onDeviceOnly: string[];
}

export class BackupError extends Error {
  override name = 'BackupError';
}

const byCodePoint = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

async function sha256Hex(bytes: Uint8Array): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes as Uint8Array<ArrayBuffer>));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
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

/**
 * Zips every file under the profile's patch roots (all files, not only patches, so nothing there
 * is lost; dot files skipped) with their bytes untouched, plus a manifest of sizes and SHA-256s.
 * Files are stored uncompressed and every entry is stamped with `now()`, so the zip is
 * deterministic for a given time. Works on read-only stores.
 */
export async function createBackup(
  store: DeviceStore,
  profile: DeviceProfile,
  onProgress?: (done: number, total: number) => void,
  now: () => Date = () => new Date(),
): Promise<Uint8Array> {
  const createdAt = now();
  const paths = await filesUnder(store, profile.layout.patchRoots.map((r) => r.path));
  const zip: Zippable = {};
  const files: BackupFileInfo[] = [];
  for (const [i, path] of paths.entries()) {
    const bytes = await store.read(path);
    zip[path] = [bytes, { level: 0, mtime: createdAt }];
    files.push({ path, size: bytes.length, sha256: await sha256Hex(bytes) });
    onProgress?.(i + 1, paths.length);
  }
  const manifest: BackupManifest = {
    format: 1,
    createdAt: createdAt.toISOString(),
    profile: profile.id,
    label: store.label,
    files,
  };
  zip[MANIFEST_FILE] = [strToU8(JSON.stringify(manifest, null, 2)), { mtime: createdAt }];
  return zipSync(zip);
}

/** A relative drive path with no empty, '.', '..' or dot-prefixed segment, and no backslash. */
function isSafePath(path: string): boolean {
  if (path === '' || path.startsWith('/') || path.endsWith('/') || path.includes('\\')) return false;
  return splitPath(path).length === path.split('/').length && splitPath(path).every((s) => !s.startsWith('.'));
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
  const seen = new Set<string>();
  for (const f of m.files as unknown[]) {
    const file = f as Partial<BackupFileInfo> | null;
    if (
      typeof file !== 'object' || file === null ||
      typeof file.path !== 'string' || typeof file.size !== 'number' || typeof file.sha256 !== 'string'
    ) {
      throw new BackupError(`${MANIFEST_FILE} has a malformed file entry`);
    }
    if (!isSafePath(file.path)) throw new BackupError(`backup lists an unsafe path: ${JSON.stringify(file.path)}`);
    if (seen.has(file.path)) throw new BackupError(`backup lists ${file.path} twice`);
    seen.add(file.path);
  }
  return m as BackupManifest;
}

/**
 * Opens a backup zip and verifies it: a format 1 manifest, every listed file present with the
 * listed size and SHA-256, and only safe relative paths. Throws BackupError naming the problem.
 * Zip entries the manifest does not list are ignored.
 */
export async function readBackup(zip: Uint8Array): Promise<Backup> {
  let entries: ReturnType<typeof unzipSync>;
  try {
    entries = unzipSync(zip);
  } catch (error) {
    throw new BackupError('not a zip file', { cause: error });
  }
  const manifestBytes = entries[MANIFEST_FILE];
  if (!manifestBytes) throw new BackupError(`not a co-op backup: ${MANIFEST_FILE} is missing`);
  const manifest = parseManifest(strFromU8(manifestBytes));
  const files = new Map<string, Uint8Array>();
  for (const info of manifest.files) {
    const bytes = Object.hasOwn(entries, info.path) ? entries[info.path] : undefined;
    if (!bytes) throw new BackupError(`backup is incomplete: ${info.path} is missing`);
    if (bytes.length !== info.size || (await sha256Hex(bytes)) !== info.sha256.toLowerCase()) {
      throw new BackupError(`backup is damaged: ${info.path} does not match its checksum`);
    }
    files.set(info.path, bytes);
  }
  return { manifest, files };
}

/** The folders a backup covers: its profile's patch roots, or its top-level folders for an unknown profile. */
function coveredRoots(backup: Backup): string[] {
  const profile = getProfile(backup.manifest.profile);
  if (profile) return profile.layout.patchRoots.map((r) => r.path);
  return [...new Set(backup.manifest.files.map((f) => splitPath(f.path)[0] as string))];
}

/** Compares a backup with the device, byte for byte. */
export async function diffBackup(store: DeviceStore, backup: Backup): Promise<BackupDiff> {
  const entries: BackupDiffEntry[] = [];
  for (const { path } of backup.manifest.files) {
    if (!(await store.exists(path))) {
      entries.push({ path, status: 'new' });
      continue;
    }
    const same = bytesEqual(await store.read(path), backup.files.get(path) as Uint8Array);
    entries.push({ path, status: same ? 'same' : 'changed' });
  }
  const inBackup = new Set(backup.files.keys());
  const onDeviceOnly = (await filesUnder(store, coveredRoots(backup))).filter((p) => !inBackup.has(p));
  return { entries, onDeviceOnly };
}

/**
 * Writes each selected path from the backup to the device atomically, with the backed-up bytes
 * untouched. Never deletes anything. Every path is checked before the first write, so an unknown
 * path or a read-only store changes nothing.
 */
export async function applyRestore(store: DeviceStore, backup: Backup, paths: readonly string[]): Promise<void> {
  if (!store.capabilities.write) throw new StoreError('read-only source: cannot restore a backup');
  const missing = paths.filter((path) => !backup.files.has(path));
  if (missing.length > 0) throw new BackupError(`not in this backup: ${missing.join(', ')}`);
  for (const path of paths) await writeAtomic(store, path, backup.files.get(path) as Uint8Array);
}
