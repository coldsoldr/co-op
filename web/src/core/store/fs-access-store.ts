import type { DirEntry } from './listing';
import { StoreError, basename, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from './store';

const byCodePoint = (a: DirEntry, b: DirEntry): number => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

/** Chromium's errors for a missing entry, or a file where a folder was expected (and vice versa). */
function isMissing(error: unknown): boolean {
  return error instanceof DOMException && (error.name === 'NotFoundError' || error.name === 'TypeMismatchError');
}

function storeError(action: string, path: string, error: unknown): StoreError {
  if (error instanceof StoreError) return error;
  const detail = error instanceof Error ? error.message : String(error);
  return new StoreError(`cannot ${action} ${path}: ${detail}`, { cause: error });
}

/** A DeviceStore over a folder picked with the File System Access API (Chromium only). */
export class FsAccessStore implements DeviceStore {
  readonly capabilities: StoreCapabilities = { write: true };
  readonly label: string;

  constructor(private readonly root: FileSystemDirectoryHandle) {
    this.label = root.name;
  }

  async list(path: string): Promise<DirEntry[]> {
    try {
      const dir = await this.dir(splitPath(path), false);
      const entries: DirEntry[] = [];
      for await (const [name, handle] of dir.entries()) {
        entries.push({ name, kind: handle.kind === 'directory' ? 'dir' : 'file' });
      }
      return entries.sort(byCodePoint);
    } catch (error) {
      if (isMissing(error)) return [];
      throw storeError('list', joinPath(path), error);
    }
  }

  async read(path: string): Promise<Uint8Array> {
    try {
      const file = await (await this.fileHandle(path)).getFile();
      return new Uint8Array(await file.arrayBuffer());
    } catch (error) {
      throw storeError('read', joinPath(path), error);
    }
  }

  async size(path: string): Promise<number> {
    try {
      return (await (await this.fileHandle(path)).getFile()).size;
    } catch (error) {
      throw storeError('get the size of', joinPath(path), error);
    }
  }

  async exists(path: string): Promise<boolean> {
    const parts = splitPath(path);
    const name = parts.pop();
    if (name === undefined) return true;
    try {
      const parent = await this.dir(parts, false);
      await parent.getFileHandle(name);
      return true;
    } catch (error) {
      // A TypeMismatchError from getFileHandle itself means the name is a folder.
      if (error instanceof DOMException && error.name === 'TypeMismatchError') {
        try {
          await (await this.dir(parts, false)).getDirectoryHandle(name);
          return true;
        } catch (inner) {
          if (isMissing(inner)) return false;
          throw storeError('check', joinPath(path), inner);
        }
      }
      if (isMissing(error)) return false;
      throw storeError('check', joinPath(path), error);
    }
  }

  async write(path: string, bytes: Uint8Array): Promise<void> {
    try {
      const parts = splitPath(path);
      const name = parts.pop();
      if (name === undefined) throw new StoreError('cannot write the drive root');
      const parent = await this.dir(parts, true);
      const handle = await parent.getFileHandle(name, { create: true });
      const writable = await handle.createWritable();
      await writable.write(bytes.slice());
      await writable.close();
    } catch (error) {
      throw storeError('write', joinPath(path), error);
    }
  }

  async move(from: string, to: string): Promise<void> {
    const target = joinPath(to);
    if (await this.exists(target)) throw new StoreError(`cannot move ${joinPath(from)} to ${target}: target exists`);
    let handle: FileSystemFileHandle;
    try {
      handle = await this.fileHandle(from);
    } catch (error) {
      throw storeError('move', joinPath(from), error);
    }
    if (typeof handle.move === 'function') {
      try {
        const destination = await this.dir(splitPath(target).slice(0, -1), true);
        await handle.move(destination, basename(target));
        return;
      } catch (error) {
        throw storeError(`move ${joinPath(from)} to`, target, error);
      }
    }
    await this.write(target, await this.read(from));
    await this.remove(from);
  }

  async remove(path: string): Promise<void> {
    try {
      const parts = splitPath(path);
      const name = parts.pop();
      if (name === undefined) throw new StoreError('cannot remove the drive root');
      const parent = await this.dir(parts, false);
      await parent.getFileHandle(name); // rejects for a folder, so folders are never removed
      await parent.removeEntry(name);
    } catch (error) {
      throw storeError('remove', joinPath(path), error);
    }
  }

  async mkdir(path: string): Promise<void> {
    try {
      await this.dir(splitPath(path), true);
    } catch (error) {
      throw storeError('create folder', joinPath(path), error);
    }
  }

  private async dir(parts: string[], create: boolean): Promise<FileSystemDirectoryHandle> {
    let dir = this.root;
    for (const part of parts) dir = await dir.getDirectoryHandle(part, { create });
    return dir;
  }

  private async fileHandle(path: string): Promise<FileSystemFileHandle> {
    const parts = splitPath(path);
    const name = parts.pop();
    if (name === undefined) throw new StoreError('the drive root is not a file');
    return (await this.dir(parts, false)).getFileHandle(name);
  }
}

/** True in browsers that can open a folder for reading and writing (Chromium). */
export function isFsAccessSupported(): boolean {
  return typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';
}

/** Asks the user to pick the OP-1 drive. Resolves null when the picker is cancelled. */
export async function connectDevice(): Promise<FsAccessStore | null> {
  if (!isFsAccessSupported() || !window.showDirectoryPicker) {
    throw new StoreError('this browser cannot open folders for writing');
  }
  try {
    return new FsAccessStore(await window.showDirectoryPicker({ id: 'op1', mode: 'readwrite' }));
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') return null;
    throw error;
  }
}

/** True when the handle may be read and written, asking the user if needed. Needs a user gesture to ask. */
export async function ensurePermission(handle: FileSystemDirectoryHandle): Promise<boolean> {
  try {
    if ((await handle.queryPermission({ mode: 'readwrite' })) === 'granted') return true;
    return (await handle.requestPermission({ mode: 'readwrite' })) === 'granted';
  } catch {
    return false;
  }
}

const DB_NAME = 'co-op';
const DB_STORE = 'handles';
const DB_KEY = 'device';

function request<T>(req: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function openDb(): Promise<IDBDatabase> {
  const req = indexedDB.open(DB_NAME, 1);
  req.onupgradeneeded = () => req.result.createObjectStore(DB_STORE);
  return request(req);
}

async function withHandles<T>(mode: IDBTransactionMode, run: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await openDb();
  try {
    return await request(run(db.transaction(DB_STORE, mode).objectStore(DB_STORE)));
  } finally {
    db.close();
  }
}

/** Saves the picked folder so the next visit can reconnect. Failures are ignored. */
export async function rememberHandle(handle: FileSystemDirectoryHandle): Promise<void> {
  try {
    await withHandles('readwrite', (store) => store.put(handle, DB_KEY));
  } catch {
    // IndexedDB unavailable (private mode, blocked storage): reconnecting simply won't be offered.
  }
}

/** The folder saved by rememberHandle, or null when there is none or storage fails. */
export async function restoreHandle(): Promise<FileSystemDirectoryHandle | null> {
  try {
    const value: unknown = await withHandles('readonly', (store) => store.get(DB_KEY));
    return value instanceof FileSystemDirectoryHandle ? value : null;
  } catch {
    return null;
  }
}
