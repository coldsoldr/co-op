import { describe, expect, it } from 'vitest';
import { installOp1FunPatches, patchFileName } from './install';
import type { Op1FunPatch } from './types';
import { MemoryStore } from '../store/memory-store';
import { StagedStore } from '../library/changeset';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { expectSameBytes } from '../../../test/same-bytes';

const synth = fixtureBytes('synth-cluster.aif');
const drum = fixtureBytes('drum-boombap.aif');
const bare = fixtureBytes('no-metadata.aif');

const patch = (overrides: Partial<Op1FunPatch> = {}): Op1FunPatch => ({
  id: '1',
  name: 'Kick',
  description: '',
  license: '',
  public: true,
  patchType: '',
  fileUrl: 'https://op1fun.s3.amazonaws.com/1.aif',
  previewUrl: '',
  selfUrl: '',
  userId: 'alice',
  ...overrides,
});

const files = (map: Record<string, Uint8Array>) => async (url: string) => {
  const bytes = map[url];
  if (!bytes) throw new Error(`download failed: HTTP 404`);
  return bytes;
};

describe('patchFileName', () => {
  it('keeps ordinary names', () => {
    expect(patchFileName({ id: '1', name: 'Big Kick 2' })).toBe('Big Kick 2.aif');
  });

  it('replaces characters the drive cannot hold and tidies the ends', () => {
    expect(patchFileName({ id: '1', name: 'a/b\\c:d*e?f"g<h>i|j' })).toBe('a-b-c-d-e-f-g-h-i-j.aif');
    expect(patchFileName({ id: '1', name: '  ..hidden  name.  ' })).toBe('hidden name.aif');
    expect(patchFileName({ id: '1', name: `tab${String.fromCharCode(9)}and${String.fromCharCode(0)}nul` })).toBe('tab-and-nul.aif');
  });

  it('falls back to the id, avoids reserved names and fits the length limit', () => {
    expect(patchFileName({ id: '42', name: '' })).toBe('op1fun-42.aif');
    expect(patchFileName({ id: '42', name: ' . ' })).toBe('op1fun-42.aif');
    expect(patchFileName({ id: '1', name: 'CON' })).toBe('CON-patch.aif');
    const long = patchFileName({ id: '1', name: 'x'.repeat(200) });
    expect(long.length).toBeLessThanOrEqual(64 - 3);
  });
});

describe('installOp1FunPatches', () => {
  it('installs raw bytes into op1fun folders by the kind in the file', async () => {
    const store = new MemoryStore({});
    const progress: number[] = [];
    const result = await installOp1FunPatches(
      store,
      'op1/stock',
      [
        patch({ id: '1', name: 'Pad', patchType: 'drum', fileUrl: 'u1' }),
        patch({ id: '2', name: 'Kick', fileUrl: 'u2' }),
      ],
      files({ u1: synth, u2: drum }),
      (done) => progress.push(done),
    );
    expect(result.failed).toEqual([]);
    expect(result.installed.map((i) => i.path)).toEqual(['synth/op1fun/Pad.aif', 'drum/op1fun/Kick.aif']);
    expectSameBytes(await store.read('synth/op1fun/Pad.aif'), synth);
    expectSameBytes(await store.read('drum/op1fun/Kick.aif'), drum);
    expect(progress).toEqual([1, 2]);
  });

  it('uses op1.fun patch type when the file has no metadata', async () => {
    const store = new MemoryStore({});
    const result = await installOp1FunPatches(store, 'op1/stock', [patch({ patchType: 'Drum', fileUrl: 'u' })], files({ u: bare }));
    expect(result.installed.map((i) => i.path)).toEqual(['drum/op1fun/Kick.aif']);
    const unknown = await installOp1FunPatches(store, 'op1/stock', [patch({ fileUrl: 'u' })], files({ u: bare }));
    expect(unknown.failed[0]?.reason).toBe('not an OP-1 synth or drum patch');
  });

  it('never replaces an existing patch and reuses the folder spelled differently', async () => {
    const store = new MemoryStore({ 'synth/OP1FUN/pad.aif': drum });
    const result = await installOp1FunPatches(store, 'op1/stock', [patch({ name: 'Pad', fileUrl: 'u' })], files({ u: synth }));
    expect(result.installed[0]?.path).toBe('synth/OP1FUN/Pad-2.aif');
    expectSameBytes(await store.read('synth/OP1FUN/pad.aif'), drum);
  });

  it('carries on past failures and reports each one', async () => {
    const store = new MemoryStore({});
    const result = await installOp1FunPatches(
      store,
      'op1/stock',
      [
        patch({ id: '1', name: 'Missing', fileUrl: 'nowhere' }),
        patch({ id: '2', name: 'NoLink', fileUrl: '' }),
        patch({ id: '3', name: 'Junk', fileUrl: 'junk' }),
        patch({ id: '4', name: 'Good', fileUrl: 'good' }),
      ],
      files({ junk: new TextEncoder().encode('<html>not a patch</html>'), good: synth }),
    );
    expect(result.installed.map((i) => i.path)).toEqual(['synth/op1fun/Good.aif']);
    expect(result.failed.map((f) => [f.patch.id, f.reason])).toEqual([
      ['1', 'download failed: HTTP 404'],
      ['2', 'op1.fun gave no file link'],
      ['3', 'not an AIFF file'],
    ]);
    expect((await store.list('synth/op1fun')).map((e) => e.name)).toEqual(['Good.aif']);
  });

  it('stages installs on a read-only source', async () => {
    const staged = new StagedStore(new MemoryStore({ 'synth/user/a.aif': synth }, { write: false }));
    const result = await installOp1FunPatches(staged, 'op1/stock', [patch({ fileUrl: 'u' })], files({ u: drum }));
    expect(result.installed[0]?.path).toBe('drum/op1fun/Kick.aif');
    expect(staged.changes().map((c) => c.path)).toContain('drum/op1fun/Kick.aif');
  });
});
