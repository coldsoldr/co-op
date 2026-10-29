import { describe, it, expect } from 'vitest';
import {
  HEADER,
  MAX_MESSAGE,
  SysexAssembler,
  buildMessage,
  crc32,
  pack7,
  parseReply,
  readU32le,
  statusName,
  u32le,
  unpack7,
} from './framing';

/** A reply as the OP-1 would build it: cmd | 0x40, the seq, then status and payload packed. */
const reply = (cmd: number, seq: number, raw: number[]): Uint8Array =>
  Uint8Array.from([...HEADER, cmd | 0x40, seq & 0x7f, seq >> 7, ...pack7(Uint8Array.from(raw)), 0xf7]);

describe('pack7 and unpack7', () => {
  it('packs a group of seven with the high bits first', () => {
    // High bits of bytes 0, 2 and 6 are set: 0b1000101.
    const raw = Uint8Array.from([0x80, 0x01, 0xff, 0x02, 0x03, 0x04, 0x85]);
    expect([...pack7(raw)]).toEqual([0x45, 0x00, 0x01, 0x7f, 0x02, 0x03, 0x04, 0x05]);
  });

  it('packs a short last group', () => {
    const raw = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 0x88, 9]);
    expect([...pack7(raw)]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 0x01, 0x08, 9]);
  });

  it('round-trips every byte value', () => {
    const raw = Uint8Array.from({ length: 256 }, (_, i) => i);
    const packed = pack7(raw);
    expect(packed.every((b) => b < 0x80)).toBe(true);
    expect([...unpack7(packed)]).toEqual([...raw]);
  });

  it('handles nothing', () => {
    expect(pack7(new Uint8Array(0))).toHaveLength(0);
    expect(unpack7(new Uint8Array(0))).toHaveLength(0);
  });
});

describe('buildMessage', () => {
  it('frames a command with its seq split into two 7-bit bytes', () => {
    expect([...buildMessage(7, 0x1234, Uint8Array.from([0x40, 0, 0, 0]))]).toEqual([
      0xf0, 0x00, 0x20, 0x76, 0x7d, 7, 0x34, 0x24, 0x00, 0x40, 0, 0, 0, 0xf7,
    ]);
  });

  it('sends HELLO with no payload', () => {
    expect([...buildMessage(1, 1)]).toEqual([0xf0, 0x00, 0x20, 0x76, 0x7d, 1, 1, 0, 0xf7]);
  });

  it('allows exactly 96 bytes (76 raw payload bytes) and refuses more', () => {
    expect(buildMessage(3, 5, new Uint8Array(76))).toHaveLength(MAX_MESSAGE);
    expect(() => buildMessage(3, 5, new Uint8Array(77))).toThrow(/longer than the OP-1 takes/);
  });
});

describe('parseReply', () => {
  it('reads cmd, seq, status and payload', () => {
    const parsed = parseReply(reply(1, 0x2abc, [0, 1, 0, 0, 0, 3]));
    expect(parsed).toEqual({ cmd: 1, seq: 0x2abc, status: 0, payload: Uint8Array.from([1, 0, 0, 0, 3]) });
  });

  it('round-trips a high-bit payload', () => {
    expect(parseReply(reply(7, 9, [5, 0xff, 0x80]))?.payload).toEqual(Uint8Array.from([0xff, 0x80]));
  });

  it('ignores requests, other manufacturers, cut-off messages and replies without a status', () => {
    expect(parseReply(buildMessage(1, 1, Uint8Array.from([0])))).toBeNull();
    const other = reply(1, 1, [0]);
    other[4] = 0x7e;
    expect(parseReply(other)).toBeNull();
    expect(parseReply(reply(1, 1, [0]).subarray(0, 10))).toBeNull();
    expect(parseReply(Uint8Array.from([...HEADER, 0x41, 1, 0, 0xf7]))).toBeNull();
    expect(parseReply(Uint8Array.from([0x90, 60, 100]))).toBeNull();
  });
});

describe('SysexAssembler', () => {
  it('joins a message split across runs and skips realtime bytes inside it', () => {
    const whole = reply(1, 3, [0, 1, 0, 0, 0]);
    const assembler = new SysexAssembler();
    expect(assembler.push(Uint8Array.from([0xf8, ...whole.subarray(0, 4), 0xf8]))).toEqual([]);
    expect(assembler.push(Uint8Array.from([0xfe, ...whole.subarray(4, 9), 0xf8, ...whole.subarray(9)]))).toEqual([
      whole,
    ]);
  });

  it('returns several messages from one run and drops bytes outside SysEx', () => {
    const a = reply(1, 1, [0]);
    const b = reply(7, 2, [5]);
    expect(new SysexAssembler().push(Uint8Array.from([0x90, 60, 100, ...a, ...b]))).toEqual([a, b]);
  });

  it('starts over on a new F0 and drops a SysEx cut by another status byte', () => {
    const a = reply(1, 1, [0]);
    const assembler = new SysexAssembler();
    expect(assembler.push(Uint8Array.from([0xf0, 0x00, 0x20, ...a]))).toEqual([a]);
    expect(assembler.push(Uint8Array.from([0xf0, 0x00, 0x90, 60, 0xf7]))).toEqual([]);
  });
});

describe('helpers', () => {
  it('computes zlib CRC-32', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array(0))).toBe(0);
    const text = new TextEncoder().encode('hello world');
    expect(crc32(text.subarray(5), crc32(text.subarray(0, 5)))).toBe(crc32(text));
  });

  it('reads and writes little-endian u32', () => {
    expect(u32le(0xdeadbeef)).toEqual([0xef, 0xbe, 0xad, 0xde]);
    expect(readU32le(Uint8Array.from([0, 0xef, 0xbe, 0xad, 0xde]), 1)).toBe(0xdeadbeef);
    expect(() => readU32le(Uint8Array.from([1, 2, 3]))).toThrow(/too short/);
  });

  it('names statuses', () => {
    expect(statusName(5)).toBe('offset');
    expect(statusName(9)).toBe('wait key');
    expect(statusName(42)).toBe('status 42');
  });
});
