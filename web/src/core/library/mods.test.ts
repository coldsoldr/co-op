import { describe, it, expect } from 'vitest';
import { listOpusPlugins } from './mods';
import { MemoryStore } from '../store/memory-store';

const OPMD = [0x4f, 0x50, 0x4d, 0x44];
const bytes = (...values: number[]): Uint8Array => Uint8Array.from(values);

describe('listOpusPlugins', () => {
  it('lists the .opmod files in mods/ in file-name order with sizes, flagging files without the magic', async () => {
    const store = new MemoryStore({
      'mods/zeta.opmod': bytes(...OPMD, 1, 2),
      'mods/Alpha.OPMOD': bytes(...OPMD),
      'mods/broken.opmod': bytes(1, 2, 3, 4, 5),
      'mods/short.opmod': bytes(0x4f),
      'mods/readme.txt': bytes(1),
      'mods/dx/bank.syx': bytes(0xf0),
      'mods/.hidden.opmod': bytes(...OPMD),
      'synth/user/a.aif': bytes(1),
    });
    expect((await listOpusPlugins(store)).plugins).toEqual([
      { path: 'mods/Alpha.OPMOD', name: 'Alpha.OPMOD', sizeBytes: 4, valid: true, error: null },
      { path: 'mods/broken.opmod', name: 'broken.opmod', sizeBytes: 5, valid: false, error: null },
      { path: 'mods/short.opmod', name: 'short.opmod', sizeBytes: 1, valid: false, error: null },
      { path: 'mods/zeta.opmod', name: 'zeta.opmod', sizeBytes: 6, valid: true, error: null },
    ]);
  });

  it('finds the mods folder whatever its case', async () => {
    const store = new MemoryStore({ 'MODS/a.opmod': bytes(...OPMD) });
    expect((await listOpusPlugins(store)).plugins.map((p) => p.path)).toEqual(['MODS/a.opmod']);
  });

  it('returns nothing without a mods folder', async () => {
    expect(await listOpusPlugins(new MemoryStore({ 'synth/user/a.aif': bytes(1) }))).toEqual({ plugins: [], leftovers: [] });
  });

  it('reports a file it cannot read instead of failing the list', async () => {
    const store = new MemoryStore({ 'mods/a.opmod': bytes(...OPMD) });
    store.readHead = async () => {
      throw new Error('gone');
    };
    expect((await listOpusPlugins(store)).plugins).toEqual([
      { path: 'mods/a.opmod', name: 'a.opmod', sizeBytes: 4, valid: false, error: 'gone' },
    ]);
  });
});

describe('listOpusPlugins details', () => {
  it('reads only the head of each plugin', async () => {
    const store = new MemoryStore({ 'mods/a.opmod': bytes(...OPMD, 9, 9, 9) });
    const asked: number[] = [];
    const head = store.readHead.bind(store);
    store.read = async () => {
      throw new Error('read the whole file');
    };
    store.readHead = async (path, n) => {
      asked.push(n);
      return head(path, n);
    };
    expect((await listOpusPlugins(store)).plugins[0]).toMatchObject({ valid: true, sizeBytes: 7 });
    expect(asked).toEqual([4]);
  });

  it('falls back to read for a store without readHead', async () => {
    const store = new MemoryStore({ 'mods/a.opmod': bytes(...OPMD) });
    Object.defineProperty(store, 'readHead', { value: undefined });
    expect((await listOpusPlugins(store)).plugins[0]?.valid).toBe(true);
  });

  it('lists co-op leftovers anywhere under mods', async () => {
    const store = new MemoryStore({
      'mods/a.opmod': bytes(...OPMD),
      'mods/.co-op-old-b.opmod': bytes(...OPMD),
      'mods/dx/.co-op-tmp-bank.syx': bytes(1),
      'mods/.DS_Store': bytes(1),
    });
    expect(await listOpusPlugins(store)).toMatchObject({
      leftovers: ['mods/.co-op-old-b.opmod', 'mods/dx/.co-op-tmp-bank.syx'],
    });
  });
});
