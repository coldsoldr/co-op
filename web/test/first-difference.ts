/** First offset where the two buffers differ, or -1. A length mismatch counts as a difference at the shorter length. */
export function firstDifference(a: Uint8Array, b: Uint8Array): number {
  const shorter = Math.min(a.length, b.length);
  for (let i = 0; i < shorter; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : shorter;
}
