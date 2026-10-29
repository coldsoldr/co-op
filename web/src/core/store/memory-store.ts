import type { DirEntry } from './listing';
import { StoreError, joinPath, dirname, basename, splitPath, type DeviceStore, type StoreCapabilities } from './store';
import { byEntryName } from '../util';

export interface MemoryStoreOptions {
  /** Defaults to true. */
  write?: boolean;
  /** Defaults to 'memory'. */
  label?: string;
}

/**
 * A DeviceStore held in memory. Folders are implied by file paths and by mkdir, and stay after
 * their last file is removed, as on a real drive. Bytes are copied in and out so callers can never
 * alias the stored data.
 */
export class MemoryStore implements DeviceStore {
  readonly capabilities: StoreCapabilities;
  readonly label: string;
  private readonly files = new Map<string, Uint8Array>();
  /** Every folder path except the root ''. */
  private readonly dirs = new Set<string>();

  constructor(files: Record<string, Uint8Array>, options: MemoryStoreOptions = {}) {
    this.capabilities = { write: options.write ?? true };
    this.label = options.label ?? 'memory';
    for (const [path, bytes] of Object.entries(files)) {
      const key = joinPath(path);
      if (this.isDir(key)) throw new StoreError(`${key} is both a file and a folder`);
      this.addParents(key);
      this.files.set(key, bytes.slice());
    }
  }

  async list(path: string): Promise<DirEntry[]> {
    const dir = joinPath(path);
    if (!this.isDir(dir)) return [];
    const entries: DirEntry[] = [];
    for (const file of this.files.keys()) if (dirname(file) === dir) entries.push({ name: basename(file), kind: 'file' });
    for (const folder of this.dirs) if (dirname(folder) === dir) entries.push({ name: basename(folder), kind: 'dir' });
    return entries.sort(byEntryName);
  }

  async read(path: string): Promise<Uint8Array> {
    return this.file(path).slice();
  }

  async size(path: string): Promise<number> {
    return this.file(path).length;
  }

  async exists(path: string): Promise<boolean> {
    const key = joinPath(path);
    return this.files.has(key) || this.isDir(key);
  }

  async write(path: string, bytes: Uint8Array): Promise<void> {
    const key = joinPath(path);
    this.assertWritable(key);
    if (this.isDir(key)) throw new StoreError(`cannot write ${key}: it is a folder`);
    this.addParents(key);
    this.files.set(key, bytes.slice());
  }

  async move(from: string, to: string): Promise<void> {
    const source = joinPath(from);
    const target = joinPath(to);
    this.assertWritable(source);
    const bytes = this.file(source);
    // Checked synchronously with the set below, so two moves in flight cannot both take one target.
    if (this.files.has(target) || this.isDir(target)) {
      throw new StoreError(`cannot move ${source} to ${target}: target exists`);
    }
    this.addParents(target);
    this.files.set(target, bytes);
    this.files.delete(source);
  }

  async remove(path: string): Promise<void> {
    const key = joinPath(path);
    this.assertWritable(key);
    this.file(key);
    this.files.delete(key);
  }

  async mkdir(path: string): Promise<void> {
    const key = joinPath(path);
    this.assertWritable(key);
    if (key === '') return;
    this.addParents(joinPath(key, 'x'));
  }

  /** A copy of every file, keyed by path in code-point order. */
  snapshot(): Record<string, Uint8Array> {
    const out: Record<string, Uint8Array> = {};
    for (const path of [...this.files.keys()].sort()) out[path] = (this.files.get(path) as Uint8Array).slice();
    return out;
  }

  private isDir(key: string): boolean {
    return key === '' || this.dirs.has(key);
  }

  private file(path: string): Uint8Array {
    const key = joinPath(path);
    const bytes = this.files.get(key);
    if (!bytes) throw new StoreError(this.isDir(key) ? `${key} is a folder, not a file` : `no such file: ${key}`);
    return bytes;
  }

  private assertWritable(key: string): void {
    if (!this.capabilities.write) throw new StoreError(`read-only source: cannot change ${key}`);
  }

  /** Registers every ancestor folder of a file path; throws if one of them is a file. */
  private addParents(filePath: string): void {
    const parts = splitPath(filePath).slice(0, -1);
    for (let i = 1; i <= parts.length; i++) {
      const folder = parts.slice(0, i).join('/');
      if (this.files.has(folder)) throw new StoreError(`cannot create folder ${folder}: a file has that name`);
    }
    for (let i = 1; i <= parts.length; i++) this.dirs.add(parts.slice(0, i).join('/'));
  }
}
