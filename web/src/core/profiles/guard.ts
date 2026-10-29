import type { DeviceProfile } from './types';
import { readPatch } from '../codec/patch';

/** A file about to be written that the profile marks unsupported. */
export interface UnsupportedPatch {
  /** A stable id when the path may repeat (op1.fun patch names); list keys use it. */
  id?: string;
  path: string;
  reason: string;
}

/**
 * The files among `files` that `profile` judges unsupported, in order, for the write guard.
 * Files that do not parse as patches at all are skipped: the guard is about patches the
 * device would try to load, and the writers check readability themselves.
 */
export function findUnsupported(
  files: Iterable<{ id?: string; path: string; bytes: Uint8Array }>,
  profile: DeviceProfile,
): UnsupportedPatch[] {
  const found: UnsupportedPatch[] = [];
  for (const { id, path, bytes } of files) {
    let compat;
    try {
      compat = profile.compatibility(readPatch(bytes));
    } catch {
      continue;
    }
    if (compat.level === 'unsupported') {
      found.push({ ...(id !== undefined ? { id } : {}), path, reason: compat.reason ?? 'unsupported' });
    }
  }
  return found;
}
