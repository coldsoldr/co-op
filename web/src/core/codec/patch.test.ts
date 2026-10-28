import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { readPatch, writePatch, withMeta, decodePcm, PatchError } from './patch';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';

const fixture = (name: string): Uint8Array =>
  new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url))));

describe('readPatch', () => {
  it('reads a synth patch', () => {
    const patch = readPatch(fixture('synth-cluster.aif'));
    expect(patch.kind).toBe('synth');
    expect(patch.name).toBe('default');
    expect(patch.meta).toEqual(SYNTH_META);
    expect(patch.comm.sampleRate).toBe(44100);
    expect(patch.comm.compression).toBe('NONE');
  });

  it('reads a little-endian AIFC sampler patch', () => {
    const patch = readPatch(fixture('synth-sampler-sowt.aif'));
    expect(patch.kind).toBe('synth');
    expect(patch.meta).toEqual(SAMPLER_META);
    expect(patch.comm.compression).toBe('sowt');
  });

  it('reads a drum patch', () => {
    const patch = readPatch(fixture('drum-boombap.aif'));
    expect(patch.kind).toBe('drum');
    expect(patch.name).toBe('boombap1');
    expect(patch.meta).toEqual(DRUM_META);
  });

  it('reads an AIFF with no op-1 metadata as unknown', () => {
    const patch = readPatch(fixture('no-metadata.aif'));
    expect(patch.kind).toBe('unknown');
    expect(patch.meta).toBeNull();
    expect(patch.name).toBe('');
  });

  it('rejects non-AIFF input', () => {
    expect(() => readPatch(new TextEncoder().encode('hello'))).toThrow(PatchError);
  });
});

describe('writePatch', () => {
  it('round-trips every fixture byte for byte when nothing changed', () => {
    for (const name of ['synth-cluster.aif', 'synth-sampler-sowt.aif', 'drum-boombap.aif', 'no-metadata.aif']) {
      const bytes = fixture(name);
      expect(Array.from(writePatch(readPatch(bytes)))).toEqual(Array.from(bytes));
    }
  });

  it('writes an edited name that reads back', () => {
    const original = readPatch(fixture('synth-cluster.aif'));
    const renamed = withMeta(original, { ...SYNTH_META, name: 'renamed' });
    const reread = readPatch(writePatch(renamed));
    expect(reread.name).toBe('renamed');
    expect(reread.meta).toEqual({ ...SYNTH_META, name: 'renamed' });
    expect(Array.from(decodePcm(reread))).toEqual(Array.from(decodePcm(original)));
  });

  it('adds an APPL chunk before SSND when a file had none', () => {
    const plain = readPatch(fixture('no-metadata.aif'));
    const tagged = withMeta(plain, DRUM_META);
    const bytes = writePatch(tagged);
    const reread = readPatch(bytes);
    expect(reread.kind).toBe('drum');
    const ids = reread.form.chunks.map((c) => c.id);
    expect(ids.indexOf('APPL')).toBeLessThan(ids.indexOf('SSND'));
  });
});

describe('decodePcm', () => {
  it('returns the expected number of frames', () => {
    const patch = readPatch(fixture('synth-cluster.aif'));
    expect(decodePcm(patch).length).toBe(Math.round(0.25 * 44100));
  });
});
