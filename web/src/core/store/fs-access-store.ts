import type { DirEntry } from './listing';
import { StoreError, basename, joinPath, splitPath, type DeviceStore, type StoreCapabilities } from './store';
import { byEntryName } from '../util';

/** Chromium's errors for a missing entry, or a file where a folder was expected (and vice versa). */
function isMissing(error: unknown): boolean {
  return error instanceof DOMException && (error.name === 'NotFoundError' || error.name === 'TypeMismatchError');
}

function storeError(action: string, path: string, error: unknown): StoreError {
  if (error instanceof StoreError) return error;
  const detail = error instanceof Error ? error.message : String(error);
  return new StoreError(`cannot ${action} ${path}: ${detail}`, { cause: error });
}

type CoOpNativeMove = (destination: FileSystemDirectoryHandle, name: string) => Promise<void>;

/**
 * FileSystemFileHandle.move (Chromium 110+) bound to the handle, or null where it is missing.
 * Reached through a cast rather than a lib.dom augmentation, like showDirectoryPicker below.
 */
function nativeMoveOf(handle: FileSystemFileHandle): CoOpNativeMove | null {
  const move = (handle as { move?: unknown }).move;
  return typeof move === 'function' ? (move.bind(handle) as CoOpNativeMove) : null;
}

/** A DeviceStore over a folder picked with the File System Access API (Chromium only). */
export class FsAccessStore implements DeviceStore {
  readonly capabilities: StoreCapabilities = { write: true };
  readonly label: string;

  /** `root` is public so the UI can remember the handle for reconnecting. */
  constructor(readonly root: FileSystemDirectoryHandle) {
    this.label = root.name;
  }

  async list(path: string): Promise<DirEntry[]> {
    try {
      const dir = await this.dir(splitPath(path), false);
      const entries: DirEntry[] = [];
      for await (const [name, handle] of dir.entries()) {
        entries.push({ name, kind: handle.kind === 'directory' ? 'dir' : 'file' });
      }
      return entries.sort(byEntryName);
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

  async readHead(path: string, n: number): Promise<Uint8Array> {
    try {
      const file = await (await this.fileHandle(path)).getFile();
      return new Uint8Array(await file.slice(0, Math.max(0, n)).arrayBuffer());
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
      const existed = await parent.getFileHandle(name).then(
        () => true,
        (error: unknown) => {
          if (isMissing(error)) return false;
          throw error;
        },
      );
      // Chromium creates an empty file here, before any bytes are written.
      const handle = await parent.getFileHandle(name, { create: true });
      const writable = await handle.createWritable();
      try {
        await writable.write(bytes.slice());
        await writable.close();
      } catch (error) {
        // Aborting discards the swap file, so an existing file keeps its old contents.
        await writable.abort().catch(() => {});
        if (!existed) await parent.removeEntry(name).catch(() => {});
        throw error;
      }
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
    const nativeMove = nativeMoveOf(handle);
    if (nativeMove) {
      try {
        const destination = await this.dir(splitPath(target).slice(0, -1), true);
        await nativeMove(destination, basename(target));
        return;
      } catch (error) {
        // A missing source is final; anything else (e.g. NotSupportedError for a move across
        // folders on some platforms) gets the copy-then-remove fallback below.
        if (error instanceof DOMException && error.name === 'NotFoundError') {
          throw storeError(`move ${joinPath(from)} to`, target, error);
        }
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

  async removeFolder(path: string): Promise<void> {
    try {
      const parts = splitPath(path);
      const name = parts.pop();
      if (name === undefined) throw new StoreError('cannot remove the drive root');
      const parent = await this.dir(parts, false);
      await parent.getDirectoryHandle(name); // rejects for a file
      // Without { recursive: true } this refuses a folder that still holds anything.
      await parent.removeEntry(name);
    } catch (error) {
      throw storeError('remove folder', joinPath(path), error);
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

interface CoOpDirectoryPickerOptions {
  id?: string;
  mode?: 'read' | 'readwrite';
  startIn?: string;
}

type CoOpShowDirectoryPicker = (options?: CoOpDirectoryPickerOptions) => Promise<FileSystemDirectoryHandle>;

/**
 * window.showDirectoryPicker where the browser has it (Chromium), else null. Reached through a cast
 * rather than a Window augmentation so a future lib.dom declaration cannot clash with ours.
 */
function directoryPicker(): CoOpShowDirectoryPicker | null {
  if (typeof window === 'undefined' || !('showDirectoryPicker' in window)) return null;
  const picker = (window as { showDirectoryPicker: unknown }).showDirectoryPicker;
  return typeof picker === 'function' ? (picker.bind(window) as CoOpShowDirectoryPicker) : null;
}

/** True in browsers that can open a folder for reading and writing (Chromium). */
export function isFsAccessSupported(): boolean {
  return directoryPicker() !== null;
}

/** Asks the user to pick the OP-1 drive. Resolves null when the picker is cancelled. */
export async function connectDevice(): Promise<FsAccessStore | null> {
  const showDirectoryPicker = directoryPicker();
  if (!showDirectoryPicker) throw new StoreError('this browser cannot open folders for writing');
  try {
    return new FsAccessStore(await showDirectoryPicker({ id: 'op1', mode: 'readwrite' }));
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

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(DB_STORE)) req.result.createObjectStore(DB_STORE);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
    // Another tab holds an older version open; give up rather than wait indefinitely.
    req.onblocked = () => reject(new Error('IndexedDB open blocked by another tab'));
  });
}

/** Saves the picked folder so the next visit can reconnect. Failures are ignored. */
export async function rememberHandle(handle: FileSystemDirectoryHandle): Promise<void> {
  try {
    const db = await openDb();
    try {
      // Resolve on transaction complete, not on the put's success, so the write is durable.
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(DB_STORE, 'readwrite');
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error ?? new Error('IndexedDB transaction aborted'));
        tx.objectStore(DB_STORE).put(handle, DB_KEY);
      });
    } finally {
      db.close();
    }
  } catch {
    // IndexedDB unavailable (private mode, blocked storage): reconnecting simply won't be offered.
  }
}

/** The folder saved by rememberHandle, or null when there is none or storage fails. */
export async function restoreHandle(): Promise<FileSystemDirectoryHandle | null> {
  try {
    const db = await openDb();
    let value: unknown;
    try {
      value = await request(db.transaction(DB_STORE, 'readonly').objectStore(DB_STORE).get(DB_KEY));
    } finally {
      db.close();
    }
    return value instanceof FileSystemDirectoryHandle ? value : null;
  } catch {
    return null;
  }
}
