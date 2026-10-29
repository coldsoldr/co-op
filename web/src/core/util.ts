/** Small helpers shared across core and ui. */

/** An error's message, or the value as text when something other than an Error was thrown. */
export const messageOf = (error: unknown): string => (error instanceof Error ? error.message : String(error));

/** Compares strings by UTF-16 code unit, the order the stores and backups list paths in. */
export const byCodePoint = (a: string, b: string): number => (a < b ? -1 : a > b ? 1 : 0);

/** Compares directory entries by name, by code point. */
export const byEntryName = (a: { name: string }, b: { name: string }): number => byCodePoint(a.name, b.name);

export function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/** Lower-case hex SHA-256 of the bytes. */
export async function sha256Hex(bytes: Uint8Array | ArrayBuffer): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes as Uint8Array<ArrayBuffer>));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
}
