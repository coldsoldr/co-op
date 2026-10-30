import { readPatch, withMeta } from '../codec/patch';
import { isWindowsReservedName } from '@co-op/shared/packs';
import { OLD_PREFIX, TEMP_PREFIX, writeAtomic } from '../store/atomic';
import { StoreError, basename, dirname, joinPath, splitPath, type DeviceStore } from '../store/store';

/** A library operation was refused because of its arguments (a bad name, an occupied target). */
export class OperationError extends Error {
  override name = 'OperationError';
}

/** The longest patch name the OP-1 shows; a UI guard, not a device limit. */
export const MAX_PATCH_NAME = 24;

/** Extensions a renamed patch file must keep, compared case-insensitively. */
export const PATCH_EXTENSIONS: readonly string[] = ['.aif', '.aiff'];


/** How often copyPatch retries when another writer takes its chosen name. */
const COPY_ATTEMPTS = 5;

/** Numbers copyPatch temp names; the random part covers several tabs on one drive. */
let copyCounter = 0;

/**
 * Refuses up front on a read-only store, so an operation never gets part-way. Wrap a read-only
 * source in a StagedStore to collect changes instead.
 */
function assertWritable(store: DeviceStore, path: string): void {
  if (!store.capabilities.write) throw new StoreError(`read-only source: cannot change ${joinPath(path)}`);
}

// eslint-disable-next-line no-control-regex
const FAT_ILLEGAL = /[\u0000-\u001f\u007f:*?"<>|/\\]/;

/**
 * Rejects names the OP-1's FAT drive cannot hold or the scanner would hide: empty, a path
 * separator, one of `:*?"<>|`, a control character, a trailing dot or space, or a leading '.'.
 */
function assertEntryName(name: string, what: string): void {
  if (name === '') throw new OperationError(`${what} name is empty`);
  if (FAT_ILLEGAL.test(name)) {
    throw new OperationError(
      `${what} name ${JSON.stringify(name)} contains a character the OP-1 drive cannot hold (/ \\ : * ? " < > | or a control character)`,
    );
  }
  if (name.startsWith('.')) throw new OperationError(`${what} name "${name}" starts with "."`);
  if (isWindowsReservedName(name)) {
    throw new OperationError(`${what} name "${name}" is reserved by Windows`);
  }
  if (name.endsWith('.') || name.endsWith(' ')) {
    throw new OperationError(`${what} name "${name}" ends with a dot or a space`);
  }
}

/** The longest patch file name accepted; matches MAX_FILE_NAME in shared/src/packs.ts. */
export const MAX_FILE_NAME = 64;

export function assertPatchFileName(name: string): void {
  assertEntryName(name, 'file');
  if (name.length > MAX_FILE_NAME) {
    throw new OperationError(`file name "${name}" is longer than ${MAX_FILE_NAME} characters`);
  }
  const lower = name.toLowerCase();
  if (!PATCH_EXTENSIONS.some((ext) => lower.endsWith(ext) && lower.length > ext.length)) {
    throw new OperationError(`file name "${name}" must end in ${PATCH_EXTENSIONS.join(' or ')}`);
  }
}

/**
 * The OP-1 drive is FAT, so names are case-insensitive there even when the store (MemoryStore, a
 * dropped folder) is not. Every collision check compares lower-cased names from a folder listing.
 */
const sameName = (a: string, b: string): boolean => a.toLowerCase() === b.toLowerCase();

/** The entry in `folderPath` whose name matches `name` ignoring case, if any. */
async function findEntry(store: DeviceStore, folderPath: string, name: string) {
  return (await store.list(folderPath)).find((e) => sameName(e.name, name));
}

/**
 * The folder path as the drive spells it: each segment is replaced by an existing folder whose
 * name matches it ignoring case, until a segment has no match; the rest is kept as given.
 */
async function resolveFolder(store: DeviceStore, folderPath: string): Promise<string> {
  let resolved = '';
  const parts = splitPath(folderPath);
  for (const [i, part] of parts.entries()) {
    const entry = await findEntry(store, resolved, part);
    if (!entry || entry.kind !== 'dir') return joinPath(resolved, ...parts.slice(i));
    resolved = joinPath(resolved, entry.name);
  }
  return resolved;
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
 * A case-only rename (kick.aif to Kick.aif): a FAT drive treats the two names as one, so the file
 * is parked at `.co-op-old-<original>` first. If the second move fails the file is moved back; if
 * that fails too, the error names where the file is, and recoverStranded will restore it.
 */
async function renameCaseOnly(store: DeviceStore, source: string, target: string): Promise<void> {
  const parked = joinPath(dirname(source), `${OLD_PREFIX}${basename(source)}`);
  // A leftover from an earlier crash is stale here: the original is still in place.
  if (await store.exists(parked)) await store.remove(parked);
  await store.move(source, parked);
  try {
    await store.move(parked, target);
  } catch (error) {
    try {
      await store.move(parked, source);
    } catch (restoreError) {
      throw new StoreError(
        `renaming ${source} failed and the file could not be moved back; it was kept as ${parked}`,
        { cause: restoreError },
      );
    }
    throw error;
  }
}

/**
 * Renames a file within its folder and returns the new path. The new name must be FAT-safe and end
 * in .aif or .aiff; a name already used by another entry in any case is refused.
 */
export async function renameFile(store: DeviceStore, path: string, newFileName: string): Promise<string> {
  assertWritable(store, path);
  assertPatchFileName(newFileName);
  const source = joinPath(path);
  const folder = dirname(source);
  const target = joinPath(folder, newFileName);
  if (target === source) return source;
  if (sameName(basename(source), newFileName)) {
    await renameCaseOnly(store, source, target);
    return target;
  }
  if ((await findEntry(store, folder, newFileName)) !== undefined) {
    throw new OperationError(`${target} already exists`);
  }
  await store.move(source, target);
  return target;
}

/**
 * Changes the name stored in the patch's op-1 metadata to `newName` trimmed. This is the one
 * operation that re-serialises a file (through withMeta); the audio chunks are untouched.
 */
export async function setPatchName(store: DeviceStore, path: string, newName: string): Promise<void> {
  assertWritable(store, path);
  const name = newName.trim();
  if (name === '') throw new OperationError('patch name is empty');
  if (name.length > MAX_PATCH_NAME) {
    throw new OperationError(`patch name is longer than ${MAX_PATCH_NAME} characters`);
  }
  const patch = readPatch(await store.read(path));
  if (!patch.meta) throw new OperationError(`${joinPath(path)} has no op-1 metadata to rename`);
  const edited = withMeta(patch, { ...patch.meta, name });
  await writeAtomic(store, path, edited.raw);
}

/**
 * Moves a file into `targetFolder` (for example 'synth/user', matched to existing folders ignoring
 * case), taking a free name there if its own is used. The bytes are moved untouched. Returns the
 * new path; moving into the folder the file is already in changes nothing.
 */
export async function movePatch(store: DeviceStore, path: string, targetFolder: string): Promise<string> {
  assertWritable(store, path);
  const source = joinPath(path);
  const folder = await resolveFolder(store, targetFolder);
  if (sameName(dirname(source), folder)) return source;
  const target = joinPath(folder, await freeName(store, folder, basename(source)));
  await store.move(source, target);
  return target;
}

async function removeQuietly(store: DeviceStore, path: string): Promise<void> {
  try {
    if (await store.exists(path)) await store.remove(path);
  } catch {
    // A leftover .co-op-tmp- file is invisible to the scanner and cleared by recoverStranded.
  }
}

/**
 * Copies a file's bytes, unchanged, into `targetFolder` (matched ignoring case) under a free name,
 * so copying into the same folder yields `name-2.aif`. The bytes go to a dot temp file first and
 * are moved into place; the move refuses an existing target, so a name taken meanwhile by another
 * writer is never overwritten and the next free name is tried instead. Returns the new path.
 */
export async function copyPatch(store: DeviceStore, path: string, targetFolder: string): Promise<string> {
  assertWritable(store, path);
  const source = joinPath(path);
  const folder = await resolveFolder(store, targetFolder);
  const bytes = await store.read(source);
  // Unique per call, so two copies in flight never share (and clobber) one temp file.
  const temp = joinPath(folder, `${TEMP_PREFIX}copy-${++copyCounter}-${crypto.randomUUID()}-${basename(source)}`);
  await store.write(temp, bytes);
  for (let attempt = 1; ; attempt++) {
    const target = joinPath(folder, await freeName(store, folder, basename(source)));
    try {
      await store.move(temp, target);
      return target;
    } catch (error) {
      const clash = await store.exists(target).catch(() => false);
      if (!clash || attempt >= COPY_ATTEMPTS) {
        await removeQuietly(store, temp);
        throw error;
      }
    }
  }
}

export async function deletePatch(store: DeviceStore, path: string): Promise<void> {
  assertWritable(store, path);
  await store.remove(path);
}

/** The folder in each patch root that holds the OP-1's 8 quick access presets; it always stays. */
export const PRESET_FOLDER = 'user';

export const isPresetFolder = (name: string): boolean => sameName(name, PRESET_FOLDER);

/**
 * Removes a patch folder and every file in it. The preset folder is refused, and so is a folder
 * holding another folder, before anything is deleted.
 */
export async function removeFolder(store: DeviceStore, folderPath: string): Promise<void> {
  const folder = joinPath(folderPath);
  assertWritable(store, folder);
  if (isPresetFolder(basename(folder))) throw new OperationError(`${folder} holds the OP-1's quick access presets`);
  const entries = await store.list(folder);
  const nested = entries.find((e) => e.kind === 'dir');
  if (nested) throw new OperationError(`${folder} holds the folder ${nested.name}`);
  for (const entry of entries) await store.remove(joinPath(folder, entry.name));
  await store.removeFolder(folder);
}

/**
 * Creates `<rootPath>/<name>` (for example 'synth/pads') if missing and returns its path. A folder
 * whose name differs only in case is the same folder on the drive, so its path is returned as is;
 * a file with that name in any case is refused.
 */
export async function ensureFolder(store: DeviceStore, rootPath: string, name: string): Promise<string> {
  assertWritable(store, rootPath);
  assertEntryName(name, 'folder');
  const root = await resolveFolder(store, rootPath);
  const existing = await findEntry(store, root, name);
  if (existing?.kind === 'dir') return joinPath(root, existing.name);
  if (existing) throw new OperationError(`${joinPath(root, existing.name)} is a file, not a folder`);
  const folder = joinPath(root, name);
  await store.mkdir(folder);
  return folder;
}
