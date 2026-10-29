import { describe, it, expect } from 'vitest';
import { crc32 } from './crc32';

const ascii = (text: string): Uint8Array => Uint8Array.from(text, (c) => c.charCodeAt(0));

describe('crc32', () => {
  it('computes zlib CRC-32', () => {
    expect(crc32(ascii('123456789'))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array(0))).toBe(0);
  });

  it('continues over more bytes', () => {
    const text = ascii('hello world');
    expect(crc32(text.subarray(5), crc32(text.subarray(0, 5)))).toBe(crc32(text));
  });
});
