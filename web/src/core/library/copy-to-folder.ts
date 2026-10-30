import { zipSync, type Zippable } from 'fflate';
import { writeAtomic } from '../store/atomic';
import { basename, dirname, joinPath, type DeviceStore } from '../store/store';
import { messageOf, sha256Hex } from '../util';

/** What copying needs to know of a community patch. `path` is `<pack slug>/<file in the pack>`. */
export interface CopyPatch {
  path: string;
  pack: { sha256: string };
}

export interface CopyResult {
  /** Where each new file was written. */
  copied: string[];
  /** Patches that were in the folder already. */
  already: string[];
  failed: { path: string; message: string }[];
}

type Progress = (done: number, total: number) => void;

/** The name to write `path` under, or null when a file with this hash is already there. */
async function targetFor(store: DeviceStore, path: string, sha256: string): Promise<string | null> {
  const folder = dirname(path);
  const name = basename(path);
  const dot = name.lastIndexOf('.');
  for (let n = 1; ; n++) {
    const candidate = n === 1 ? path : joinPath(folder, `${name.slice(0, dot)}-${n}${name.slice(dot)}`);
    if (!(await store.exists(candidate))) return candidate;
    if ((await sha256Hex(await store.read(candidate))) === sha256.toLowerCase()) return null;
  }
}

/** Copies patches into a folder in turn. Nothing is overwritten, and a failure does not stop the rest. */
export async function copyPatches<T extends CopyPatch>(
  store: DeviceStore,
  patches: readonly T[],
  fetchBytes: (patch: T) => Promise<Uint8Array>,
  onProgress?: Progress,
): Promise<CopyResult> {
  const result: CopyResult = { copied: [], already: [], failed: [] };
  let done = 0;
  for (const patch of patches) {
    try {
      const target = await targetFor(store, patch.path, patch.pack.sha256);
      if (target === null) {
        result.already.push(patch.path);
      } else {
        await writeAtomic(store, target, await fetchBytes(patch));
        result.copied.push(target);
      }
    } catch (error) {
      result.failed.push({ path: patch.path, message: messageOf(error) });
    }
    onProgress?.(++done, patches.length);
  }
  return result;
}

/** The same copy as a zip, for browsers that cannot open a folder. Audio is stored, not compressed. */
export async function zipPatches<T extends CopyPatch>(
  patches: readonly T[],
  fetchBytes: (patch: T) => Promise<Uint8Array>,
  onProgress?: Progress,
): Promise<{ zip: Uint8Array; result: CopyResult }> {
  const result: CopyResult = { copied: [], already: [], failed: [] };
  const files: Zippable = {};
  let done = 0;
  for (const patch of patches) {
    try {
      files[patch.path] = [await fetchBytes(patch), { level: 0 }];
      result.copied.push(patch.path);
    } catch (error) {
      result.failed.push({ path: patch.path, message: messageOf(error) });
    }
    onProgress?.(++done, patches.length);
  }
  return { zip: zipSync(files), result };
}
