import { writeId, type Chunk } from './iff';

export interface CommInfo {
  channels: number;
  frames: number;
  bitsPerSample: number;
  sampleRate: number;
  /** AIFC compression type. 'NONE' for plain AIFF. 'sowt' means little-endian PCM. */
  compression: string;
  compressionName: string;
}

const latin1 = new TextDecoder('latin1');
const encoder = new TextEncoder();

export class CommError extends Error {
  override name = 'CommError';
}

/** Reads an 80-bit IEEE 754 extended float (sign, 15-bit exponent, 64-bit mantissa with explicit integer bit). */
export function readExtended(view: DataView, offset: number): number {
  const signAndExponent = view.getUint16(offset);
  const sign = signAndExponent & 0x8000 ? -1 : 1;
  const exponent = signAndExponent & 0x7fff;
  const hi = view.getUint32(offset + 2);
  const lo = view.getUint32(offset + 6);
  if (exponent === 0 && hi === 0 && lo === 0) return 0;
  const mantissa = hi * 2 ** 32 + lo;
  return sign * mantissa * 2 ** (exponent - 16383 - 63);
}

export function writeExtended(view: DataView, offset: number, value: number): void {
  if (!Number.isFinite(value)) throw new RangeError(`cannot encode ${value} as an extended float`);
  if (value === 0) {
    for (let i = 0; i < 10; i++) view.setUint8(offset + i, 0);
    return;
  }
  const sign = value < 0 ? 0x8000 : 0;
  const abs = Math.abs(value);
  let exponent = Math.floor(Math.log2(abs));
  // abs / 2^exponent should be in [1, 2), so the mantissa lands in [2^63, 2^64). Math.log2 can
  // round up for values just below a power of two, which would clear the explicit integer bit.
  let mantissa = abs * 2 ** (63 - exponent);
  if (mantissa < 2 ** 63) {
    exponent -= 1;
    mantissa *= 2;
  }
  const hi = Math.floor(mantissa / 2 ** 32);
  const lo = Math.floor(mantissa - hi * 2 ** 32);
  view.setUint16(offset, sign | (exponent + 16383));
  view.setUint32(offset + 2, hi);
  view.setUint32(offset + 6, lo);
}

export function parseComm(chunk: Chunk, formType: string): CommInfo {
  const d = chunk.data;
  if (d.length < 18) throw new CommError('COMM chunk too short');
  const view = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const info: CommInfo = {
    channels: view.getInt16(0),
    frames: view.getUint32(2),
    bitsPerSample: view.getInt16(6),
    sampleRate: readExtended(view, 8),
    compression: 'NONE',
    compressionName: '',
  };
  if (formType === 'AIFC' && d.length >= 22) {
    info.compression = latin1.decode(d.subarray(18, 22));
    const nameLength = d[22] ?? 0;
    info.compressionName = latin1.decode(d.subarray(23, 23 + nameLength));
  }
  return info;
}

export function buildComm(info: CommInfo, formType: string): Chunk {
  const name = encoder.encode(info.compressionName);
  if (name.length > 255) throw new CommError('compression name exceeds 255 bytes');
  // AIFC appends a 4-char compression type and a Pascal string padded to an even length.
  const extra = formType === 'AIFC' ? 4 + 1 + name.length + ((1 + name.length) % 2) : 0;
  const data = new Uint8Array(18 + extra);
  const view = new DataView(data.buffer);
  view.setInt16(0, info.channels);
  view.setUint32(2, info.frames);
  view.setInt16(6, info.bitsPerSample);
  writeExtended(view, 8, info.sampleRate);
  if (formType === 'AIFC') {
    writeId(data, 18, info.compression);
    data[22] = name.length;
    data.set(name, 23);
  }
  return { id: 'COMM', data };
}
