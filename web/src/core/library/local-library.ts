import { readPatch } from '../codec/patch';
import type { PatchKind } from '../codec/op1-meta';
import { soundTraits } from '../codec/sound-traits';
import { dirname, joinPath, basename, type DeviceStore } from '../store/store';
import { messageOf } from '../util';
import { DEFAULT_CONCURRENCY, mapPool } from './library';
import { PATCH_EXTENSIONS } from './operations';

/** One patch in the person's own folder on their computer. */
export interface LocalPatch {
  /** Relative to the chosen folder. */
  path: string;
  fileName: string;
  /** The folder it is in, relative to the chosen folder; '' for the chosen folder itself. */
  folder: string;
  /** The name in its metadata, or the file name without its extension. */
  name: string;
  kind: PatchKind;
  /** The sound engine and effect, when its metadata names them. */
  engine: string | null;
  fx: string | null;
  sizeBytes: number;
  /** Why it could not be read; such a file stays listed. */
  error: string | null;
}

export interface LocalFilter {
  /** Every whitespace-separated word must appear, ignoring case. */
  text?: string;
  folder?: string;
  kind?: PatchKind;
}

const isPatchFile = (name: string): boolean => {
  const lower = name.toLowerCase();
  return PATCH_EXTENSIONS.some((ext) => lower.endsWith(ext) && lower.length > ext.length);
};

const stem = (fileName: string): string => fileName.slice(0, fileName.lastIndexOf('.'));

/** Every patch file under `folder`, at any depth and in any layout; dot entries are skipped. */
export async function findPatchFiles(store: DeviceStore, folder = ''): Promise<string[]> {
  const found: string[] = [];
  for (const entry of await store.list(folder)) {
    if (entry.name.startsWith('.')) continue;
    const path = joinPath(folder, entry.name);
    if (entry.kind === 'dir') found.push(...(await findPatchFiles(store, path)));
    else if (isPatchFile(entry.name)) found.push(path);
  }
  return found;
}

async function readLocalPatch(store: DeviceStore, path: string): Promise<LocalPatch> {
  const fileName = basename(path);
  const base = { path, fileName, folder: dirname(path) };
  try {
    const bytes = await store.read(path);
    const patch = readPatch(bytes);
    const traits = soundTraits(patch.meta);
    return {
      ...base,
      name: patch.name || stem(fileName),
      kind: patch.kind,
      engine: traits?.engine || null,
      fx: traits?.fx || null,
      sizeBytes: bytes.length,
      error: null,
    };
  } catch (error) {
    return { ...base, name: stem(fileName), kind: 'unknown', engine: null, fx: null, sizeBytes: 0, error: messageOf(error) };
  }
}

/**
 * Reads every patch in the folder. Aborting `signal` stops further reads and rejects with its
 * reason, so a newer scan can replace this one.
 */
export async function scanLocalFolder(
  store: DeviceStore,
  onProgress?: (done: number, total: number) => void,
  signal?: AbortSignal,
): Promise<LocalPatch[]> {
  const paths = await findPatchFiles(store);
  return mapPool(paths, DEFAULT_CONCURRENCY, (path) => readLocalPatch(store, path), onProgress, signal);
}

/** The folders that hold patches, in code-point order. */
export function localFolders(patches: readonly LocalPatch[]): string[] {
  return [...new Set(patches.map((p) => p.folder))].sort();
}

/** The patches that match every given criterion, in the order given. */
export function filterLocal(patches: readonly LocalPatch[], filter: LocalFilter): LocalPatch[] {
  const words = (filter.text ?? '').toLowerCase().split(/\s+/).filter(Boolean);
  return patches.filter((patch) => {
    if (filter.folder !== undefined && patch.folder !== filter.folder) return false;
    if (filter.kind !== undefined && patch.kind !== filter.kind) return false;
    const haystack = [patch.name, patch.fileName, patch.folder, patch.engine ?? '', patch.fx ?? ''].join(' ').toLowerCase();
    return words.every((word) => haystack.includes(word));
  });
}
