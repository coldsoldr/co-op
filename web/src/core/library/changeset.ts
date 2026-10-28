import { strToU8, zipSync, type Zippable } from 'fflate';
import type { DirEntry } from '../store/listing';
import { StoreError, basename, dirname, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from '../store/store';

/** One file-level change against the base store, in its final state. */
export type Change = { op: 'write'; path: string; bytes: Uint8Array } | { op: 'remove'; path: string };

/** The instructions file inside an exported change set. */
export const CHANGES_FILE = 'CHANGES.txt';

const byCodePoint = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/** Every ancestor folder of a path, outermost first, excluding the root and the path itself. */
function ancestors(key: string): string[] {
  const parts = splitPath(key);
  return parts.slice(0, -1).map((_, i) => parts.slice(0, i + 1).join('/'));
}

/**
 * A writable DeviceStore layered over a read-only one (a dropped folder). Reads see staged bytes
 * first, then the base; writes, moves, removals and new folders are recorded in an overlay and the
 * base is never touched. The overlay is kept minimal as it goes: a write that restores a base
 * file's exact bytes drops back to the base, and a file created then removed leaves no trace, so
 * changes() is the final state only.
 */
export class StagedStore implements DeviceStore {
  readonly capabilities: StoreCapabilities = { write: true };
  readonly label: string;
  /** Files whose current bytes differ from the base (or that the base lacks). */
  private readonly staged = new Map<string, Uint8Array>();
  /** Base files that are currently deleted. */
  private readonly removed = new Set<string>();
  /** Folders created here; base folders are not listed. */
  private readonly dirs = new Set<string>();

  constructor(private readonly base: DeviceStore) {
    this.label = base.label;
  }

  async list(path: string): Promise<DirEntry[]> {
    const dir = joinPath(path);
    if (!(await this.isDir(dir))) return [];
    const entries = new Map<string, DirEntry['kind']>();
    for (const entry of await this.base.list(dir)) {
      if (entry.kind === 'file' && this.removed.has(joinPath(dir, entry.name))) continue;
      entries.set(entry.name, entry.kind);
    }
    for (const file of this.staged.keys()) if (dirname(file) === dir) entries.set(basename(file), 'file');
    for (const folder of this.dirs) if (dirname(folder) === dir) entries.set(basename(folder), 'dir');
    return [...entries.keys()].sort(byCodePoint).map((name) => ({ name, kind: entries.get(name) as DirEntry['kind'] }));
  }

  async read(path: string): Promise<Uint8Array> {
    const key = joinPath(path);
    const bytes = this.staged.get(key);
    if (bytes) return bytes.slice();
    if (this.removed.has(key)) throw new StoreError(`no such file: ${key}`);
    return this.base.read(key);
  }

  async size(path: string): Promise<number> {
    const key = joinPath(path);
    const bytes = this.staged.get(key);
    if (bytes) return bytes.length;
    if (this.removed.has(key)) throw new StoreError(`no such file: ${key}`);
    return this.base.size(key);
  }

  async exists(path: string): Promise<boolean> {
    const key = joinPath(path);
    return (await this.isFile(key)) || (await this.isDir(key));
  }

  async write(path: string, bytes: Uint8Array): Promise<void> {
    const key = joinPath(path);
    if (await this.isDir(key)) throw new StoreError(`cannot write ${key}: it is a folder`);
    await this.addParents(key);
    this.removed.delete(key);
    if ((await this.baseHasFile(key)) && bytesEqual(await this.base.read(key), bytes)) {
      this.staged.delete(key);
    } else {
      this.staged.set(key, bytes.slice());
    }
  }

  async move(from: string, to: string): Promise<void> {
    const source = joinPath(from);
    const target = joinPath(to);
    if (!(await this.isFile(source))) throw new StoreError(`cannot move ${source}: no such file`);
    if (await this.exists(target)) throw new StoreError(`cannot move ${source} to ${target}: target exists`);
    await this.write(target, await this.read(source));
    await this.remove(source);
  }

  async remove(path: string): Promise<void> {
    const key = joinPath(path);
    if (!(await this.isFile(key))) {
      throw new StoreError((await this.isDir(key)) ? `${key} is a folder, not a file` : `no such file: ${key}`);
    }
    this.staged.delete(key);
    if (await this.baseHasFile(key)) this.removed.add(key);
  }

  async mkdir(path: string): Promise<void> {
    const key = joinPath(path);
    if (key === '') return;
    await this.addParents(joinPath(key, 'x'));
  }

  /** The net changes against the base, sorted by path. Written bytes are the staged buffers, not copies. */
  changes(): Change[] {
    const out: Change[] = [];
    for (const [path, bytes] of this.staged) out.push({ op: 'write', path, bytes });
    for (const path of this.removed) if (!this.staged.has(path)) out.push({ op: 'remove', path });
    return out.sort((a, b) => byCodePoint(a.path, b.path));
  }

  private async baseHasFile(key: string): Promise<boolean> {
    if (key === '') return false;
    try {
      await this.base.size(key);
      return true;
    } catch {
      return false;
    }
  }

  private async isFile(key: string): Promise<boolean> {
    if (this.staged.has(key)) return true;
    return !this.removed.has(key) && (await this.baseHasFile(key));
  }

  private async isDir(key: string): Promise<boolean> {
    if (key === '' || this.dirs.has(key)) return true;
    return !(await this.baseHasFile(key)) && (await this.base.exists(key));
  }

  /** Registers every ancestor folder of a file path; throws if one of them is a file. */
  private async addParents(filePath: string): Promise<void> {
    const folders = ancestors(filePath);
    for (const folder of folders) {
      if (await this.isFile(folder)) throw new StoreError(`cannot create folder ${folder}: a file has that name`);
    }
    for (const folder of folders) this.dirs.add(folder);
  }
}

/**
 * Packs a change set as a zip for the user to apply by hand: every written file at its drive path
 * (stored uncompressed; audio barely compresses) plus CHANGES.txt with the instructions and the
 * files to delete. Empty folders created with mkdir are not carried; the OP-1 ignores them anyway.
 */
export function exportChangeSet(changes: readonly Change[]): Uint8Array {
  const zip: Zippable = {};
  const removals: string[] = [];
  for (const change of changes) {
    if (change.op === 'write') zip[change.path] = [change.bytes, { level: 0 }];
    else removals.push(change.path);
  }
  const lines = [
    'Changes made in co-op to a read-only copy of an OP-1 drive.',
    '',
    'To apply them:',
    '1. Put the OP-1 in disk mode (shift + COM, then DISK) and open its drive.',
    '2. Copy these folders onto the OP-1 in disk mode, merging them with the folders already there',
    '   and replacing files when asked.',
    '3. Then delete the files listed below from the OP-1 drive.',
    '',
    'Files to delete:',
    ...(removals.length > 0 ? removals.map((path) => `  ${path}`) : ['  (nothing to delete)']),
    '',
  ];
  zip[CHANGES_FILE] = strToU8(lines.join('\r\n'));
  return zipSync(zip);
}
