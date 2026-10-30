import { useCallback, useEffect, useRef, useState } from 'react';
import { scanLocalFolder, type LocalPatch } from '../core/library/local-library';
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

export interface LocalLibrary {
  /** False in browsers that cannot open a folder. */
  supported: boolean;
  /** The folder in use; null until one is open. */
  folder: FsAccessStore | null;
  /** A folder from an earlier visit that needs permission again. */
  remembered: FileSystemDirectoryHandle | null;
  patches: LocalPatch[];
  progress: { done: number; total: number } | null;
  error: string | null;
  choose(): Promise<void>;
  reconnect(): Promise<void>;
  refresh(): Promise<void>;
  forget(): Promise<void>;
}

/** The person's own patch folder. It needs no device, so every tab shares this one. */
export function useLocalLibrary(): LocalLibrary {
  const [supported] = useState(isFsAccessSupported);
  const [folder, setFolder] = useState<FsAccessStore | null>(null);
  const [remembered, setRemembered] = useState<FileSystemDirectoryHandle | null>(null);
  const [patches, setPatches] = useState<LocalPatch[]>([]);
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
      if (controller.signal.aborted) return;
      setPatches(found);
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
    setProgress(null);
    setError(null);
  }, []);

  return { supported, folder, remembered, patches, progress, error, choose, reconnect, refresh, forget };
}
