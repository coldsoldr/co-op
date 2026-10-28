import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { readPatch, writePatch, withMeta, decodePcm, PatchError } from './patch';
import { writeForm, type Chunk } from './iff';
import { buildComm } from './comm';
import { buildOp1Chunk } from './op1-chunk';
import { buildSsnd } from './ssnd';
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
    expect(ids).toEqual(['COMM', 'APPL', 'SSND']);
  });

  it('round-trips real device files byte for byte', () => {
    const dir = fileURLToPath(new URL('../../../test/fixtures/real/', import.meta.url));
    const files = readdirSync(dir).filter((f) => f.toLowerCase().endsWith('.aif'));
    expect(files.length).toBeGreaterThan(0);
    for (const name of files) {
      const bytes = new Uint8Array(readFileSync(dir + name));
      const patch = readPatch(bytes);
      expect(patch.kind).not.toBe('unknown');
      expect(patch.name.length).toBeGreaterThan(0);
      expect(Array.from(writePatch(patch))).toEqual(Array.from(bytes));
      const renamed = readPatch(writePatch(withMeta(patch, { ...patch.meta!, name: 'x' })));
      expect(renamed.name).toBe('x');
      expect(renamed.comm).toEqual(patch.comm);
    }
  });
});

describe('decodePcm', () => {
  it('returns the expected number of frames', () => {
    const patch = readPatch(fixture('synth-cluster.aif'));
    expect(decodePcm(patch).length).toBe(Math.round(0.25 * 44100));
  });
});

describe('writePatch APPL placement', () => {
  const samples = Int16Array.from([0, 1000, -1000, 0]);
  const comm = () =>
    buildComm(
      { channels: 1, frames: samples.length, bitsPerSample: 16, sampleRate: 44100, compression: 'NONE', compressionName: '' },
      'AIFF',
    );
  const ssnd = () => buildSsnd(samples, false);
  const form = (chunks: Chunk[]): Uint8Array => writeForm({ formType: 'AIFF', chunks });
  const ids = (bytes: Uint8Array): string[] => readPatch(bytes).form.chunks.map((c) => c.id);

  it('keeps an APPL that follows SSND where it was', () => {
    const bytes = form([comm(), ssnd(), buildOp1Chunk(SYNTH_META)]);
    const patch = readPatch(bytes);
    expect(Array.from(writePatch(patch))).toEqual(Array.from(bytes));
    const renamed = readPatch(writePatch(withMeta(patch, { ...SYNTH_META, name: 'after' })));
    expect(renamed.name).toBe('after');
    expect(renamed.form.chunks.map((c) => c.id)).toEqual(['COMM', 'SSND', 'APPL']);
  });

  it('rejects a file with two op-1 APPL chunks', () => {
    const bytes = form([comm(), buildOp1Chunk(SYNTH_META), buildOp1Chunk(DRUM_META), ssnd()]);
    expect(() => readPatch(bytes)).toThrow(/more than one/);
    expect(() => readPatch(bytes)).toThrow(PatchError);
  });

  it('preserves a non-op-1 APPL chunk verbatim through an edit', () => {
    const foreign: Chunk = { id: 'APPL', data: new TextEncoder().encode('abcd some other payload') };
    const bytes = form([comm(), foreign, buildOp1Chunk(SYNTH_META), ssnd()]);
    const renamed = readPatch(writePatch(withMeta(readPatch(bytes), { ...SYNTH_META, name: 'edited' })));
    expect(renamed.form.chunks.map((c) => c.id)).toEqual(['COMM', 'APPL', 'APPL', 'SSND']);
    expect(Array.from(renamed.form.chunks[1]!.data)).toEqual(Array.from(foreign.data));
    expect(renamed.name).toBe('edited');
  });

  it('strips the op-1 APPL chunk when meta is null', () => {
    const patch = readPatch(form([comm(), buildOp1Chunk(SYNTH_META), ssnd()]));
    const stripped = readPatch(writePatch({ ...patch, meta: null }));
    expect(stripped.kind).toBe('unknown');
    expect(stripped.meta).toBeNull();
    expect(stripped.form.chunks.map((c) => c.id)).toEqual(['COMM', 'SSND']);
    expect(ids(writePatch(patch))).toEqual(['COMM', 'APPL', 'SSND']);
  });
});
