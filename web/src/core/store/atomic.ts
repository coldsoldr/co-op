import { StoreError, basename, dirname, joinPath, type DeviceStore } from './store';

const TEMP_PREFIX = '.co-op-tmp-';
const OLD_PREFIX = '.co-op-old-';

async function removeQuietly(store: DeviceStore, path: string): Promise<void> {
  try {
    if (await store.exists(path)) await store.remove(path);
  } catch {
    // Best effort: a leftover dot file is invisible to the scanner and cleaned up by recoverStranded.
  }
}

/**
 * Writes a file so that a failure part-way never leaves a truncated target: the bytes go to
 * `.co-op-tmp-<name>` in the same folder, an existing target is set aside as `.co-op-old-<name>`,
 * the temp is moved into place, and only then is the old file removed. If any step fails, the old
 * file is put back, the temp is removed and the error is rethrown. If putting the old file back
 * also fails, a StoreError naming where the old file was kept is thrown, with the original error
 * as its cause.
 *
 * An old file stranded by an earlier failed write (target missing, `.co-op-old-<name>` present) is
 * moved back first, so it is never lost. The scanner skips dot files, so leftovers never show as
 * patches.
 */
export async function writeAtomic(store: DeviceStore, path: string, bytes: Uint8Array): Promise<void> {
  const folder = dirname(path);
  const name = basename(path);
  const target = joinPath(path);
  const temp = joinPath(folder, `${TEMP_PREFIX}${name}`);
  const old = joinPath(folder, `${OLD_PREFIX}${name}`);

  if (store.capabilities.write && !(await store.exists(target)) && (await store.exists(old))) {
    await store.move(old, target);
  }

  let setAside = false;
  try {
    await store.write(temp, bytes);
    if (await store.exists(target)) {
      await removeQuietly(store, old);
      await store.move(target, old);
      setAside = true;
    }
    await store.move(temp, target);
  } catch (error) {
    await removeQuietly(store, temp);
    if (setAside) {
      try {
        await store.move(old, target);
      } catch {
        const detail = error instanceof Error ? error.message : String(error);
        throw new StoreError(`could not write ${target}: ${detail}; the previous file is kept at ${old}`, {
          cause: error,
        });
      }
    }
    throw error;
  }
  if (setAside) await removeQuietly(store, old);
}

/**
 * Cleans up after writes interrupted in `folder` (not recursive): every `.co-op-old-X` whose `X`
 * is missing is moved back to `X`, an `.co-op-old-X` whose `X` exists is stale (the write finished)
 * and is removed, and every `.co-op-tmp-*` is removed. Returns the restored paths. Does nothing on
 * a read-only store.
 */
export async function recoverStranded(store: DeviceStore, folder: string): Promise<string[]> {
  if (!store.capabilities.write) return [];
  const restored: string[] = [];
  for (const entry of await store.list(folder)) {
    if (entry.kind !== 'file') continue;
    const path = joinPath(folder, entry.name);
    if (entry.name.startsWith(TEMP_PREFIX)) {
      await store.remove(path);
    } else if (entry.name.startsWith(OLD_PREFIX) && entry.name.length > OLD_PREFIX.length) {
      const target = joinPath(folder, entry.name.slice(OLD_PREFIX.length));
      if (await store.exists(target)) {
        await store.remove(path);
      } else {
        await store.move(path, target);
        restored.push(target);
      }
    }
  }
  return restored;
}
