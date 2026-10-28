import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { readPatch, writePatch, withMeta, decodePcm, PatchError, type Patch } from './patch';
import { writeForm, type Chunk } from './iff';
import { buildComm } from './comm';
import { buildOp1Chunk } from './op1-chunk';
import { buildSsnd } from './ssnd';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';
import { firstDifference } from '../../../test/first-difference';

const fixture = (name: string): Uint8Array =>
  new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url))));

const GENERATED_FIXTURES = ['synth-cluster.aif', 'synth-sampler-sowt.aif', 'drum-boombap.aif', 'no-metadata.aif'];

const REAL_DIR = fileURLToPath(new URL('../../../test/fixtures/real/', import.meta.url));
const realFiles = (): string[] => readdirSync(REAL_DIR).filter((f) => f.toLowerCase().endsWith('.aif'));

/** What we know about each file copied from the owner's OP-1. Files not listed here must still round-trip. */
const REAL_EXPECTATIONS: Record<string, { kind: string; name: string; sampleRate: number; compression: string }> = {
  'synth.aif': { kind: 'synth', name: 'realistic-cello-4545', sampleRate: 22050, compression: 'sowt' },
  'drum.aif': { kind: 'drum', name: 'rlnd_tr808-7', sampleRate: 44100, compression: 'sowt' },
};

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

describe('readPatch errors', () => {
  const caught = (bytes: Uint8Array): PatchError => {
    try {
      readPatch(bytes);
    } catch (error) {
      return error as PatchError;
    }
    throw new Error('expected readPatch to throw');
  };

  it('says "not an AIFF file" when the FORM magic is missing', () => {
    const error = caught(new TextEncoder().encode('hello world, long enough'));
    expect(error).toBeInstanceOf(PatchError);
    expect(error.message).toBe('not an AIFF file');
    expect(error.cause).toBeInstanceOf(Error);
  });

  it('says "malformed AIFF container" when FORM is present but broken', () => {
    const good = fixture('no-metadata.aif');
    const truncated = good.subarray(0, good.length - 7);
    const error = caught(truncated);
    expect(error).toBeInstanceOf(PatchError);
    expect(error.message).toMatch(/^malformed AIFF container: /);
    expect(error.cause).toBeInstanceOf(Error);
    expect((error.cause as Error).message).toMatch(/runs past end of file/);
  });

  it('wraps COMM and op-1 metadata errors with their cause', () => {
    const shortComm: Chunk = { id: 'COMM', data: new Uint8Array(4) };
    const commError = caught(writeForm({ formType: 'AIFF', chunks: [shortComm] }));
    expect(commError.message).toMatch(/^bad COMM chunk: /);
    expect((commError.cause as Error).name).toBe('CommError');

    const goodComm = readPatch(fixture('no-metadata.aif')).form.chunks[0]!;
    const badJson: Chunk = { id: 'APPL', data: new TextEncoder().encode('op-1{not json}') };
    const metaError = caught(writeForm({ formType: 'AIFF', chunks: [goodComm, badJson] }));
    expect(metaError.message).toMatch(/^bad op-1 metadata: /);
    expect((metaError.cause as Error).name).toBe('Op1ChunkError');
  });
});

describe('writePatch', () => {
  it.each(GENERATED_FIXTURES)('round-trips %s byte for byte when nothing changed', (name) => {
    const bytes = fixture(name);
    expect(firstDifference(writePatch(readPatch(bytes)), bytes), name).toBe(-1);
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

});

describe('real device files', () => {
  it('has at least one .aif in test/fixtures/real', () => {
    expect(realFiles().length).toBeGreaterThan(0);
  });

  it.each(Object.entries(REAL_EXPECTATIONS))('reads %s as expected', (name, expected) => {
    const patch = readPatch(fixture(`real/${name}`));
    expect(patch.kind).toBe(expected.kind);
    expect(patch.name).toBe(expected.name);
    expect(patch.comm.sampleRate).toBe(expected.sampleRate);
    expect(patch.comm.compression).toBe(expected.compression);
  });

  it('expects every file named in the table to exist', () => {
    for (const name of Object.keys(REAL_EXPECTATIONS)) expect(realFiles(), name).toContain(name);
  });

  it.each(realFiles())('round-trips %s byte for byte and survives a rename', (name) => {
    const bytes = fixture(`real/${name}`);
    const patch = readPatch(bytes);
    expect(patch.kind).not.toBe('unknown');
    expect(patch.name.length).toBeGreaterThan(0);
    expect(firstDifference(writePatch(patch), bytes), name).toBe(-1);
    const renamed = readPatch(writePatch(withMeta(patch, { ...patch.meta!, name: 'x' })));
    expect(renamed.name).toBe('x');
    expect(renamed.comm).toEqual(patch.comm);
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

describe('op1Canonical', () => {
  const samples = Int16Array.from([0, 1000, -1000, 0]);
  const comm = () =>
    buildComm(
      { channels: 1, frames: samples.length, bitsPerSample: 16, sampleRate: 44100, compression: 'NONE', compressionName: '' },
      'AIFF',
    );

  it.each(GENERATED_FIXTURES)('generated fixture %s is canonical', (name) => {
    expect(readPatch(fixture(name)).op1Canonical).toBe(true);
  });

  it.each(realFiles())('real device file %s is canonical', (name) => {
    expect(readPatch(fixture(`real/${name}`)).op1Canonical).toBe(true);
  });

  it('reads pretty-printed metadata as non-canonical but still usable', () => {
    const encoder = new TextEncoder();
    const json = encoder.encode(JSON.stringify(SYNTH_META, null, 2));
    const data = new Uint8Array(4 + json.length + 1);
    data.set(encoder.encode('op-1'), 0);
    data.set(json, 4);
    data[4 + json.length] = 0x0a;
    const pretty: Chunk = { id: 'APPL', data };
    const bytes = writeForm({ formType: 'AIFF', chunks: [comm(), pretty, buildSsnd(samples, false)] });
    const patch = readPatch(bytes);
    expect(patch.op1Canonical).toBe(false);
    expect(patch.kind).toBe('synth');
    expect(patch.name).toBe('default');
    expect(patch.meta).toEqual(SYNTH_META);
    const edited = readPatch(writePatch(withMeta(patch, { ...SYNTH_META, name: 'tidy' })));
    expect(edited.op1Canonical).toBe(true);
    expect(edited.name).toBe('tidy');
  });
});

describe('raw', () => {
  it('aliases the input bytes on read', () => {
    const bytes = fixture('synth-cluster.aif');
    expect(readPatch(bytes).raw).toBe(bytes);
  });

  it('is the re-serialised file after withMeta and leaves the original alone', () => {
    const bytes = fixture('synth-cluster.aif');
    const original = readPatch(bytes);
    const renamed = withMeta(original, { ...SYNTH_META, name: 'renamed' });
    expect(Array.from(renamed.raw)).toEqual(Array.from(writePatch(renamed)));
    expect(Array.from(renamed.raw)).not.toEqual(Array.from(bytes));
    expect(original.raw).toBe(bytes);
    expect(Array.from(original.raw)).toEqual(Array.from(fixture('synth-cluster.aif')));
  });

  it('refreshes form after withMeta so chunk sizes match the rebuilt APPL', () => {
    const original = readPatch(fixture('synth-cluster.aif'));
    const renamed = withMeta(original, { ...SYNTH_META, name: `${SYNTH_META.name}-much-longer-name` });
    const applOf = (p: Patch) => p.form.chunks.find((c) => c.id === 'APPL')!;
    expect(applOf(renamed).data.length).not.toBe(applOf(original).data.length);
    expect(Array.from(writePatch(renamed))).toEqual(Array.from(renamed.raw));
  });
});
