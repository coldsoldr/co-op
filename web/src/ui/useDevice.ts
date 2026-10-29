import { useCallback, useEffect, useRef, useState } from 'react';
import type { DeviceProfile } from '../core/profiles/types';
import {
  detectProfiles,
  getProfile,
  profiles,
  runDetectors,
  suggestProfile,
  type RankedProfile,
} from '../core/profiles/registry';
import type { SoundTraits } from '../core/codec/sound-traits';
import type { Inventory } from '../core/usbmidi/inventory';
import { DEFAULT_CONCURRENCY, loadAllPatches, loadLibrary } from '../core/library/library';
import { StagedStore, countChanges } from '../core/library/changeset';
import { recoverDrive } from '../core/library/recover';
import { DropStore, fromInputFiles, type DroppedFile } from '../core/store/drop-store';
import {
  FsAccessStore,
  connectDevice,
  ensurePermission,
  isFsAccessSupported,
  rememberHandle,
  restoreHandle,
} from '../core/store/fs-access-store';
import type { DirectoryListing } from '../core/store/listing';
import type { DeviceStore } from '../core/store/store';
import { createQueue } from './queue';
import { OVERRIDE_CONFIDENCE, loadRememberedProfile, rememberProfile } from './remembered-profile';
import { mergeRows, toRow, type LibraryRow } from './rows';
import { messageOf } from '../core/util';

/** What the library is showing. */
export interface DeviceSource {
  /** The store operations run against: the drive itself, or a staging layer over a dropped folder. */
  store: DeviceStore;
  /** Set for a read-only source; collects changes for download. */
  staged: StagedStore | null;
  /** Distinct per opened source, for resetting per-source UI state. */
  key: number;
}

export interface Progress {
  done: number;
  total: number;
}

/**
 * An operation against the open source. `source` is the source it runs on, so an operation queued
 * earlier can check that the source is still the one it was meant for (compare `source.key`).
 */
export type Operation = (store: DeviceStore, source: DeviceSource) => Promise<unknown>;

export interface Device {
  source: DeviceSource | null;
  profile: DeviceProfile;
  /** Why this profile was chosen. */
  detection: string;
  /**
   * A profile whose content sniff beats the active profile's folder detection, once every patch is
   * read; null when there is none, after a dismissal, or when the profile was chosen by hand.
   */
  suggestion: RankedProfile | null;
  /** Hides the suggestion for the open source. */
  dismissSuggestion(): void;
  /** The active profile right now, for async work that must check it has not changed. */
  currentProfile(): DeviceProfile;
  /** Bumped each time a load of the library settles; read-only views re-read the drive then. */
  settledCount: number;
  rows: LibraryRow[];
  /** Folder names under each patch root, keyed by root path ('synth'). */
  folders: Record<string, string[]>;
  /** Patch reading progress while patches load. */
  progress: Progress | null;
  /** True while the drive is being opened or listed. */
  loading: boolean;
  /** True while any queued action (open, operation, backup, restore) is waiting or running. */
  busy: boolean;
  error: string | null;
  /** Files put back by recovery of interrupted writes when the drive was opened. */
  recovered: string[];
  /** Why recovering interrupted writes failed when the drive was opened, if it did. */
  recoveryError: string | null;
  /** Entries a drop could not read. */
  skipped: string[];
  /** Staged changes waiting for download, counted by distinct file (read-only sources only). */
  changeCount: number;
  /**
   * True when there are staged changes and an operation has changed them since the last download
   * (or they were never downloaded); leaving or disconnecting then asks first.
   */
  unsavedChanges: boolean;
  /** Records that the current staged changes were downloaded. */
  markDownloaded(): void;
  /** A folder remembered from an earlier visit, offered for reconnecting. */
  remembered: FileSystemDirectoryHandle | null;
  fsAccess: boolean;
  connect(): Promise<void>;
  reconnect(): Promise<void>;
  openDrop(files: DroppedFile[], skipped: string[]): Promise<void>;
  openInput(files: Iterable<File>): Promise<void>;
  /** Shows an error outside the queue, e.g. when reading a dropped folder fails. */
  fail(message: string): void;
  setProfile(id: string): Promise<void>;
  /**
   * What the last USB check read from the OP-1 (its plugin inventory), or null before a check, after a
   * check that found none, or on a build without the inventory. Kept in memory only.
   */
  installedPlugins: Inventory | null;
  setInstalledPlugins(inventory: Inventory | null): void;
  /**
   * Queues an operation, runs it against the source, then reloads, re-reading `touched` paths and
   * any whose size changed. Errors are rethrown for the caller to show.
   */
  run(op: Operation, touched?: readonly string[]): Promise<void>;
  /** Queues a task that must not overlap an operation (a backup, a restore diff). */
  exclusive<T>(task: () => Promise<T>): Promise<T>;
  refresh(): Promise<void>;
  disconnect(): void;
}

const isAbort = (error: unknown): boolean => error instanceof DOMException && error.name === 'AbortError';

const defaultProfile = profiles[0] as DeviceProfile;

/** How often progress is pushed to React while patches load. */
const PROGRESS_INTERVAL_MS = 100;

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

/**
 * Picks the profile for a newly opened drive. The profile remembered from last time wins, unless
 * folder detection proves a different one (OVERRIDE_CONFIDENCE). `confidence` is the picked
 * profile's own folder-detection confidence, which the content sniff must beat to suggest another.
 */
export async function detect(
  root: DirectoryListing,
  rememberedId: string | null,
): Promise<{ profile: DeviceProfile; detection: string; confidence: number }> {
  const ranked = await detectProfiles(root);
  const [best] = ranked;
  const remembered = rememberedId === null ? undefined : getProfile(rememberedId);
  const proven = best !== undefined && best.profile.id !== remembered?.id && best.result.confidence >= OVERRIDE_CONFIDENCE;
  if (remembered && !proven) {
    const own = ranked.find((r) => r.profile.id === remembered.id)?.result.confidence ?? 0;
    return { profile: remembered, detection: 'remembered from last time', confidence: own };
  }
  if (best) {
    const note = remembered ? ` (you chose ${remembered.label} last time)` : '';
    return { profile: best.profile, detection: `detected: ${best.result.reason}${note}`, confidence: best.result.confidence };
  }
  const reasons = (await runDetectors(root)).map((r) => r.result.reason).join('; ');
  return {
    profile: defaultProfile,
    detection: `not recognised (${reasons}); assuming ${defaultProfile.label}`,
    confidence: 0,
  };
}

export function useDevice(): Device {
  const [source, setSourceState] = useState<DeviceSource | null>(null);
  const [profile, setProfileState] = useState<DeviceProfile>(defaultProfile);
  const [detection, setDetection] = useState('');
  /** The active profile's folder-detection confidence; Infinity once a profile is chosen by hand. */
  const confidenceRef = useRef(0);
  /** Source keys whose suggestion was dismissed. Kept in memory only. */
  const [dismissed, setDismissed] = useState<ReadonlySet<number>>(() => new Set());
  /**
   * The suggestion computed when the last load of a source settled. It is kept while a later load
   * runs, so the notice does not flicker, and recomputed only when a load settles.
   */
  const [settledCount, setSettledCount] = useState(0);
  const [settledSuggestion, setSettledSuggestion] = useState<{ key: number; value: RankedProfile | null } | null>(null);
  const [rows, setRowsState] = useState<LibraryRow[]>([]);
  const [folders, setFolders] = useState<Record<string, string[]>>({});
  const [progress, setProgress] = useState<Progress | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [recovered, setRecovered] = useState<string[]>([]);
  const [recoveryError, setRecoveryError] = useState<string | null>(null);
  const [skipped, setSkipped] = useState<string[]>([]);
  const [changeCount, setChangeCount] = useState(0);
  /** Bumped after every operation on a staged source, successful or not. */
  const [revision, setRevision] = useState(0);
  /** revision when the changes were last downloaded; null when never downloaded. */
  const [downloadedRevision, setDownloadedRevision] = useState<number | null>(null);
  const [remembered, setRemembered] = useState<FileSystemDirectoryHandle | null>(null);
  const [installedPlugins, setInstalledPlugins] = useState<Inventory | null>(null);
  const [fsAccess] = useState(isFsAccessSupported);
  const [pending, setPending] = useState(0);
  const [mutating, setMutating] = useState(0);
  const [queue] = useState(() => createQueue(setPending));

  // The latest values, read by async work that outlives the render that started it.
  const sourceRef = useRef<DeviceSource | null>(null);
  const profileRef = useRef<DeviceProfile>(defaultProfile);
  const rowsRef = useRef<LibraryRow[]>([]);
  /** Bumped by every load, so a superseded load never writes state. */
  const generation = useRef(0);
  /** Bumped by every open and disconnect, so a superseded open stops. */
  const session = useRef(0);
  const abortRef = useRef<AbortController | null>(null);
  const nextKey = useRef(0);

  const setSource = useCallback((next: DeviceSource | null) => {
    sourceRef.current = next;
    setSourceState(next);
  }, []);
  const setProfileBoth = useCallback((next: DeviceProfile) => {
    profileRef.current = next;
    setProfileState(next);
  }, []);
  const setRows = useCallback((next: LibraryRow[]) => {
    rowsRef.current = next;
    setRowsState(next);
  }, []);

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

  // Warn before leaving with staged changes not downloaded, or with a write in flight.
  const unsavedChanges = changeCount > 0 && revision !== downloadedRevision;
  const dirty = unsavedChanges || mutating > 0;
  useEffect(() => {
    if (!dirty) return;
    const warn = (event: BeforeUnloadEvent): void => {
      event.preventDefault();
      event.returnValue = '';
    };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [dirty]);

  /**
   * Lists the drive and reads patches. With `touched` (after an operation), rows whose path and
   * size are unchanged keep their details and only the rest are read; with null everything is read.
   * Rows stay populated throughout. A newer load aborts this one.
   */
  const load = useCallback(
    async (next: DeviceSource, nextProfile: DeviceProfile, touched: readonly string[] | null): Promise<void> => {
      abortRef.current?.abort();
      const controller = new AbortController();
      abortRef.current = controller;
      const id = ++generation.current;
      const current = (): boolean => id === generation.current;
      setError(null);
      setLoading(true);
      try {
        const listed = await loadLibrary(next.store, nextProfile);
        const nextFolders = await listFolders(next.store, nextProfile);
        if (!current()) return;
        const previous = touched === null ? new Map<string, LibraryRow>() : new Map(rowsRef.current.map((r) => [r.path, r]));
        const merged = mergeRows(listed, previous, new Set(touched ?? []));
        setRows(merged.rows);
        setFolders(nextFolders);
        setChangeCount(next.staged ? countChanges(next.staged.changes()) : 0);
        if (merged.toLoad.length === 0) return;
        let last = 0;
        setProgress({ done: 0, total: merged.toLoad.length });
        const loaded = await loadAllPatches(
          next.store,
          nextProfile,
          merged.toLoad,
          (done, total) => {
            const now = performance.now();
            if (!current() || (done < total && now - last < PROGRESS_INTERVAL_MS)) return;
            last = now;
            setProgress({ done, total });
          },
          DEFAULT_CONCURRENCY,
          controller.signal,
        );
        if (!current()) return;
        const read = new Map(loaded.map((entry) => [entry.path, toRow(entry)]));
        setRows(merged.rows.map((row) => read.get(row.path) ?? row));
      } catch (e) {
        if (current() && !isAbort(e)) setError(messageOf(e));
      } finally {
        if (current()) {
          setLoading(false);
          setProgress(null);
          const traits = rowsRef.current.map((row) => row.info?.traits).filter((t): t is SoundTraits => t != null);
          setSettledSuggestion({ key: next.key, value: suggestProfile(traits, nextProfile, confidenceRef.current) });
          setSettledCount((n) => n + 1);
        }
      }
    },
    [setRows],
  );

  /** Opens a store: detects the profile, recovers interrupted writes, wraps read-only sources, loads. */
  const open = useCallback(
    async (base: DeviceStore, droppedSkips: string[]): Promise<void> => {
      const id = ++session.current;
      const live = (): boolean => id === session.current;
      setError(null);
      setRecovered([]);
      setRecoveryError(null);
      setRevision(0);
      setDownloadedRevision(null);
      setSkipped(droppedSkips);
      setLoading(true);
      try {
        const found = await detect(base, loadRememberedProfile());
        if (!live()) return;
        let restored: string[] = [];
        try {
          restored = await recoverDrive(base, found.profile);
        } catch (e) {
          // Kept apart from error, which the load below clears.
          setRecoveryError(`could not finish recovering interrupted writes: ${messageOf(e)}`);
        }
        if (!live()) return;
        const staged = base.capabilities.write ? null : new StagedStore(base);
        const next: DeviceSource = { store: staged ?? base, staged, key: ++nextKey.current };
        setRows([]);
        setProfileBoth(found.profile);
        setDetection(found.detection);
        confidenceRef.current = found.confidence;
        setRecovered(restored);
        setSource(next);
        await load(next, found.profile, null);
      } catch (e) {
        if (live()) {
          setError(messageOf(e));
          setLoading(false);
        }
      }
    },
    [load, setProfileBoth, setRows, setSource],
  );

  const exclusive = useCallback(<T,>(task: () => Promise<T>): Promise<T> => queue.run(task), [queue]);

  const connect = useCallback(
    () =>
      exclusive(async () => {
        try {
          const store = await connectDevice();
          if (!store) return;
          await rememberHandle(store.root);
          setRemembered(store.root);
          await open(store, []);
        } catch (e) {
          setError(messageOf(e));
        }
      }),
    [exclusive, open],
  );

  const reconnect = useCallback(
    () =>
      exclusive(async () => {
        if (!remembered) return;
        if (!(await ensurePermission(remembered))) {
          setError(`permission to open ${remembered.name} was not granted`);
          return;
        }
        await open(new FsAccessStore(remembered), []);
      }),
    [exclusive, open, remembered],
  );

  const openDrop = useCallback(
    (files: DroppedFile[], dropSkipped: string[]) =>
      exclusive(async () => {
        try {
          if (files.length === 0) throw new Error('nothing readable was dropped; drop the OP-1 drive folder');
          await open(new DropStore(files), dropSkipped);
        } catch (e) {
          setSkipped(dropSkipped);
          setError(messageOf(e));
        }
      }),
    [exclusive, open],
  );

  const openInput = useCallback(
    (files: Iterable<File>) => {
      const list = [...files];
      return exclusive(async () => {
        try {
          if (list.length === 0) throw new Error('the chosen folder is empty');
          await open(fromInputFiles(list), []);
        } catch (e) {
          setError(messageOf(e));
        }
      });
    },
    [exclusive, open],
  );

  const fail = useCallback((message: string) => setError(message), []);

  const setProfile = useCallback(
    (id: string) =>
      exclusive(async () => {
        const next = getProfile(id);
        if (!next) return;
        rememberProfile(next.id);
        setProfileBoth(next);
        setDetection('chosen by hand');
        // A choice made by hand is never second-guessed.
        confidenceRef.current = Number.POSITIVE_INFINITY;
        setSettledSuggestion(null);
        const current = sourceRef.current;
        if (current) await load(current, next, null);
      }),
    [exclusive, load, setProfileBoth],
  );

  const run = useCallback(
    (op: Operation, touched: readonly string[] = []) =>
      exclusive(async () => {
        const current = sourceRef.current;
        if (!current) throw new Error('nothing is connected');
        setMutating((n) => n + 1);
        try {
          await op(current.store, current);
        } finally {
          // Also after a failure: an operation can stage part of its changes before it fails.
          if (current.staged && sourceRef.current === current) setRevision((n) => n + 1);
          try {
            if (sourceRef.current === current) await load(current, profileRef.current, touched);
          } finally {
            setMutating((n) => n - 1);
          }
        }
      }),
    [exclusive, load],
  );

  const refresh = useCallback(
    () =>
      exclusive(async () => {
        const current = sourceRef.current;
        if (current) await load(current, profileRef.current, null);
      }),
    [exclusive, load],
  );

  const disconnect = useCallback(() => {
    session.current++;
    generation.current++;
    abortRef.current?.abort();
    setSource(null);
    setRows([]);
    setFolders({});
    setProgress(null);
    setLoading(false);
    setError(null);
    setRecovered([]);
    setRecoveryError(null);
    setSkipped([]);
    setChangeCount(0);
    setRevision(0);
    setDownloadedRevision(null);
  }, [setRows, setSource]);

  const markDownloaded = useCallback(() => setDownloadedRevision(revision), [revision]);

  const suggestion =
    source && settledSuggestion?.key === source.key && !dismissed.has(source.key) ? settledSuggestion.value : null;

  const currentProfile = useCallback(() => profileRef.current, []);

  const dismissSuggestion = useCallback(() => {
    const key = sourceRef.current?.key;
    if (key !== undefined) setDismissed((prev) => new Set(prev).add(key));
  }, []);

  return {
    source,
    profile,
    detection,
    suggestion,
    dismissSuggestion,
    currentProfile,
    settledCount,
    rows,
    folders,
    progress,
    loading,
    busy: pending > 0,
    error,
    recovered,
    recoveryError,
    skipped,
    changeCount,
    unsavedChanges,
    markDownloaded,
    remembered,
    fsAccess,
    connect,
    reconnect,
    openDrop,
    openInput,
    fail,
    setProfile,
    installedPlugins,
    setInstalledPlugins,
    run,
    exclusive,
    refresh,
    disconnect,
  };
}
