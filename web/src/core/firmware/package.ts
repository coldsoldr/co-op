/**
 * The OP-1 firmware package (.op1) as op1repacker and TE write it: a CRC-32 of the rest in 4 bytes (little
 * endian), then LZMA "alone" data holding a GNU tar of the firmware folder. Parsing and rebuilding the tar
 * and checking a package the way firmware/tools/verify_bootloader.py does live here; the LZMA coding is
 * passed in (lzma.ts), so this file runs anywhere.
 */
import { crc32 } from '@co-op/shared/crc32';

/** op1repacker notes the OP-1 only takes firmware files up to about 15 MB. */
export const MAX_PACKAGE_BYTES = 15 * 1024 * 1024;
/** OP1_vdk.ldr is written raw from NAND block 4 to the end of the 128-block boot area (128 KB blocks). */
export const MAX_APP_BYTES = (128 - 4) * 128 * 1024;
export const APP_LDR = 'OP1_vdk.ldr';
export const BOOT_LDR = 'te-boot.ldr';

const BLOCK = 512;
/** GNU tar's record: 20 blocks. Archives end padded to a whole record. */
const RECORD = 20 * BLOCK;

export class PackageError extends Error {}

/** The package's body (after the CRC), checked against its CRC. */
export function unwrapCrc(file: Uint8Array): Uint8Array {
  if (file.length < 4 + 13) throw new PackageError('This is too short to be an OP-1 firmware file.');
  const stored = new DataView(file.buffer, file.byteOffset, 4).getUint32(0, true);
  const body = file.subarray(4);
  if (crc32(body) !== stored) throw new PackageError('Its checksum is wrong: this is not an OP-1 firmware file, or it is damaged.');
  return body;
}

/** The body with its CRC-32 in front. */
export function wrapCrc(body: Uint8Array): Uint8Array {
  const out = new Uint8Array(4 + body.length);
  new DataView(out.buffer).setUint32(0, crc32(body), true);
  out.set(body, 4);
  return out;
}

/** One tar entry: its 512-byte header and where its data sits in the archive. */
export interface TarEntry {
  name: string;
  /** '0' for a file, '5' for a folder. */
  type: string;
  header: Uint8Array;
  data: Uint8Array;
}

const ascii = (bytes: Uint8Array): string => {
  const end = bytes.indexOf(0);
  return String.fromCharCode(...(end < 0 ? bytes : bytes.subarray(0, end)));
};
const octal = (bytes: Uint8Array): number => parseInt(ascii(bytes).trim() || '0', 8);

function headerChecksum(header: Uint8Array): number {
  let sum = 0;
  for (let i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? 0x20 : (header[i] as number);
  return sum;
}

/** The entries of a tar, header checksums checked. Only plain files and folders, as in TE's packages. */
export function readTar(tar: Uint8Array): TarEntry[] {
  const entries: TarEntry[] = [];
  let at = 0;
  while (at + BLOCK <= tar.length) {
    const header = tar.subarray(at, at + BLOCK);
    if (header.every((b) => b === 0)) break;
    const name = ascii(header.subarray(0, 100));
    if (headerChecksum(header) !== octal(header.subarray(148, 156))) throw new PackageError(`The archive is damaged at ${name}.`);
    const type = String.fromCharCode(header[156] || 0x30);
    if (type !== '0' && type !== '5') throw new PackageError(`The archive has an entry of a kind the OP-1's packages never have (${name}).`);
    const size = octal(header.subarray(124, 136));
    const start = at + BLOCK;
    if (start + size > tar.length) throw new PackageError(`The archive ends inside ${name}.`);
    entries.push({ name, type, header, data: tar.subarray(start, start + size) });
    at = start + Math.ceil(size / BLOCK) * BLOCK;
  }
  return entries;
}

/** A header like `template` (same mode, owner, time and format) for a file of `size` bytes. */
function headerFor(template: Uint8Array, size: number): Uint8Array {
  const header = template.slice();
  const field = size.toString(8).padStart(11, '0');
  for (let i = 0; i < 11; i++) header[124 + i] = field.charCodeAt(i);
  header[135] = 0;
  const sum = headerChecksum(header).toString(8).padStart(6, '0');
  for (let i = 0; i < 6; i++) header[148 + i] = sum.charCodeAt(i);
  header[154] = 0;
  header[155] = 0x20;
  return header;
}

/**
 * A tar of `entries` in their order, each with its own header; `replace` gives new data for a file
 * by name (its header's size and checksum are rewritten), and a file in `drop` is left out. Ends with
 * zero blocks to a whole record, as GNU tar does.
 */
export function writeTar(entries: readonly TarEntry[], replace: ReadonlyMap<string, Uint8Array>, drop: ReadonlySet<string>): Uint8Array {
  const parts: Uint8Array[] = [];
  let length = 0;
  for (const entry of entries) {
    if (drop.has(entry.name)) continue;
    const data = replace.get(entry.name) ?? entry.data;
    const header = replace.has(entry.name) ? headerFor(entry.header, data.length) : entry.header;
    parts.push(header, data);
    length += BLOCK + Math.ceil(data.length / BLOCK) * BLOCK;
  }
  const total = Math.ceil((length + 2 * BLOCK) / RECORD) * RECORD;
  const out = new Uint8Array(total);
  let at = 0;
  for (let i = 0; i < parts.length; i += 2) {
    const header = parts[i] as Uint8Array;
    const data = parts[i + 1] as Uint8Array;
    out.set(header, at);
    out.set(data, at + BLOCK);
    at += BLOCK + Math.ceil(data.length / BLOCK) * BLOCK;
  }
  return out;
}

/** Checks an LDR stream as firmware/tools/ldr_dump.py parses it; returns the number of DXEs. */
export function checkLdr(ldr: Uint8Array): number {
  const view = new DataView(ldr.buffer, ldr.byteOffset, ldr.byteLength);
  let at = 0;
  let dxes = 0;
  let nextDxe: number | null = null;
  while (at < ldr.length) {
    if (at + 16 > ldr.length) throw new PackageError(`OP1_vdk.ldr ends inside a block header at ${at}.`);
    const code = view.getUint32(at, true);
    const count = view.getUint32(at + 8, true);
    const arg = view.getUint32(at + 12, true);
    if (code >>> 24 !== 0xad) throw new PackageError(`OP1_vdk.ldr has a bad block signature at ${at}.`);
    let xor = 0;
    for (let i = 0; i < 16; i++) xor ^= ldr[at + i] as number;
    if (xor !== 0) throw new PackageError(`OP1_vdk.ldr has a bad block header checksum at ${at}.`);
    const flags = code & 0xfff0;
    const payload = flags & 0x0100 ? 0 : count;
    if (at + 16 + payload > ldr.length) throw new PackageError(`OP1_vdk.ldr ends inside a block at ${at}.`);
    if (flags & 0x4000) {
      if (nextDxe !== null && nextDxe !== at) throw new PackageError(`OP1_vdk.ldr's DXE at ${at} is not where the previous one pointed.`);
      nextDxe = at + 16 + arg;
      dxes += 1;
    } else if (dxes === 0) {
      throw new PackageError("OP1_vdk.ldr's first block is not a FIRST block.");
    }
    at += 16 + payload;
  }
  if (nextDxe !== null && nextDxe !== ldr.length) throw new PackageError("OP1_vdk.ldr's last DXE does not end the file.");
  if (dxes === 0) throw new PackageError('OP1_vdk.ldr is empty.');
  return dxes;
}

/**
 * The problems firmware/tools/verify_bootloader.py would report for a coop package: over the size limit,
 * te-boot.ldr present (the bootloader must never be touched), a file name te-boot treats specially, or
 * OP1_vdk.ldr missing, too big for the boot area, or not a valid LDR stream. Empty when it is safe.
 */
export function packageProblems(file: Uint8Array, tar: Uint8Array, fileName: string): string[] {
  const problems: string[] = [];
  if (file.length > MAX_PACKAGE_BYTES) problems.push(`The package is ${file.length} bytes, over the OP-1's ${MAX_PACKAGE_BYTES}.`);
  try {
    unwrapCrc(file);
  } catch (e) {
    problems.push((e as Error).message);
  }
  const upper = fileName.toUpperCase();
  if (upper.includes('FRC')) problems.push('The file name contains "FRC", which makes te-boot rewrite the bootloader.');
  if (upper.endsWith('.KEY')) problems.push('.KEY files make te-boot program signature keys.');
  let entries: TarEntry[];
  try {
    entries = readTar(tar);
  } catch (e) {
    problems.push((e as Error).message);
    return problems;
  }
  if (entries.some((e) => e.name === BOOT_LDR)) problems.push('The package has te-boot.ldr; a coop package must never touch the bootloader.');
  const app = entries.find((e) => e.name === APP_LDR);
  if (!app) {
    problems.push('The package has no OP1_vdk.ldr.');
  } else {
    if (app.data.length > MAX_APP_BYTES) problems.push(`OP1_vdk.ldr is ${app.data.length} bytes, over the ${MAX_APP_BYTES}-byte boot area.`);
    try {
      checkLdr(app.data);
    } catch (e) {
      problems.push((e as Error).message);
    }
  }
  return problems;
}
