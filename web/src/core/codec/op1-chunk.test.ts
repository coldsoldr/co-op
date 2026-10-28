import { describe, it, expect } from 'vitest';
import { isOp1Chunk, parseOp1Chunk, buildOp1Chunk } from './op1-chunk';
import { SYNTH_META, DRUM_META } from '../../../test/fixtures/meta';

const text = (s: string) => new TextEncoder().encode(s);

describe('op-1 APPL chunk', () => {
  it('detects the op-1 signature', () => {
    expect(isOp1Chunk({ id: 'APPL', data: text('op-1{}') })).toBe(true);
    expect(isOp1Chunk({ id: 'APPL', data: text('abcd{}') })).toBe(false);
    expect(isOp1Chunk({ id: 'SSND', data: text('op-1{}') })).toBe(false);
  });

  it('parses JSON and keeps the trailing filler bytes', () => {
    const chunk = { id: 'APPL', data: text('op-1{"name":"a}b","octave":1}\n\x1e') };
    const parsed = parseOp1Chunk(chunk);
    expect(parsed.meta).toEqual({ name: 'a}b', octave: 1 });
    expect(Array.from(parsed.trailing)).toEqual([0x0a, 0x1e]);
  });

  it('builds a chunk with sorted keys, a newline filler and an even length', () => {
    const chunk = buildOp1Chunk({ octave: 0, name: 'zz' });
    const body = new TextDecoder().decode(chunk.data);
    expect(body.startsWith('op-1{"name":"zz","octave":0}')).toBe(true);
    expect(chunk.data.length % 2).toBe(0);
  });

  it('round-trips real synth and drum metadata byte for byte', () => {
    for (const meta of [SYNTH_META, DRUM_META]) {
      const built = buildOp1Chunk(meta);
      const parsed = parseOp1Chunk(built);
      expect(parsed.meta).toEqual(meta);
      expect(Array.from(buildOp1Chunk(parsed.meta, parsed.trailing))).toEqual(Array.from(built));
    }
  });

  it('rejects a chunk without a JSON object', () => {
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1nothing') })).toThrow(/no JSON/);
  });
});
