import { describe, expect, it } from 'vitest';
import { MemoryStore } from '../store/memory-store';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { filterLocal, findPatchFiles, localFolders, scanLocalFolder, type LocalPatch } from './local-library';

const synth = fixtureBytes('synth-cluster.aif');
const drum = fixtureBytes('drum-boombap.aif');

describe('findPatchFiles', () => {
  it('finds patches at any depth, in any layout, and skips everything else', async () => {
    const store = new MemoryStore({
      'a.aif': synth,
      'my stuff/pads/deep/b.AIFF': synth,
      'notes.txt': synth,
      '.aif': synth,
      '.hidden/c.aif': synth,
      'pads/.d.aif': synth,
    });
    expect((await findPatchFiles(store)).sort()).toEqual(['a.aif', 'my stuff/pads/deep/b.AIFF']);
  });
});

describe('scanLocalFolder', () => {
  it('reads what the list shows', async () => {
    const store = new MemoryStore({ 'pads/one.aif': synth, 'kits/two.aif': drum });
    const seen: number[] = [];
    const patches = await scanLocalFolder(store, (done) => seen.push(done));
    expect(seen.at(-1)).toBe(2);
    expect(patches.find((p) => p.fileName === 'one.aif')).toMatchObject({
      path: 'pads/one.aif',
      folder: 'pads',
      kind: 'synth',
      engine: 'cluster',
      fx: 'nitro',
      sizeBytes: synth.length,
      error: null,
    });
    expect(patches.find((p) => p.fileName === 'two.aif')).toMatchObject({ kind: 'drum', error: null });
  });

  it('keeps a file it cannot read, with the reason', async () => {
    const [patch] = await scanLocalFolder(new MemoryStore({ 'broken.aif': Uint8Array.of(1, 2, 3) }));
    expect(patch).toMatchObject({ name: 'broken', kind: 'unknown', engine: null });
    expect(patch?.error).toBeTruthy();
  });

  it('stops when aborted', async () => {
    const controller = new AbortController();
    controller.abort(new Error('replaced'));
    await expect(scanLocalFolder(new MemoryStore({ 'a.aif': synth }), undefined, controller.signal)).rejects.toThrow('replaced');
  });
});

describe('filtering', () => {
  const patch = (over: Partial<LocalPatch>): LocalPatch => ({
    path: 'a.aif',
    fileName: 'a.aif',
    folder: '',
    name: 'a',
    kind: 'synth',
    engine: null,
    fx: null,
    sizeBytes: 1,
    error: null,
    ...over,
  });
  const list = [
    patch({ path: 'pads/glass.aif', fileName: 'glass.aif', folder: 'pads', name: 'glass harp', engine: 'fm' }),
    patch({ path: 'kits/dust.aif', fileName: 'dust.aif', folder: 'kits', name: 'dust kit', kind: 'drum' }),
    patch({ path: 'top.aif', fileName: 'top.aif', name: 'top' }),
  ];

  it('needs every word, ignoring case, across the fields', () => {
    expect(filterLocal(list, { text: 'HARP fm' }).map((p) => p.name)).toEqual(['glass harp']);
    expect(filterLocal(list, { text: 'harp drum' })).toEqual([]);
  });

  it('filters by folder and kind, and "" is the chosen folder itself', () => {
    expect(filterLocal(list, { folder: '' }).map((p) => p.name)).toEqual(['top']);
    expect(filterLocal(list, { kind: 'drum' }).map((p) => p.name)).toEqual(['dust kit']);
  });

  it('lists each folder once', () => {
    expect(localFolders(list)).toEqual(['', 'kits', 'pads']);
  });
});
