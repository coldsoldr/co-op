import { describe, it, expect } from 'vitest';
import { isOp1Chunk, parseOp1Chunk, buildOp1Chunk, Op1ChunkError } from './op1-chunk';
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
    const json = '{"name":"zz","octave":0}';
    const body = new TextDecoder().decode(chunk.data);
    expect(body.startsWith('op-1' + json)).toBe(true);
    expect(chunk.data.length % 2).toBe(0);
    // 4 + 24 + newline is odd, so the device-style space pad byte follows the newline.
    expect(Array.from(chunk.data.subarray(4 + json.length))).toEqual([0x0a, 0x20]);
  });

  it('uses only a newline when the JSON already makes the length odd', () => {
    const chunk = buildOp1Chunk({ octave: 0, name: 'z' });
    const json = '{"name":"z","octave":0}';
    expect(Array.from(chunk.data.subarray(4 + json.length))).toEqual([0x0a]);
  });

  it('rebuilds the filler from scratch instead of carrying trailing bytes over', () => {
    const meta = { octave: 0, name: 'zz' }; // 4 + 24 + 1 is odd, so a pad byte is needed
    for (const trailing of [[0x0a, 0x20], [0x0a, 0x20, 0x20], [0x0a]]) {
      const chunk = buildOp1Chunk(meta, Uint8Array.from(trailing));
      expect(chunk.data.length % 2).toBe(0);
      expect(parseOp1Chunk(chunk).trailing.length).toBeLessThanOrEqual(2);
    }
  });

  it('keeps the pad byte read from the file when rebuilding', () => {
    const chunk = buildOp1Chunk({ octave: 0, name: 'zz' }, Uint8Array.from([0x0a, 0x1e]));
    expect(Array.from(chunk.data.subarray(chunk.data.length - 2))).toEqual([0x0a, 0x1e]);
  });

  it('never grows the filler across successive edits that flip parity', () => {
    let built = buildOp1Chunk({ octave: 0, name: 'a' });
    for (const name of ['bb', 'ccc', 'dddd']) {
      const parsed = parseOp1Chunk(built);
      built = buildOp1Chunk({ ...parsed.meta, name }, parsed.trailing);
      expect(built.data.length % 2).toBe(0);
      expect(parseOp1Chunk(built).trailing.length).toBeLessThanOrEqual(2);
    }
  });

  it('round-trips real synth and drum metadata byte for byte', () => {
    for (const meta of [SYNTH_META, DRUM_META]) {
      const built = buildOp1Chunk(meta);
      const parsed = parseOp1Chunk(built);
      expect(parsed.meta).toEqual(meta);
      expect(Array.from(buildOp1Chunk(parsed.meta, parsed.trailing).data)).toEqual(Array.from(built.data));
    }
  });

  it('rejects a chunk without a JSON object', () => {
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1nothing') })).toThrow(/no JSON/);
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1nothing') })).toThrow(Op1ChunkError);
    expect(() => parseOp1Chunk({ id: 'SSND', data: text('op-1{}') })).toThrow(Op1ChunkError);
    expect(new Op1ChunkError('x').name).toBe('Op1ChunkError');
  });

  it('rejects malformed JSON with the parser message', () => {
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1{"name":}') })).toThrow(Op1ChunkError);
    expect(() => parseOp1Chunk({ id: 'APPL', data: text('op-1{"name":}') })).toThrow(/JSON/);
  });

  it('rejects invalid UTF-8 inside the JSON', () => {
    const data = Uint8Array.from([...text('op-1{"name":"'), 0xff, 0xfe, ...text('"}')]);
    expect(() => parseOp1Chunk({ id: 'APPL', data })).toThrow(Op1ChunkError);
  });
});
