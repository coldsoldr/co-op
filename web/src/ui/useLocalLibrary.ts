import { useCallback, useEffect, useRef, useState } from 'react';
import { findFolders, readLocalPatch, scanLocalFolder, type LocalPatch } from '../core/library/local-library';
import type { DeviceStore } from '../core/store/store';
import {
  FsAccessStore,
  chooseLocalFolder,
  ensurePermission,
  forgetHandle,
  hasPermission,
  isFsAccessSupported,
  rememberHandle,
  restoreHandle,
} from '../core/store/fs-access-store';
import { messageOf } from '../core/util';

/** Where the chosen folder is remembered, apart from the OP-1 drive. */
const LOCAL_KEY = 'local';

/** A folder that may be written to, or why there is none. */
export type WritableFolder = { store: FsAccessStore } | { refused: 'cancelled' | 'denied' };

export interface LocalLibrary {
  /** False in browsers that cannot open a folder. */
  supported: boolean;
  /** The folder in use; null until one is open. */
  folder: FsAccessStore | null;
  /** A folder from an earlier visit that needs permission again. */
  remembered: FileSystemDirectoryHandle | null;
  patches: LocalPatch[];
  /** Every folder, including empty ones, relative to the chosen folder. */
  folders: string[];
  progress: { done: number; total: number } | null;
  error: string | null;
  choose(): Promise<void>;
  reconnect(): Promise<void>;
  refresh(): Promise<void>;
  forget(): Promise<void>;
  /** The folder with write permission, choosing or reconnecting it first. Needs a click. */
  writableFolder(): Promise<WritableFolder>;
  /** Brings these paths up to date: existing files are read again and missing ones dropped. */
  add(store: FsAccessStore, paths: readonly string[]): Promise<void>;
  /** Runs a change with write permission, then updates `touched` and the paths it returned. */
  apply(op: (store: DeviceStore) => Promise<unknown>, touched?: readonly string[]): Promise<void>;
}

/** The person's own patch folder. It needs no device, so every tab shares this one. */
export function useLocalLibrary(): LocalLibrary {
  const [supported] = useState(isFsAccessSupported);
  const [folder, setFolder] = useState<FsAccessStore | null>(null);
  const [remembered, setRemembered] = useState<FileSystemDirectoryHandle | null>(null);
  const [patches, setPatches] = useState<LocalPatch[]>([]);
  const [folders, setFolders] = useState<string[]>([]);
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const scan = useRef<AbortController | null>(null);

  const open = useCallback(async (store: FsAccessStore): Promise<void> => {
    scan.current?.abort();
    const controller = new AbortController();
    scan.current = controller;
    setFolder(store);
    setRemembered(null);
    setError(null);
    setProgress({ done: 0, total: 0 });
    try {
      const found = await scanLocalFolder(store, (done, total) => setProgress({ done, total }), controller.signal);
      const nested = await findFolders(store);
      if (controller.signal.aborted) return;
      setPatches(found);
      setFolders(nested);
    } catch (e) {
      if (!controller.signal.aborted) setError(`could not read ${store.label}: ${messageOf(e)}`);
    } finally {
      if (scan.current === controller) setProgress(null);
    }
  }, []);

  useEffect(() => {
    if (!supported) return;
    let live = true;
    void restoreHandle(LOCAL_KEY).then(async (handle) => {
      if (!live || !handle) return;
      if (await hasPermission(handle, 'read')) {
        if (live) await open(new FsAccessStore(handle));
      } else if (live) {
        setRemembered(handle);
      }
    });
    return () => {
      live = false;
    };
  }, [open, supported]);

  const choose = useCallback(async () => {
    setError(null);
    try {
      const store = await chooseLocalFolder();
      if (!store) return;
      await rememberHandle(store.root, LOCAL_KEY);
      await open(store);
    } catch (e) {
      setError(messageOf(e));
    }
  }, [open]);

  const reconnect = useCallback(async () => {
    if (!remembered) return;
    if (await ensurePermission(remembered, 'read')) await open(new FsAccessStore(remembered));
    else setError(`permission to open ${remembered.name} was not granted`);
  }, [open, remembered]);

  const refresh = useCallback(async () => {
    if (folder) await open(folder);
  }, [folder, open]);

  const forget = useCallback(async () => {
    scan.current?.abort();
    await forgetHandle(LOCAL_KEY);
    setFolder(null);
    setRemembered(null);
    setPatches([]);
    setFolders([]);
    setProgress(null);
    setError(null);
  }, []);

  const writableFolder = useCallback(async (): Promise<WritableFolder> => {
    setError(null);
    const handle = folder?.root ?? remembered;
    if (handle) {
      if (!(await ensurePermission(handle, 'readwrite'))) return { refused: 'denied' };
      if (folder) return { store: folder };
      const store = new FsAccessStore(handle);
      await open(store);
      return { store };
    }
    const store = await chooseLocalFolder();
    if (!store) return { refused: 'cancelled' };
    if (!(await ensurePermission(store.root, 'readwrite'))) return { refused: 'denied' };
    await rememberHandle(store.root, LOCAL_KEY);
    await open(store);
    return { store };
  }, [folder, open, remembered]);

  // Takes the store because writableFolder may have just opened it, after this render.
  const add = useCallback(async (store: FsAccessStore, paths: readonly string[]) => {
    const fresh = new Set(paths);
    const present = (await Promise.all([...fresh].map(async (path) => ((await store.exists(path)) ? [path] : [])))).flat();
    const read = await Promise.all(present.map((path) => readLocalPatch(store, path)));
    const nested = await findFolders(store);
    setPatches((prev) => [...prev.filter((p) => !fresh.has(p.path)), ...read].sort((a, b) => (a.path < b.path ? -1 : 1)));
    setFolders(nested);
  }, []);

  const apply = useCallback(
    async (op: (store: DeviceStore) => Promise<unknown>, touched: readonly string[] = []) => {
      const writable = await writableFolder();
      if ('refused' in writable) {
        if (writable.refused === 'cancelled') return;
        throw new Error('the browser was not allowed to write to the folder');
      }
      const result = await op(writable.store);
      const returned = Array.isArray(result) ? (result as string[]) : typeof result === 'string' ? [result] : [];
      await add(writable.store, [...touched, ...returned]);
    },
    [add, writableFolder],
  );

  return { supported, folder, remembered, patches, folders, progress, error, choose, reconnect, refresh, forget, writableFolder, add, apply };
}
