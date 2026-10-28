import type { Patch } from '../codec/patch';
import { readPatch } from '../codec/patch';
import type { Compat, DeviceProfile } from '../profiles/types';
import { scanDrive, type ScannedPatch } from '../profiles/scan';
import type { DeviceStore } from '../store/store';

export interface LibraryEntry extends ScannedPatch {
  sizeBytes: number;
  /** Filled by loadEntryPatch; null until loaded. */
  patch: Patch | null;
  /** Set when reading or parsing failed; the entry stays visible as unreadable. */
  error: string | null;
  compat: Compat | null;
  /** True when the folder kind (synth/drum) disagrees with the metadata kind. */
  kindMismatch: boolean;
}

/** How many store calls run at once by default. */
export const DEFAULT_CONCURRENCY = 4;

const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/**
 * Maps `items` through `fn` with at most `concurrency` calls in flight, keeping input order.
 * A concurrency that is not a positive number (0, negative, NaN) runs one at a time; fractions
 * round down. `onDone` is called after each item with the running count.
 */
async function mapPool<T, R>(
  items: readonly T[],
  concurrency: number,
  fn: (item: T) => Promise<R>,
  onDone?: (done: number, total: number) => void,
): Promise<R[]> {
  const results: R[] = new Array(items.length);
  let next = 0;
  let done = 0;
  const worker = async (): Promise<void> => {
    while (next < items.length) {
      const index = next++;
      results[index] = await fn(items[index] as T);
      done++;
      onDone?.(done, items.length);
    }
  };
  const workers = Math.max(1, Math.min(Math.floor(concurrency) || 1, items.length));
  await Promise.all(Array.from({ length: workers }, worker));
  return results;
}

/**
 * Scans the drive with the profile and sizes every patch file without reading it, with at most
 * `concurrency` size calls in flight. Patches are loaded separately (loadEntryPatch,
 * loadAllPatches) so a large drive lists quickly. An entry whose size cannot be read gets
 * sizeBytes 0 and the error, rather than failing the whole listing.
 */
export async function loadLibrary(
  store: DeviceStore,
  profile: DeviceProfile,
  concurrency = DEFAULT_CONCURRENCY,
): Promise<LibraryEntry[]> {
  const scanned = await scanDrive(store, profile);
  return mapPool(scanned, concurrency, async (found): Promise<LibraryEntry> => {
    const base = { ...found, patch: null, compat: null, kindMismatch: false };
    try {
      return { ...base, sizeBytes: await store.size(found.path), error: null };
    } catch (error) {
      return { ...base, sizeBytes: 0, error: messageOf(error) };
    }
  });
}

/**
 * Reads and parses one entry, returning a new entry with patch, compat and kindMismatch filled.
 * The folder kind stays the entry's kind (it decides where the patch lives); a disagreeing
 * metadata kind only sets kindMismatch. Any read or parse failure is returned as `error` with the
 * patch left null, never thrown.
 */
export async function loadEntryPatch(
  store: DeviceStore,
  profile: DeviceProfile,
  entry: LibraryEntry,
): Promise<LibraryEntry> {
  try {
    const patch = readPatch(await store.read(entry.path));
    return {
      ...entry,
      patch,
      error: null,
      compat: profile.compatibility(patch),
      kindMismatch: patch.kind !== 'unknown' && patch.kind !== entry.kind,
    };
  } catch (error) {
    return { ...entry, patch: null, error: messageOf(error), compat: null, kindMismatch: false };
  }
}

/**
 * Loads every entry with at most `concurrency` reads in flight. The result is in the same order as
 * `entries`; onProgress is called once per finished entry with the running count.
 */
export async function loadAllPatches(
  store: DeviceStore,
  profile: DeviceProfile,
  entries: readonly LibraryEntry[],
  onProgress?: (done: number, total: number) => void,
  concurrency = DEFAULT_CONCURRENCY,
): Promise<LibraryEntry[]> {
  return mapPool(entries, concurrency, (entry) => loadEntryPatch(store, profile, entry), onProgress);
}
