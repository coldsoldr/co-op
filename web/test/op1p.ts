import { crc32 } from '@co-op/shared/crc32';

/**
 * A small valid op1r plugin (.op1p) for tests: the binary's 76-byte header (magic, format 1, API 1, kind, saved
 * name, a 4-byte code image, no relocations or imports), its metadata lines, then the 16-byte trailer with the
 * binary's CRC-32 (synth_sdk/plugload/op1p.h in op1r-src).
 */
export function op1pFile({ name, kind = 0, author, filler = 0 }: { name: string; kind?: number; author?: string; filler?: number }): Uint8Array {
  const meta = new TextEncoder().encode(author === undefined ? '' : `author=${author}\n`);
  const size = 76 + 4 + meta.length + filler;
  const binary = new Uint8Array(size + ((4 - (size % 4)) % 4));
  const view = new DataView(binary.buffer);
  view.setUint32(0, 0x5031504f, true);
  view.setUint32(4, 1, true);
  view.setUint32(8, 1, true);
  view.setUint32(12, kind, true);
  binary.set(new TextEncoder().encode(name), 16);
  view.setUint32(32, 4, true);
  binary.set(meta, 80);
  const file = new Uint8Array(binary.length + 16);
  file.set(binary, 0);
  const trailer = new DataView(file.buffer, binary.length);
  trailer.setUint32(0, 0x5431504f, true);
  trailer.setUint32(4, 0, true);
  trailer.setUint32(8, binary.length, true);
  trailer.setUint32(12, crc32(binary), true);
  return file;
}
