import type { DirEntry } from './listing';
import { StoreError, basename, dirname, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from './store';

const byCodePoint = (a: DirEntry, b: DirEntry): number => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

/** A dropped or picked file and its path, starting with the dropped folder's name. */
export interface DroppedFile {
  path: string;
  file: File;
}

/**
 * A read-only DeviceStore over a folder dropped onto the page or picked with a `webkitdirectory`
 * input, for browsers without the File System Access API. Every path must start with the same
 * folder name; that segment is stripped and becomes the label. Changes are refused; wrap it in a
 * staging store to collect them.
 */
export class DropStore implements DeviceStore {
  readonly capabilities: StoreCapabilities = { write: false };
  readonly label: string;
  private readonly files = new Map<string, File>();
  /** Every folder path except the root ''. */
  private readonly dirs = new Set<string>();

  constructor(entries: DroppedFile[]) {
    let label: string | undefined;
    for (const { path, file } of entries) {
      const [top, ...rest] = splitPath(path);
      if (top === undefined || rest.length === 0) {
        throw new StoreError(`${path} is not inside a folder; drop the OP-1 drive folder itself`);
      }
      if (label !== undefined && top !== label) {
        throw new StoreError(`files come from more than one folder (${label}, ${top}); drop a single folder`);
      }
      label = top;
      const key = rest.join('/');
      this.files.set(key, file);
      for (let i = 1; i < rest.length; i++) this.dirs.add(rest.slice(0, i).join('/'));
    }
    this.label = label ?? '';
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
export function fromInputFiles(files: Iterable<File>): DropStore {
  return new DropStore([...files].map((file) => ({ path: file.webkitRelativePath, file })));
}

function fileOf(entry: FileSystemFileEntry): Promise<File> {
  return new Promise((resolve, reject) => entry.file(resolve, reject));
}

/** Reads every child of a directory entry; readEntries returns them in batches until an empty one. */
async function childrenOf(entry: FileSystemDirectoryEntry): Promise<FileSystemEntry[]> {
  const reader = entry.createReader();
  const children: FileSystemEntry[] = [];
  for (;;) {
    const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => reader.readEntries(resolve, reject));
    if (batch.length === 0) return children;
    children.push(...batch);
  }
}

async function walk(entry: FileSystemEntry, out: DroppedFile[]): Promise<void> {
  if (entry.isFile) {
    out.push({ path: joinPath(entry.fullPath), file: await fileOf(entry as FileSystemFileEntry) });
  } else if (entry.isDirectory) {
    for (const child of await childrenOf(entry as FileSystemDirectoryEntry)) await walk(child, out);
  }
}

/**
 * Every file under the folders (and files) dropped in a drag-and-drop, with paths starting at the
 * dropped item's name. Entries are taken synchronously, since the DataTransfer is emptied once the
 * drop event returns.
 */
export async function filesFromDataTransfer(dt: DataTransfer): Promise<DroppedFile[]> {
  const entries: FileSystemEntry[] = [];
  for (const item of Array.from(dt.items)) {
    if (item.kind !== 'file') continue;
    const entry = item.webkitGetAsEntry();
    if (entry) entries.push(entry);
  }
  const out: DroppedFile[] = [];
  for (const entry of entries) await walk(entry, out);
  return out;
}
