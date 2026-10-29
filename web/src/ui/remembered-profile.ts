/**
 * The firmware profile last chosen by hand, kept in localStorage so the next connect or drop
 * starts on it. Folder detection cannot tell a hundekraft drive from a stock one, so without this
 * the choice would have to be made again every visit. Only a choice made by the user is stored.
 */
import { getProfile } from '../core/profiles/registry';

export const STORAGE_KEY = 'co-op.profile';

/**
 * Folder detection at or above this confidence beats the remembered profile: it proves the
 * firmware on the drive (opus by its mods folder), where a remembered choice only recalls it.
 */
export const OVERRIDE_CONFIDENCE = 0.97;

/** The subset of Storage used here, so tests can pass a fake. */
export type KeyValueStore = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

function defaultStorage(): KeyValueStore | null {
  try {
    return typeof localStorage === 'undefined' ? null : localStorage;
  } catch {
    // Access can throw when site data is blocked.
    return null;
  }
}

/** The remembered profile id, or null when there is none, it is unreadable or no longer exists. */
export function loadRememberedProfile(storage: KeyValueStore | null = defaultStorage()): string | null {
  try {
    const raw = storage?.getItem(STORAGE_KEY) ?? null;
    if (raw === null) return null;
    const value = JSON.parse(raw) as unknown;
    if (typeof value !== 'object' || value === null) return null;
    const { id } = value as Record<string, unknown>;
    return typeof id === 'string' && getProfile(id) ? id : null;
  } catch {
    return null;
  }
}

/** Remembers the chosen profile id, or forgets it with null. A refusing browser is ignored. */
export function rememberProfile(id: string | null, storage: KeyValueStore | null = defaultStorage()): void {
  try {
    if (id === null) storage?.removeItem(STORAGE_KEY);
    else storage?.setItem(STORAGE_KEY, JSON.stringify({ id, at: new Date().toISOString() }));
  } catch {
    // Nothing more can be done; the choice holds for this page only.
  }
}
