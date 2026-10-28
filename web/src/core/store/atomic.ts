import { basename, dirname, joinPath, type DeviceStore } from './store';

async function removeQuietly(store: DeviceStore, path: string): Promise<void> {
  try {
    if (await store.exists(path)) await store.remove(path);
  } catch {
    // Best effort: a leftover dot file is invisible to the scanner and overwritten next time.
  }
}

/**
 * Writes a file so that a failure part-way never leaves a truncated target: the bytes go to
 * `.co-op-tmp-<name>` in the same folder, an existing target is set aside as `.co-op-old-<name>`,
 * the temp is moved into place, and only then is the old file removed. If any move fails, the old
 * file is put back, the temp is removed and the error is rethrown. The scanner skips dot files, so
 * a leftover from a crash never shows as a patch.
 */
export async function writeAtomic(store: DeviceStore, path: string, bytes: Uint8Array): Promise<void> {
  const folder = dirname(path);
  const name = basename(path);
  const target = joinPath(path);
  const temp = joinPath(folder, `.co-op-tmp-${name}`);
  const old = joinPath(folder, `.co-op-old-${name}`);

  await store.write(temp, bytes);
  let setAside = false;
  try {
    if (await store.exists(target)) {
      await removeQuietly(store, old);
      await store.move(target, old);
      setAside = true;
    }
    await store.move(temp, target);
  } catch (error) {
    if (setAside) {
      try {
        await store.move(old, target);
      } catch {
        // The original stays at the .co-op-old- name; nothing more can be done here.
      }
    }
    await removeQuietly(store, temp);
    throw error;
  }
  if (setAside) await removeQuietly(store, old);
}
