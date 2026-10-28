import { describe, it, expect } from 'vitest';
import { readExtended, writeExtended, parseComm, buildComm, CommError, type CommInfo } from './comm';
import { IffError } from './iff';

// Sample rates as 80-bit IEEE 754 extended floats. OP-1 files carry 44100 Hz for the
// most part, but real devices also write 22050 Hz (the synth.aif fixture does).
const RATE_44100 = [0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
const RATE_22050 = [0x40, 0x0d, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

describe('extended floats', () => {
  it('reads 44100 and 22050', () => {
    expect(readExtended(new DataView(new Uint8Array(RATE_44100).buffer), 0)).toBe(44100);
    expect(readExtended(new DataView(new Uint8Array(RATE_22050).buffer), 0)).toBe(22050);
  });

  it('writes 44100 and 22050', () => {
    for (const [rate, expected] of [[44100, RATE_44100], [22050, RATE_22050]] as const) {
      const bytes = new Uint8Array(10);
      writeExtended(new DataView(bytes.buffer), 0, rate);
      expect(Array.from(bytes)).toEqual(expected);
    }
  });

  it('always sets the explicit integer bit of the mantissa', () => {
    // log2 of this rounds up to exactly 3, so a naive exponent leaves the mantissa below 2^63.
    const value = 8 - 2 ** -50;
    const bytes = new Uint8Array(10);
    writeExtended(new DataView(bytes.buffer), 0, value);
    expect(bytes[2]! & 0x80).toBe(0x80);
    expect(readExtended(new DataView(bytes.buffer), 0)).toBe(value);
  });

  it('rejects non-finite values', () => {
    const view = new DataView(new Uint8Array(10).buffer);
    expect(() => writeExtended(view, 0, NaN)).toThrow(RangeError);
    expect(() => writeExtended(view, 0, Infinity)).toThrow(RangeError);
  });

  it('round-trips other rates and zero', () => {
    for (const rate of [0, 8000, 22050, 48000, 96000]) {
      const bytes = new Uint8Array(10);
      writeExtended(new DataView(bytes.buffer), 0, rate);
      expect(readExtended(new DataView(bytes.buffer), 0)).toBe(rate);
    }
  });
});

describe('COMM', () => {
  const info: CommInfo = {
    channels: 1,
    frames: 44100,
    bitsPerSample: 16,
    sampleRate: 44100,
    compression: 'NONE',
    compressionName: '',
  };

  it('builds and parses a plain AIFF COMM chunk of 18 bytes', () => {
    const chunk = buildComm(info, 'AIFF');
    expect(chunk.id).toBe('COMM');
    expect(chunk.data.length).toBe(18);
    expect(parseComm(chunk, 'AIFF')).toEqual(info);
  });

  it('builds and parses an AIFC COMM chunk with a sowt compression type', () => {
    const sowt: CommInfo = { ...info, compression: 'sowt', compressionName: 'not compressed' };
    const chunk = buildComm(sowt, 'AIFC');
    // 18 + 4 (type) + 1 (length byte) + 14 (name) + 1 (pad to even) = 38
    expect(chunk.data.length).toBe(38);
    expect(parseComm(chunk, 'AIFC')).toEqual(sowt);
  });

  it('rejects a COMM chunk that is too short', () => {
    expect(() => parseComm({ id: 'COMM', data: new Uint8Array(10) }, 'AIFF')).toThrow(/too short/);
    expect(() => parseComm({ id: 'COMM', data: new Uint8Array(10) }, 'AIFF')).toThrow(CommError);
    expect(new CommError('x').name).toBe('CommError');
  });

  it('rejects a compression name longer than 255 bytes', () => {
    const long: CommInfo = { ...info, compression: 'NONE', compressionName: 'x'.repeat(256) };
    expect(() => buildComm(long, 'AIFC')).toThrow(CommError);
  });

  it('rejects a compression type that is not four latin1 characters', () => {
    expect(() => buildComm({ ...info, compression: 'NO' }, 'AIFC')).toThrow(IffError);
  });
});
