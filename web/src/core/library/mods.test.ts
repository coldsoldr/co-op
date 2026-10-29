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
    expect(await listOpusPlugins(store)).toEqual([
      { path: 'mods/Alpha.OPMOD', name: 'Alpha.OPMOD', sizeBytes: 4, valid: true, error: null },
      { path: 'mods/broken.opmod', name: 'broken.opmod', sizeBytes: 5, valid: false, error: null },
      { path: 'mods/short.opmod', name: 'short.opmod', sizeBytes: 1, valid: false, error: null },
      { path: 'mods/zeta.opmod', name: 'zeta.opmod', sizeBytes: 6, valid: true, error: null },
    ]);
  });

  it('finds the mods folder whatever its case', async () => {
    const store = new MemoryStore({ 'MODS/a.opmod': bytes(...OPMD) });
    expect((await listOpusPlugins(store)).map((p) => p.path)).toEqual(['MODS/a.opmod']);
  });

  it('returns nothing without a mods folder', async () => {
    expect(await listOpusPlugins(new MemoryStore({ 'synth/user/a.aif': bytes(1) }))).toEqual([]);
  });

  it('reports a file it cannot read instead of failing the list', async () => {
    const store = new MemoryStore({ 'mods/a.opmod': bytes(...OPMD) });
    store.read = async () => {
      throw new Error('gone');
    };
    expect(await listOpusPlugins(store)).toEqual([
      { path: 'mods/a.opmod', name: 'a.opmod', sizeBytes: 4, valid: false, error: 'gone' },
    ]);
  });
});
