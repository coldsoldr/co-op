import type { DeviceProfile } from '../profiles/types';
import { recoverStranded, recoverTree } from '../store/atomic';
import { joinPath, type DeviceStore } from '../store/store';

/** The drive's spelling of a top-level folder, compared ignoring case as on the FAT drive. */
async function topFolder(store: DeviceStore, name: string): Promise<string | null> {
  const lower = name.toLowerCase();
  return (await store.list('')).find((e) => e.kind === 'dir' && e.name.toLowerCase() === lower)?.name ?? null;
}

/**
 * Puts back files stranded by interrupted writes when a drive is opened: every category folder
 * under the profile's patch roots (one level, as the OP-1 reads them), and every folder of the
 * profile's backup roots at any depth (opus: mods/ and its plugin folders). Returns the restored
 * paths. Does nothing on a read-only store.
 */
export async function recoverDrive(store: DeviceStore, profile: DeviceProfile): Promise<string[]> {
  if (!store.capabilities.write) return [];
  const restored: string[] = [];
  for (const root of profile.layout.patchRoots) {
    for (const entry of await store.list(root.path)) {
      if (entry.kind === 'dir' && !entry.name.startsWith('.')) {
        restored.push(...(await recoverStranded(store, joinPath(root.path, entry.name))));
      }
    }
  }
  for (const name of profile.layout.backupOtherRoots ?? []) {
    const folder = await topFolder(store, name);
    if (folder) restored.push(...(await recoverTree(store, folder)));
  }
  return restored;
}
