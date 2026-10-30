import { strToU8, zipSync, type Zippable } from 'fflate';
import type { DirEntry } from '../store/listing';
import { StoreError, basename, dirname, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from '../store/store';
import { byCodePoint, bytesEqual } from '../util';

/**
 * One change against the base store, in its final state. 'mkdir' is a new folder left empty;
 * 'rmdir' is a base folder removed once its files are gone.
 * 'rename' is a case-only rename: the drive file at `path` must be renamed by hand to `to` (a
 * file name in the same folder), because a FAT drive keeps the old spelling when the file is
 * replaced by a copy whose name differs only in case. Its bytes travel as the matching 'write'.
 */
export type Change =
  | { op: 'write'; path: string; bytes: Uint8Array }
  | { op: 'remove'; path: string }
  | { op: 'mkdir'; path: string }
  | { op: 'rmdir'; path: string }
  | { op: 'rename'; path: string; to: string };

/** The instructions file inside an exported change set. */
export const CHANGES_FILE = 'CHANGES.txt';

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
  /** Folders made explicitly with mkdir that the base does not have. */
  private readonly made = new Set<string>();
  /** Base folders that are currently removed. */
  private readonly goneDirs = new Set<string>();

  constructor(private readonly base: DeviceStore) {
    this.label = base.label;
  }

  async list(path: string): Promise<DirEntry[]> {
    const dir = joinPath(path);
    if (!(await this.isDir(dir))) return [];
    const entries = new Map<string, DirEntry['kind']>();
    for (const entry of await this.base.list(dir)) {
      const path = joinPath(dir, entry.name);
      if (entry.kind === 'file' ? this.removed.has(path) : this.goneDirs.has(path)) continue;
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

  async readHead(path: string, n: number): Promise<Uint8Array> {
    const key = joinPath(path);
    const bytes = this.staged.get(key);
    if (bytes) return bytes.slice(0, Math.max(0, n));
    if (this.removed.has(key)) throw new StoreError(`no such file: ${key}`);
    if (this.base.readHead) return this.base.readHead(key, n);
    return (await this.base.read(key)).slice(0, Math.max(0, n));
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
    if (!(await this.base.exists(key))) this.made.add(key);
  }

  async removeFolder(path: string): Promise<void> {
    const key = joinPath(path);
    if (key === '' || !(await this.isDir(key))) throw new StoreError(`no such folder: ${key}`);
    if ((await this.list(key)).length > 0) throw new StoreError(`${key} is not empty`);
    this.dirs.delete(key);
    this.made.delete(key);
    if (await this.base.exists(key)) this.goneDirs.add(key);
  }

  /**
   * The net changes against the base, sorted by path. Written bytes are the staged buffers, not
   * copies. A removal whose path matches a written path ignoring case is dropped: on the OP-1's
   * FAT drive the two names are one file, so deleting it would lose the patch (a case-only
   * rename); a 'rename' change to the written spelling takes its place when the file names
   * differ, so the user can fix the spelling by hand. A folder made with mkdir is listed only while
   * nothing staged lies beneath it.
   */
  changes(): Change[] {
    const out: Change[] = [];
    const written = new Map<string, string>();
    for (const [path, bytes] of this.staged) {
      out.push({ op: 'write', path, bytes });
      written.set(path.toLowerCase(), path);
    }
    for (const path of this.removed) {
      const replacement = written.get(path.toLowerCase());
      if (replacement === undefined) out.push({ op: 'remove', path });
      else if (basename(replacement) !== basename(path)) out.push({ op: 'rename', path, to: basename(replacement) });
    }
    const below = (dir: string, path: string): boolean => path.startsWith(`${dir}/`);
    for (const dir of this.made) {
      const occupied = [...this.staged.keys(), ...this.made].some((path) => below(dir, path));
      if (!occupied) out.push({ op: 'mkdir', path: dir });
    }
    for (const dir of this.goneDirs) out.push({ op: 'rmdir', path: dir });
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
    if (this.goneDirs.has(key)) return false;
    return !(await this.baseHasFile(key)) && (await this.base.exists(key));
  }

  /** Registers every ancestor folder of a file path; throws if one of them is a file. */
  private async addParents(filePath: string): Promise<void> {
    const folders = ancestors(filePath);
    for (const folder of folders) {
      if (await this.isFile(folder)) throw new StoreError(`cannot create folder ${folder}: a file has that name`);
    }
    for (const folder of folders) {
      this.dirs.add(folder);
      this.goneDirs.delete(folder);
    }
  }
}

/**
 * How many files a change set touches, as a user counts them: distinct paths ignoring case, so a
 * case-only rename (a write of the new spelling plus a rename of the old) counts once.
 */
export function countChanges(changes: readonly Change[]): number {
  return new Set(changes.map((c) => c.path.toLowerCase())).size;
}

/**
 * Packs a change set as a zip for the user to apply by hand: every written file at its drive path
 * (stored uncompressed; audio barely compresses), a directory entry for each new empty folder, and
 * CHANGES.txt with the steps. Deletions come first so that copying never precedes a delete of the
 * same name, and copying is described per destination folder, because copying a whole top-level
 * folder onto the drive can replace the folder there (macOS Finder's "Replace"). Every entry is
 * stamped with `now()`, so the zip is deterministic for a given time. Case-only renames get a final
 * step listing the files to rename by hand.
 */
export function exportChangeSet(changes: readonly Change[], now: () => Date = () => new Date()): Uint8Array {
  const mtime = now();
  const zip: Zippable = {};
  const removals: string[] = [];
  const folders: string[] = [];
  const emptied: string[] = [];
  const renames: string[] = [];
  const copies = new Map<string, string[]>();
  for (const change of changes) {
    if (change.op === 'write') {
      zip[change.path] = [change.bytes, { level: 0, mtime }];
      const dir = dirname(change.path);
      copies.set(dir, [...(copies.get(dir) ?? []), basename(change.path)]);
    } else if (change.op === 'remove') {
      removals.push(change.path);
    } else if (change.op === 'rename') {
      renames.push(`${change.path} -> ${change.to}`);
    } else if (change.op === 'rmdir') {
      emptied.push(change.path);
    } else {
      zip[`${change.path}/`] = [new Uint8Array(0), { mtime }];
      folders.push(change.path);
    }
  }
  const none = (text: string): string[] => [`   (${text})`];
  const lines = [
    'Changes made in co-op to a read-only copy of an OP-1 drive.',
    '',
    'Put the OP-1 in disk mode (shift + COM, then DISK), open its drive, and apply these steps in order.',
    '',
    '1. Delete these files from the OP-1 drive:',
    ...(removals.length > 0 ? removals.map((path) => `   ${path}`) : none('nothing to delete')),
    ...(emptied.length > 0 ? ['   Then delete these folders, now empty:', ...emptied.map((path) => `   ${path}/`)] : []),
    '',
    '2. Create these folders on the OP-1 drive:',
    ...(folders.length > 0 ? folders.map((path) => `   ${path}/`) : none('no new folders')),
    '',
    '3. Copy the files from this zip into these folders on the OP-1 drive, creating a folder if it is missing:',
    ...(copies.size > 0
      ? [...copies.keys()].sort(byCodePoint).flatMap((dir) => [
          `   ${dir}/`,
          ...(copies.get(dir) as string[]).sort(byCodePoint).map((name) => `     ${name}`),
        ])
      : none('nothing to copy')),
    '',
    ...(renames.length > 0
      ? [
          '4. Rename these files on the OP-1 drive by hand (the drive keeps the old spelling when a file is replaced):',
          ...renames.map((line) => `   ${line}`),
          '',
        ]
      : []),
    'Copy the files themselves into each folder; never copy a folder from this zip over a folder on the drive.',
    'On a Mac, never choose Replace for a folder; copy the files themselves, or hold Option and choose Merge.',
    '',
  ];
  zip[CHANGES_FILE] = [strToU8(lines.join('\r\n')), { mtime }];
  return zipSync(zip);
}
