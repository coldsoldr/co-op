import { describe, expect, it } from 'vitest';
import { unzipSync } from 'fflate';
import { MemoryStore } from '../store/memory-store';
import { sha256Hex } from '../util';
import { copyPatches, zipPatches, type CopyPatch } from './copy-to-folder';

const one = Uint8Array.of(1, 2, 3);
const two = Uint8Array.of(4, 5, 6);

const item = async (path: string, bytes: Uint8Array): Promise<CopyPatch> => ({ path, pack: { sha256: await sha256Hex(bytes) } });
const fetchOf = (bytes: Uint8Array) => async () => bytes;

describe('copyPatches', () => {
  it('writes a new patch at its pack path', async () => {
    const store = new MemoryStore({});
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: ['kit/drum/a.aif'], already: [], failed: [] });
    expect(await store.read('kit/drum/a.aif')).toEqual(one);
  });

  it('skips a patch that is already there', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': one });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: [], already: ['kit/drum/a.aif'], failed: [] });
  });

  it('keeps both when a different file has the name', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': two });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result.copied).toEqual(['kit/drum/a-2.aif']);
    expect(await store.read('kit/drum/a.aif')).toEqual(two);
    expect(await store.read('kit/drum/a-2.aif')).toEqual(one);
  });

  it('finds an identical copy under a numbered name', async () => {
    const store = new MemoryStore({ 'kit/drum/a.aif': two, 'kit/drum/a-2.aif': one });
    const result = await copyPatches(store, [await item('kit/drum/a.aif', one)], fetchOf(one));
    expect(result).toEqual({ copied: [], already: ['kit/drum/a.aif'], failed: [] });
  });

  it('carries on after a failed patch and reports it', async () => {
    const store = new MemoryStore({});
    const patches = [await item('kit/drum/bad.aif', one), await item('kit/drum/good.aif', two)];
    const seen: number[] = [];
    const result = await copyPatches(
      store,
      patches,
      async (patch) => {
        if (patch.path.endsWith('bad.aif')) throw new Error('no network');
        return two;
      },
      (done) => seen.push(done),
    );
    expect(result.copied).toEqual(['kit/drum/good.aif']);
    expect(result.failed).toEqual([{ path: 'kit/drum/bad.aif', message: 'no network' }]);
    expect(seen).toEqual([1, 2]);
  });
});

describe('zipPatches', () => {
  it('keeps the layout and reports failures', async () => {
    const patches = [await item('kit/drum/a.aif', one), await item('kit/drum/bad.aif', two)];
    const { zip, result } = await zipPatches(patches, async (patch) => {
      if (patch.path.endsWith('bad.aif')) throw new Error('no network');
      return one;
    });
    expect(Object.keys(unzipSync(zip))).toEqual(['kit/drum/a.aif']);
    expect(result).toEqual({ copied: ['kit/drum/a.aif'], already: [], failed: [{ path: 'kit/drum/bad.aif', message: 'no network' }] });
  });
});
