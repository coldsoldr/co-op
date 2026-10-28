import { describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { readPatch } from '../src/core/codec/patch';
import { isDrumMeta } from '../src/core/codec/op1-meta';
import { TEST_TONES_DIR, makeTestTones } from './make-test-tones';
import { expectSameBytes } from '../test/same-bytes';

describe('test-tones pack', () => {
  const files = makeTestTones();

  it('has three patches with neutral names', () => {
    expect(Object.fromEntries(Object.entries(files).map(([path, bytes]) => [path, readPatch(bytes).name]))).toEqual({
      'synth/tone-cluster.aif': 'tone-cluster',
      'synth/tone-sampler.aif': 'tone-sampler',
      'drum/tone-drum.aif': 'tone-drum',
    });
    expect(readPatch(files['synth/tone-cluster.aif'] as Uint8Array).kind).toBe('synth');
    expect(readPatch(files['drum/tone-drum.aif'] as Uint8Array).kind).toBe('drum');
  });

  it('spaces the drum slices evenly inside the audio', () => {
    const drum = readPatch(files['drum/tone-drum.aif'] as Uint8Array);
    if (!isDrumMeta(drum.meta)) throw new Error('not a drum patch');
    const { start, end } = drum.meta;
    expect(start).toHaveLength(24);
    const step = (start[1] as number) - (start[0] as number);
    start.forEach((s, i) => {
      expect(s).toBe(i * step);
      expect(end[i]).toBeGreaterThan(s);
      expect(end[i]).toBeLessThan(s + step);
    });
    expect((end[23] as number) / 4058).toBeLessThan(drum.comm.frames);
  });

  it('matches the committed files byte for byte (run scripts/make-test-tones.ts after changing it)', () => {
    for (const [path, bytes] of Object.entries(files)) {
      expectSameBytes(new Uint8Array(readFileSync(join(TEST_TONES_DIR, path))), bytes, path);
    }
  });
});
