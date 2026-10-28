import { useEffect, useRef, useState } from 'react';
import type { DeviceProfile } from '../core/profiles/types';
import { detectProfiles, getProfile, profiles, runDetectors } from '../core/profiles/registry';
import { loadAllPatches, loadLibrary, type LibraryEntry } from '../core/library/library';
import { StagedStore } from '../core/library/changeset';
import { recoverStranded } from '../core/store/atomic';
import { DropStore, fromInputFiles, type DroppedFile } from '../core/store/drop-store';
import {
  FsAccessStore,
  connectDevice,
  ensurePermission,
  isFsAccessSupported,
  rememberHandle,
  restoreHandle,
} from '../core/store/fs-access-store';
import { joinPath, type DeviceStore } from '../core/store/store';

/** What the library is showing. */
export interface DeviceSource {
  /** The store operations run against: the drive itself, or a staging layer over a dropped folder. */
  store: DeviceStore;
  /** Set for a read-only source; collects changes for download. */
  staged: StagedStore | null;
}

export interface Progress {
  done: number;
  total: number;
}

export interface Device {
  source: DeviceSource | null;
  profile: DeviceProfile;
  /** Why this profile was chosen. */
  detection: string;
  entries: LibraryEntry[];
  /** Folder names under each patch root, keyed by root path ('synth'). */
  folders: Record<string, string[]>;
  /** Patch reading progress while patches load. */
  progress: Progress | null;
  loading: boolean;
  error: string | null;
  /** Files put back by recovery of interrupted writes when the drive was opened. */
  recovered: string[];
  /** Entries a drop could not read. */
  skipped: string[];
  /** Staged changes waiting for download (read-only sources only). */
  changeCount: number;
  /** A folder remembered from an earlier visit, offered for reconnecting. */
  remembered: FileSystemDirectoryHandle | null;
  fsAccess: boolean;
  connect(): Promise<void>;
  reconnect(): Promise<void>;
  openDrop(files: DroppedFile[], skipped: string[]): Promise<void>;
  openInput(files: Iterable<File>): Promise<void>;
  setProfile(id: string): Promise<void>;
  /** Runs an operation against the source, then reloads. Errors are rethrown for the caller to show. */
  run(op: (store: DeviceStore) => Promise<unknown>): Promise<void>;
  refresh(): Promise<void>;
  disconnect(): void;
}

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

const defaultProfile = profiles[0] as DeviceProfile;

/** Folder names (no dot folders) under each patch root of the profile. */
async function listFolders(store: DeviceStore, profile: DeviceProfile): Promise<Record<string, string[]>> {
  const out: Record<string, string[]> = {};
  for (const root of profile.layout.patchRoots) {
    out[root.path] = (await store.list(root.path))
      .filter((e) => e.kind === 'dir' && !e.name.startsWith('.'))
      .map((e) => e.name);
  }
  return out;
}

/** Puts back files stranded by interrupted writes in every patch folder of a writable drive. */
async function recover(store: DeviceStore, profile: DeviceProfile): Promise<string[]> {
  if (!store.capabilities.write) return [];
  const restored: string[] = [];
  for (const [root, names] of Object.entries(await listFolders(store, profile))) {
    for (const name of names) restored.push(...(await recoverStranded(store, joinPath(root, name))));
  }
  return restored;
}

async function detect(store: DeviceStore): Promise<{ profile: DeviceProfile; detection: string }> {
  const [best] = await detectProfiles(store);
  if (best) return { profile: best.profile, detection: `detected: ${best.result.reason}` };
  const reasons = (await runDetectors(store)).map((r) => r.result.reason).join('; ');
  return { profile: defaultProfile, detection: `not recognised (${reasons}); assuming ${defaultProfile.label}` };
}

export function useDevice(): Device {
  const [source, setSource] = useState<DeviceSource | null>(null);
  const [profile, setProfileState] = useState<DeviceProfile>(defaultProfile);
  const [detection, setDetection] = useState('');
  const [entries, setEntries] = useState<LibraryEntry[]>([]);
  const [folders, setFolders] = useState<Record<string, string[]>>({});
  const [progress, setProgress] = useState<Progress | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [recovered, setRecovered] = useState<string[]>([]);
  const [skipped, setSkipped] = useState<string[]>([]);
  const [changeCount, setChangeCount] = useState(0);
  const [remembered, setRemembered] = useState<FileSystemDirectoryHandle | null>(null);
  const [fsAccess] = useState(isFsAccessSupported);
  /** Bumped by every load, so a slower earlier load never overwrites a later one. */
  const generation = useRef(0);

  useEffect(() => {
    if (!fsAccess) return;
    let live = true;
    void restoreHandle().then((handle) => {
      if (live) setRemembered(handle);
    });
    return () => {
      live = false;
    };
  }, [fsAccess]);

  async function load(next: DeviceSource, nextProfile: DeviceProfile): Promise<void> {
    const id = ++generation.current;
    const current = (): boolean => id === generation.current;
    setLoading(true);
    try {
      const listed = await loadLibrary(next.store, nextProfile);
      const nextFolders = await listFolders(next.store, nextProfile);
      if (!current()) return;
      setEntries(listed);
      setFolders(nextFolders);
      setChangeCount(next.staged?.changes().length ?? 0);
      setProgress({ done: 0, total: listed.length });
      const loaded = await loadAllPatches(next.store, nextProfile, listed, (done, total) => {
        if (current()) setProgress({ done, total });
      });
      if (current()) setEntries(loaded);
    } catch (e) {
      if (current()) setError(messageOf(e));
    } finally {
      if (current()) {
        setLoading(false);
        setProgress(null);
      }
    }
  }

  async function open(base: DeviceStore, droppedSkips: string[]): Promise<void> {
    setError(null);
    setRecovered([]);
    setSkipped(droppedSkips);
    setEntries([]);
    setLoading(true);
    try {
      const found = await detect(base);
      let restored: string[] = [];
      try {
        restored = await recover(base, found.profile);
      } catch (e) {
        setError(`could not finish recovering interrupted writes: ${messageOf(e)}`);
      }
      const next: DeviceSource = base.capabilities.write
        ? { store: base, staged: null }
        : (() => {
            const staged = new StagedStore(base);
            return { store: staged, staged };
          })();
      setProfileState(found.profile);
      setDetection(found.detection);
      setRecovered(restored);
      setSource(next);
      await load(next, found.profile);
    } catch (e) {
      setError(messageOf(e));
      setLoading(false);
    }
  }

  async function connect(): Promise<void> {
    try {
      const store = await connectDevice();
      if (!store) return;
      await rememberHandle(store.root);
      setRemembered(store.root);
      await open(store, []);
    } catch (e) {
      setError(messageOf(e));
    }
  }

  async function reconnect(): Promise<void> {
    if (!remembered) return;
    if (!(await ensurePermission(remembered))) {
      setError(`permission to open ${remembered.name} was not granted`);
      return;
    }
    await open(new FsAccessStore(remembered), []);
  }

  async function openDrop(files: DroppedFile[], dropSkipped: string[]): Promise<void> {
    try {
      if (files.length === 0) throw new Error('nothing readable was dropped; drop the OP-1 drive folder');
      await open(new DropStore(files), dropSkipped);
    } catch (e) {
      setSkipped(dropSkipped);
      setError(messageOf(e));
    }
  }

  async function openInput(files: Iterable<File>): Promise<void> {
    try {
      const list = [...files];
      if (list.length === 0) throw new Error('the chosen folder is empty');
      await open(fromInputFiles(list), []);
    } catch (e) {
      setError(messageOf(e));
    }
  }

  async function setProfile(id: string): Promise<void> {
    const next = getProfile(id);
    if (!next) return;
    setProfileState(next);
    setDetection(`chosen by hand`);
    if (source) await load(source, next);
  }

  async function refresh(): Promise<void> {
    if (source) await load(source, profile);
  }

  async function run(op: (store: DeviceStore) => Promise<unknown>): Promise<void> {
    if (!source) throw new Error('nothing is connected');
    try {
      await op(source.store);
    } finally {
      await load(source, profile);
    }
  }

  function disconnect(): void {
    generation.current++;
    setSource(null);
    setEntries([]);
    setFolders({});
    setProgress(null);
    setLoading(false);
    setError(null);
    setRecovered([]);
    setSkipped([]);
    setChangeCount(0);
  }

  return {
    source,
    profile,
    detection,
    entries,
    folders,
    progress,
    loading,
    error,
    recovered,
    skipped,
    changeCount,
    remembered,
    fsAccess,
    connect,
    reconnect,
    openDrop,
    openInput,
    setProfile,
    run,
    refresh,
    disconnect,
  };
}
