import { describe, it, expect } from 'vitest';
import { readExtended, writeExtended, parseComm, buildComm, type CommInfo } from './comm';

// 44100 Hz as an 80-bit IEEE 754 extended float, the value every OP-1 file carries.
const RATE_44100 = [0x40, 0x0e, 0xac, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

describe('extended floats', () => {
  it('reads 44100', () => {
    const view = new DataView(new Uint8Array(RATE_44100).buffer);
    expect(readExtended(view, 0)).toBe(44100);
  });

  it('writes 44100', () => {
    const bytes = new Uint8Array(10);
    writeExtended(new DataView(bytes.buffer), 0, 44100);
    expect(Array.from(bytes)).toEqual(RATE_44100);
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
  });
});
