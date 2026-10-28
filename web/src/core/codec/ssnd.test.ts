import { describe, it, expect } from 'vitest';
import { buildSsnd, decodeSsnd } from './ssnd';
import type { CommInfo } from './comm';

const comm = (compression: string, channels = 1): CommInfo => ({
  channels,
  frames: 4,
  bitsPerSample: 16,
  sampleRate: 44100,
  compression,
  compressionName: '',
});

describe('SSND', () => {
  const samples = Int16Array.from([0, 16384, -32768, 32767]);

  it('encodes big-endian with an 8-byte header', () => {
    const chunk = buildSsnd(samples, false);
    expect(chunk.id).toBe('SSND');
    expect(Array.from(chunk.data.subarray(0, 8))).toEqual([0, 0, 0, 0, 0, 0, 0, 0]);
    expect(Array.from(chunk.data.subarray(8, 12))).toEqual([0x00, 0x00, 0x40, 0x00]);
  });

  it('encodes little-endian when asked', () => {
    const chunk = buildSsnd(samples, true);
    expect(Array.from(chunk.data.subarray(8, 12))).toEqual([0x00, 0x00, 0x00, 0x40]);
  });

  it('decodes big-endian AIFF to floats in [-1, 1)', () => {
    const out = decodeSsnd(buildSsnd(samples, false), comm('NONE'));
    expect(Array.from(out)).toEqual([0, 0.5, -1, 32767 / 32768]);
  });

  it('decodes sowt little-endian AIFC', () => {
    const out = decodeSsnd(buildSsnd(samples, true), comm('sowt'));
    expect(Array.from(out)).toEqual([0, 0.5, -1, 32767 / 32768]);
  });

  it('mixes stereo down to mono', () => {
    const stereo = Int16Array.from([16384, -16384, 32767, 32767]);
    const out = decodeSsnd(buildSsnd(stereo, false), comm('NONE', 2));
    expect(Array.from(out)).toEqual([0, 32767 / 32768]);
  });

  it('rejects unsupported bit depths and compression types', () => {
    const chunk = buildSsnd(samples, false);
    expect(() => decodeSsnd(chunk, { ...comm('NONE'), bitsPerSample: 24 })).toThrow(/bit depth/);
    expect(() => decodeSsnd(chunk, comm('ulaw'))).toThrow(/compression/);
  });
});
