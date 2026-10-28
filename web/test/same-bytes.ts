import { expect } from 'vitest';
import { firstDifference } from './first-difference';

/**
 * Asserts byte identity. Much faster than toEqual on large buffers (vitest compares typed arrays
 * element by element through its generic equality), and reports the first differing offset.
 */
export function expectSameBytes(actual: Uint8Array | undefined, expected: Uint8Array, label = 'bytes'): void {
  expect(actual, `${label}: missing`).toBeDefined();
  expect(firstDifference(actual as Uint8Array, expected), `${label}: first differing offset`).toBe(-1);
}

/** Asserts two path-to-bytes maps hold the same paths with identical bytes. */
export function expectSameFiles(actual: Record<string, Uint8Array>, expected: Record<string, Uint8Array>): void {
  expect(Object.keys(actual).sort()).toEqual(Object.keys(expected).sort());
  for (const [path, bytes] of Object.entries(expected)) expectSameBytes(actual[path], bytes, path);
}
