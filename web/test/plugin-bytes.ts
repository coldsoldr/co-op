/**
 * Small hand-built plugin files for tests: a valid .knl, .op1p and .opmod, and a PNG header with
 * a given size. The layouts are the ones in docs/research/2026-10-29-hundekraft-usb-and-plugins.md.
 */
import { crc32 } from '@co-op/shared/crc32';

const ascii = (text: string): Uint8Array => Uint8Array.from(text, (c) => c.charCodeAt(0));
const view = (bytes: Uint8Array): DataView => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

export interface KnlOptions {
  /** 1 synth, 2 effect, 3 feature. */
  kind?: number;
  /** The saved name, up to 15 characters. */
  name?: string;
  /** Author and version for the extension table; without either there is no table. */
  author?: string;
  version?: string;
}

/** A valid .knl: the 144-byte header, 16 bytes of body and an optional extension table, with its CRC. */
export function knlBytes({ kind = 2, name = 'tape drive', author, version }: KnlOptions = {}): Uint8Array {
  const entries: [number, Uint8Array][] = [];
  if (author !== undefined) entries.push([2, ascii(`${author}\0`)]);
  if (version !== undefined) entries.push([3, ascii(`${version}\0`)]);
  const pad = (n: number): number => (n + 3) & ~3;
  const extAt = 144 + 16;
  const extSize = entries.length === 0 ? 0 : entries.reduce((sum, [, data]) => sum + 4 + pad(data.length), 0) + 4;
  const bytes = new Uint8Array(extAt + extSize);
  const v = view(bytes);
  bytes.set(ascii('KNPL'), 0);
  v.setUint16(4, 1, true);
  v.setUint16(6, 144, true);
  v.setUint32(8, bytes.length, true);
  v.setUint16(16, 1, true);
  bytes[18] = kind;
  bytes.set(ascii(name), 20);
  v.setUint32(136, 0xffffffff, true);
  if (entries.length > 0) {
    v.setUint32(140, extAt, true);
    let at = extAt;
    for (const [tag, data] of entries) {
      v.setUint16(at, tag, true);
      v.setUint16(at + 2, data.length, true);
      bytes.set(data, at + 4);
      at += 4 + pad(data.length);
    }
  }
  v.setUint32(12, crc32(bytes), true);
  return bytes;
}

/** A valid .opmod as far as anything is known about it: the OPMD magic. */
export const opmodBytes = (): Uint8Array => ascii('OPMD\x01\x02\x03\x04');

/** A PNG signature and IHDR chunk for a `width` x `height` image; enough for the screenshot check. */
export function pngBytes(width = 320, height = 240): Uint8Array {
  const bytes = new Uint8Array(33);
  const v = view(bytes);
  bytes.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a], 0);
  v.setUint32(8, 13);
  bytes.set(ascii('IHDR'), 12);
  v.setUint32(16, width);
  v.setUint32(20, height);
  bytes.set([8, 6, 0, 0, 0], 24);
  return bytes;
}
