import type { DirEntry } from './listing';
import { StoreError, basename, dirname, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from './store';

const byCodePoint = (a: DirEntry, b: DirEntry): number => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

/** A dropped or picked file and its path, starting with the name of the top-level item dropped. */
export interface DroppedFile {
  path: string;
  file: File;
}

export interface DropStoreOptions {
  /**
   * The source label. Defaults to the folder name for a single dropped folder, and to 'OP-1' when
   * several folders were dropped together.
   */
  label?: string;
}

/** Label for a drop of several top-level folders, taken to be the drive's own contents. */
const DRIVE_ROOT_LABEL = 'OP-1';

/**
 * A read-only DeviceStore over a folder dropped onto the page or picked with a `webkitdirectory`
 * input, for browsers without the File System Access API.
 *
 * Every path must start with a top-level folder. With one top-level folder (the drive itself),
 * that segment is stripped and becomes the label. With several (the drive's folders, such as
 * synth and drum, selected and dropped together), they are the drive root's contents and paths
 * are kept as they are. A loose top-level file is refused with a StoreError. Changes are refused;
 * wrap it in a staging store to collect them.
 */
export class DropStore implements DeviceStore {
  readonly capabilities: StoreCapabilities = { write: false };
  readonly label: string;
  private readonly files = new Map<string, File>();
  /** Every folder path except the root ''. */
  private readonly dirs = new Set<string>();

  constructor(entries: DroppedFile[], options: DropStoreOptions = {}) {
    const tops = new Set<string>();
    for (const { path } of entries) {
      const parts = splitPath(path);
      if (parts.length < 2) {
        throw new StoreError(`${path} is not inside a folder; drop the OP-1 drive folder or its folders`);
      }
      tops.add(parts[0] as string);
    }
    const single = tops.size === 1 ? [...tops][0] : undefined;
    for (const { path, file } of entries) {
      const parts = splitPath(path);
      const rest = single === undefined ? parts : parts.slice(1);
      this.files.set(rest.join('/'), file);
      for (let i = 1; i < rest.length; i++) this.dirs.add(rest.slice(0, i).join('/'));
    }
    this.label = options.label ?? single ?? (tops.size > 1 ? DRIVE_ROOT_LABEL : '');
  }

  async list(path: string): Promise<DirEntry[]> {
    const dir = joinPath(path);
    if (!this.isDir(dir)) return [];
    const entries: DirEntry[] = [];
    for (const file of this.files.keys()) if (dirname(file) === dir) entries.push({ name: basename(file), kind: 'file' });
    for (const folder of this.dirs) if (dirname(folder) === dir) entries.push({ name: basename(folder), kind: 'dir' });
    return entries.sort(byCodePoint);
  }

  async read(path: string): Promise<Uint8Array> {
    const file = this.file(path);
    try {
      return new Uint8Array(await file.arrayBuffer());
    } catch (error) {
      throw new StoreError(`cannot read ${joinPath(path)}: ${(error as Error).message}`, { cause: error });
    }
  }

  async size(path: string): Promise<number> {
    return this.file(path).size;
  }

  async exists(path: string): Promise<boolean> {
    const key = joinPath(path);
    return this.files.has(key) || this.isDir(key);
  }

  async write(path: string, _bytes: Uint8Array): Promise<void> {
    throw readOnly(path);
  }

  async move(from: string, _to: string): Promise<void> {
    throw readOnly(from);
  }

  async remove(path: string): Promise<void> {
    throw readOnly(path);
  }

  async mkdir(path: string): Promise<void> {
    throw readOnly(path);
  }

  private isDir(key: string): boolean {
    return key === '' || this.dirs.has(key);
  }

  private file(path: string): File {
    const key = joinPath(path);
    const file = this.files.get(key);
    if (!file) throw new StoreError(this.isDir(key) ? `${key} is a folder, not a file` : `no such file: ${key}`);
    return file;
  }
}

function readOnly(path: string): StoreError {
  return new StoreError(`read-only source: cannot change ${joinPath(path)}`);
}

/** A DropStore from the files of an `<input type="file" webkitdirectory>`. */
export function fromInputFiles(files: Iterable<File>, options: DropStoreOptions = {}): DropStore {
  return new DropStore(
    [...files].map((file) => ({ path: file.webkitRelativePath, file })),
    options,
  );
}

/** What a drag-and-drop yielded. */
export interface DroppedFiles {
  /** Every readable file, ready for `new DropStore(files)`. */
  files: DroppedFile[];
  /** Paths of files that could not be read, and of folders that could not be (fully) listed. */
  skipped: string[];
}

function fileOf(entry: FileSystemFileEntry): Promise<File> {
  return new Promise((resolve, reject) => entry.file(resolve, reject));
}

/**
 * Reads the children of a directory entry; readEntries returns them in batches until an empty one.
 * If a batch fails, the children read so far are returned with `complete: false`.
 */
async function childrenOf(entry: FileSystemDirectoryEntry): Promise<{ children: FileSystemEntry[]; complete: boolean }> {
  const reader = entry.createReader();
  const children: FileSystemEntry[] = [];
  for (;;) {
    let batch: FileSystemEntry[];
    try {
      batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
    } catch {
      return { children, complete: false };
    }
    if (batch.length === 0) return { children, complete: true };
    children.push(...batch);
  }
}

async function walk(entry: FileSystemEntry, out: DroppedFiles): Promise<void> {
  const path = joinPath(entry.fullPath);
  if (entry.isFile) {
    try {
      out.files.push({ path, file: await fileOf(entry as FileSystemFileEntry) });
    } catch {
      out.skipped.push(path);
    }
  } else if (entry.isDirectory) {
    const { children, complete } = await childrenOf(entry as FileSystemDirectoryEntry);
    if (!complete) out.skipped.push(path);
    for (const child of children) await walk(child, out);
  }
}

/**
 * Every file under the folders (and files) dropped in a drag-and-drop, with paths starting at the
 * dropped item's name. Files that cannot be read and folders that cannot be listed are skipped
 * and reported in `skipped` rather than failing the whole drop. Entries are taken synchronously,
 * since the DataTransfer is emptied once the drop event returns.
 */
export async function filesFromDataTransfer(dt: DataTransfer): Promise<DroppedFiles> {
  const entries: FileSystemEntry[] = [];
  for (const item of Array.from(dt.items)) {
    if (item.kind !== 'file') continue;
    const entry = item.webkitGetAsEntry();
    if (entry) entries.push(entry);
  }
  const out: DroppedFiles = { files: [], skipped: [] };
  for (const entry of entries) await walk(entry, out);
  return out;
}
