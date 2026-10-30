import { joinPath, type DeviceStore } from '../store/store';

/** The OP-1's flash, counted in binary units as the OP-1 itself counts it. */
export const OP1_FLASH_BYTES = 512 * 1024 * 1024;

/**
 * The bytes in every file the drive shows, folders walked one at a time. A file whose size cannot
 * be read counts as empty: the result feeds an estimate, which is no reason to fail a listing.
 */
export async function driveUsedBytes(store: DeviceStore, folder = ''): Promise<number> {
  let total = 0;
  for (const entry of await store.list(folder)) {
    const path = joinPath(folder, entry.name);
    if (entry.kind === 'dir') total += await driveUsedBytes(store, path);
    else total += await store.size(path).catch(() => 0);
  }
  return total;
}

/**
 * The whole-number share of the flash the drive's files leave free, 0 to 100. A browser cannot ask
 * a drive for its free space, and the OP-1's system files do not show on it, so the true figure is
 * somewhat lower.
 */
export function freePercent(usedBytes: number, totalBytes = OP1_FLASH_BYTES): number {
  return Math.min(100, Math.max(0, Math.round((1 - usedBytes / totalBytes) * 100)));
}
