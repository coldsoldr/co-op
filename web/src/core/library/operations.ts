import { readPatch, withMeta } from '../codec/patch';
import { writeAtomic } from '../store/atomic';
import { StoreError, basename, dirname, joinPath, type DeviceStore } from '../store/store';

/** A library operation was refused because of its arguments (a bad name, an occupied target). */
export class OperationError extends Error {
  override name = 'OperationError';
}

/** The longest patch name the OP-1 shows; a UI guard, not a device limit. */
export const MAX_PATCH_NAME = 24;

/**
 * Refuses up front on a read-only store, so an operation never gets part-way. Wrap a read-only
 * source in a StagedStore to collect changes instead.
 */
function assertWritable(store: DeviceStore, path: string): void {
  if (!store.capabilities.write) throw new StoreError(`read-only source: cannot change ${joinPath(path)}`);
}

/** Rejects names that are empty, contain a path separator, or start with '.' (hidden from the scanner). */
function assertEntryName(name: string, what: string): void {
  if (name === '') throw new OperationError(`${what} name is empty`);
  if (name.includes('/') || name.includes('\\')) throw new OperationError(`${what} name "${name}" contains a slash`);
  if (name.startsWith('.')) throw new OperationError(`${what} name "${name}" starts with "."`);
}

/**
 * The OP-1 drive is FAT, so names are case-insensitive there even when the store (MemoryStore, a
 * dropped folder) is not. Every collision check compares lower-cased names from a folder listing.
 */
const sameName = (a: string, b: string): boolean => a.toLowerCase() === b.toLowerCase();

/** The entry in `folderPath` whose name matches `name` ignoring case, if any. */
async function findName(store: DeviceStore, folderPath: string, name: string): Promise<string | undefined> {
  return (await store.list(folderPath)).find((e) => sameName(e.name, name))?.name;
}

/**
 * `fileName` if nothing in the folder has that name in any case, otherwise the first of
 * `name-2.ext`, `name-3.ext`, ... that is free. The extension is everything from the last '.'.
 */
export async function freeName(store: DeviceStore, folderPath: string, fileName: string): Promise<string> {
  const taken = new Set((await store.list(folderPath)).map((e) => e.name.toLowerCase()));
  if (!taken.has(fileName.toLowerCase())) return fileName;
  const dot = fileName.lastIndexOf('.');
  const stem = dot > 0 ? fileName.slice(0, dot) : fileName;
  const ext = dot > 0 ? fileName.slice(dot) : '';
  for (let n = 2; ; n++) {
    const candidate = `${stem}-${n}${ext}`;
    if (!taken.has(candidate.toLowerCase())) return candidate;
  }
}

/**
 * Renames a file within its folder and returns the new path. Refuses a name already used by
 * another entry in any case. A case-only rename of the same file (kick.aif to Kick.aif) goes
 * through a dot-prefixed temporary name, since a FAT drive treats the two names as one.
 */
export async function renameFile(store: DeviceStore, path: string, newFileName: string): Promise<string> {
  assertWritable(store, path);
  assertEntryName(newFileName, 'file');
  const source = joinPath(path);
  const folder = dirname(source);
  const target = joinPath(folder, newFileName);
  if (target === source) return source;
  if (sameName(basename(source), newFileName)) {
    const temp = joinPath(folder, `.co-op-rename-${newFileName}`);
    await store.move(source, temp);
    try {
      await store.move(temp, target);
    } catch (error) {
      await store.move(temp, source).catch(() => undefined);
      throw error;
    }
    return target;
  }
  if ((await findName(store, folder, newFileName)) !== undefined) {
    throw new OperationError(`${target} already exists`);
  }
  await store.move(source, target);
  return target;
}

/**
 * Changes the name stored in the patch's op-1 metadata. This is the one operation that
 * re-serialises a file (through withMeta); the audio chunks are untouched.
 */
export async function setPatchName(store: DeviceStore, path: string, newName: string): Promise<void> {
  assertWritable(store, path);
  if (newName.trim() === '') throw new OperationError('patch name is empty');
  if (newName.length > MAX_PATCH_NAME) {
    throw new OperationError(`patch name is longer than ${MAX_PATCH_NAME} characters`);
  }
  const patch = readPatch(await store.read(path));
  if (!patch.meta) throw new OperationError(`${joinPath(path)} has no op-1 metadata to rename`);
  const edited = withMeta(patch, { ...patch.meta, name: newName });
  await writeAtomic(store, path, edited.raw);
}

/**
 * Moves a file into `targetFolder` (for example 'synth/user'), taking a free name there if its own
 * is used. The bytes are moved untouched. Returns the new path.
 */
export async function movePatch(store: DeviceStore, path: string, targetFolder: string): Promise<string> {
  assertWritable(store, path);
  const source = joinPath(path);
  const folder = joinPath(targetFolder);
  if (dirname(source) === folder) return source;
  const target = joinPath(folder, await freeName(store, folder, basename(source)));
  await store.move(source, target);
  return target;
}

/**
 * Copies a file's bytes, unchanged, into `targetFolder` under a free name (so copying into the
 * same folder yields `name-2.aif`). Returns the new path.
 */
export async function copyPatch(store: DeviceStore, path: string, targetFolder: string): Promise<string> {
  assertWritable(store, path);
  const source = joinPath(path);
  const folder = joinPath(targetFolder);
  const bytes = await store.read(source);
  const target = joinPath(folder, await freeName(store, folder, basename(source)));
  await writeAtomic(store, target, bytes);
  return target;
}

export async function deletePatch(store: DeviceStore, path: string): Promise<void> {
  assertWritable(store, path);
  await store.remove(path);
}

/**
 * Creates `<rootPath>/<name>` (for example 'synth/pads') if missing and returns its path. A folder
 * whose name differs only in case is the same folder on the drive, so its path is returned as is.
 */
export async function ensureFolder(store: DeviceStore, rootPath: string, name: string): Promise<string> {
  assertWritable(store, rootPath);
  assertEntryName(name, 'folder');
  const existing = (await store.list(rootPath)).find((e) => e.kind === 'dir' && sameName(e.name, name));
  if (existing) return joinPath(rootPath, existing.name);
  const folder = joinPath(rootPath, name);
  await store.mkdir(folder);
  return folder;
}
