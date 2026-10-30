/**
 * The SysEx framing hundekraft 3.x answers on the OP-1's USB MIDI port (the "kennel" update
 * protocol; see docs/research/2026-10-29-hundekraft-usb-and-plugins.md). Every message, both ways:
 * `F0 00 20 76 7D <cmd> <seq lo> <seq hi> <7-bit packed payload> F7`. A reply carries `cmd | 0x40`,
 * the request's seq, and a payload whose first unpacked byte is a status.
 */

/** Start of every message: SysEx start and the manufacturer id. */
export const HEADER: readonly number[] = [0xf0, 0x00, 0x20, 0x76, 0x7d];

const SYSEX_START = 0xf0;
const SYSEX_END = 0xf7;
/** Bytes from 0xF8 up are MIDI realtime (clock and the like) and may arrive inside a SysEx. */
const REALTIME_FIRST = 0xf8;
const REPLY_FLAG = 0x40;

/**
 * The longest message either side may send. The OP-1's USB MIDI input takes at most 32 USB MIDI
 * packets per transfer, and a longer SysEx stops all MIDI input until it restarts.
 */
export const MAX_MESSAGE = 96;

/** Sequence numbers are 14 bits, sent as two 7-bit bytes. */
export const SEQ_MASK = 0x3fff;

/**
 * The commands co-op sends: the reads, and the ones that stage and commit plugin files. co-op never
 * sends OFF_SET (9). DELETE (10) and RESTART (11) only ever remove a plugin the person chose.
 */
export const Command = {
  HELLO: 1,
  BEGIN: 2,
  DATA: 3,
  END: 4,
  COMMIT: 5,
  ABORT: 6,
  LIST: 7,
  OFF_GET: 8,
  DELETE: 10,
  RESTART: 11,
} as const;

/** The most raw file bytes one DATA message carries: u32 offset + 72 bytes packs to a 96-byte message. */
export const MAX_DATA = 72;

/** Reply statuses. */
export const Status = {
  OK: 0,
  FORMAT: 1,
  STATE: 2,
  PATH: 3,
  SIZE: 4,
  OFFSET: 5,
  CRC: 6,
  IO: 7,
  FULL: 8,
  WAIT_KEY: 9,
} as const;

const STATUS_NAMES = ['ok', 'format', 'state', 'path', 'size', 'offset', 'crc', 'io', 'full', 'wait key'];

/** A status as a word for messages ("offset"), or its number when unknown. */
export const statusName = (status: number): string => STATUS_NAMES[status] ?? `status ${status}`;

/**
 * 7-bit packing: each group of up to 7 raw bytes becomes one byte holding their high bits (bit k is
 * the high bit of byte k) followed by the bytes with their high bit cleared.
 */
export function pack7(raw: Uint8Array): Uint8Array {
  const out = new Uint8Array(raw.length + Math.ceil(raw.length / 7));
  let o = 0;
  for (let start = 0; start < raw.length; start += 7) {
    const end = Math.min(start + 7, raw.length);
    let high = 0;
    for (let i = start; i < end; i++) high |= ((raw[i] as number) >> 7) << (i - start);
    out[o++] = high;
    for (let i = start; i < end; i++) out[o++] = (raw[i] as number) & 0x7f;
  }
  return out;
}

/** Reverses pack7. A trailing group may be short. */
export function unpack7(packed: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < packed.length; ) {
    const high = packed[i++] as number;
    for (let k = 0; k < 7 && i < packed.length; k++) out.push((packed[i++] as number) | (((high >> k) & 1) << 7));
  }
  return Uint8Array.from(out);
}

/** A whole message for the OP-1. Throws when it would be longer than MAX_MESSAGE. */
export function buildMessage(cmd: number, seq: number, payload: Uint8Array = new Uint8Array(0)): Uint8Array {
  const packed = pack7(payload);
  const length = HEADER.length + 3 + packed.length + 1;
  if (length > MAX_MESSAGE) throw new Error(`a ${length}-byte message is longer than the OP-1 takes (${MAX_MESSAGE})`);
  const out = new Uint8Array(length);
  out.set(HEADER);
  out[5] = cmd & 0x7f;
  out[6] = seq & 0x7f;
  out[7] = (seq >> 7) & 0x7f;
  out.set(packed, 8);
  out[length - 1] = SYSEX_END;
  return out;
}

export interface Reply {
  /** The command answered, without the reply flag. */
  cmd: number;
  seq: number;
  status: number;
  /** The unpacked payload after the status byte. */
  payload: Uint8Array;
}

/** A reply from the OP-1, or null when the bytes are not one of ours or not a reply. */
export function parseReply(bytes: Uint8Array): Reply | null {
  if (bytes.length < HEADER.length + 4 || bytes[bytes.length - 1] !== SYSEX_END) return null;
  if (HEADER.some((b, i) => bytes[i] !== b)) return null;
  const cmd = bytes[5] as number;
  if (!(cmd & REPLY_FLAG)) return null;
  const body = unpack7(bytes.subarray(8, bytes.length - 1));
  if (body.length === 0) return null;
  return {
    cmd: cmd & ~REPLY_FLAG & 0x7f,
    seq: (bytes[6] as number) | ((bytes[7] as number) << 7),
    status: body[0] as number,
    payload: body.subarray(1),
  };
}

/**
 * Puts SysEx messages back together from the byte runs a MIDI input hands over, which may split a
 * message or carry realtime bytes inside it. Bytes outside a SysEx are dropped.
 */
export class SysexAssembler {
  private buffer: number[] | null = null;

  /** Feeds a run of bytes; returns every message it completed, F0 to F7. */
  push(bytes: Uint8Array): Uint8Array[] {
    const done: Uint8Array[] = [];
    for (const b of bytes) {
      if (b >= REALTIME_FIRST) continue;
      if (b === SYSEX_START) {
        this.buffer = [b];
      } else if (this.buffer) {
        this.buffer.push(b);
        if (b === SYSEX_END) {
          done.push(Uint8Array.from(this.buffer));
          this.buffer = null;
        } else if (b & 0x80 || this.buffer.length >= MAX_MESSAGE) {
          // Any other status byte ends a SysEx without completing it. Nothing longer than
          // MAX_MESSAGE is ours, so a SysEx that grows past it is dropped rather than buffered.
          this.buffer = null;
        }
      }
    }
    return done;
  }
}

/** zlib's CRC-32, shared with the plugin validator. */
export { crc32 } from '@co-op/shared/crc32';

/** Little-endian u32 as four bytes. */
export const u32le = (value: number): number[] => [
  value & 0xff,
  (value >>> 8) & 0xff,
  (value >>> 16) & 0xff,
  (value >>> 24) & 0xff,
];

/** Reads a little-endian u32 at `offset`. Throws when the bytes are too short. */
export function readU32le(bytes: Uint8Array, offset = 0): number {
  if (bytes.length < offset + 4) throw new Error('the OP-1 sent a reply that is too short');
  return (
    ((bytes[offset] as number) |
      ((bytes[offset + 1] as number) << 8) |
      ((bytes[offset + 2] as number) << 16) |
      ((bytes[offset + 3] as number) << 24)) >>>
    0
  );
}
