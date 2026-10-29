/**
 * Where the op1.fun email and token live: in memory for this page by default, and in localStorage
 * only when the user ticks "Remember on this device". Forgetting clears both. Nothing here sends
 * them anywhere; the client sends them only to api.op1.fun and the co-op Worker's upload route.
 */
import type { Op1FunCredentials } from './types';

export const STORAGE_KEY = 'co-op.op1fun';

export interface SavedCredentials extends Op1FunCredentials {
  /** The op1.fun username last browsed or tested, for convenience; '' when none. */
  username: string;
  /** True when the credentials are also in localStorage. */
  remember: boolean;
}

/** The subset of Storage used here, so tests can pass a fake. */
export type KeyValueStore = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

let memory: SavedCredentials | null = null;

function defaultStorage(): KeyValueStore | null {
  try {
    return typeof localStorage === 'undefined' ? null : localStorage;
  } catch {
    // Access can throw when site data is blocked.
    return null;
  }
}

function parse(raw: string | null): SavedCredentials | null {
  if (raw === null) return null;
  try {
    const value = JSON.parse(raw) as unknown;
    if (typeof value !== 'object' || value === null) return null;
    const { email, token, username } = value as Record<string, unknown>;
    if (typeof email !== 'string' || typeof token !== 'string' || email === '' || token === '') return null;
    return { email, token, username: typeof username === 'string' ? username : '', remember: true };
  } catch {
    return null;
  }
}

/** The credentials in memory, else the remembered ones, else null. */
export function loadCredentials(storage: KeyValueStore | null = defaultStorage()): SavedCredentials | null {
  if (memory) return { ...memory };
  try {
    const saved = parse(storage?.getItem(STORAGE_KEY) ?? null);
    if (saved) memory = saved;
    return saved ? { ...saved } : null;
  } catch {
    return null;
  }
}

/**
 * Keeps the credentials in memory and, with `remember`, in localStorage; without it any
 * remembered copy is removed. Returns false when the browser refused to store them.
 */
export function saveCredentials(
  creds: Op1FunCredentials & { username?: string },
  remember: boolean,
  storage: KeyValueStore | null = defaultStorage(),
): boolean {
  const saved: SavedCredentials = { email: creds.email, token: creds.token, username: creds.username ?? '', remember };
  memory = saved;
  try {
    if (remember) {
      if (!storage) return false;
      storage.setItem(STORAGE_KEY, JSON.stringify({ email: saved.email, token: saved.token, username: saved.username }));
    } else {
      storage?.removeItem(STORAGE_KEY);
    }
    return true;
  } catch {
    if (remember) memory = { ...saved, remember: false };
    return !remember;
  }
}

/** Forgets the credentials in memory and in localStorage. */
export function clearCredentials(storage: KeyValueStore | null = defaultStorage()): void {
  memory = null;
  try {
    storage?.removeItem(STORAGE_KEY);
  } catch {
    // Nothing more can be done; the memory copy is gone.
  }
}
