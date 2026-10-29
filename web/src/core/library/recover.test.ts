import { describe, it, expect } from 'vitest';
import { recoverDrive } from './recover';
import { MemoryStore } from '../store/memory-store';
import { op1Stock } from '../profiles/op1-stock';
import { op1Opus } from '../profiles/op1-opus';

const b = (value: number): Uint8Array => Uint8Array.of(value);

const files = () => ({
  'synth/user/.co-op-old-a.aif': b(1),
  'drum/kits/.co-op-tmp-k.aif': b(2),
  'MODS/.co-op-old-x.opmod': b(3),
  'MODS/dx/.co-op-tmp-bank.syx': b(4),
  'tape/.co-op-old-track.aif': b(5),
});

describe('recoverDrive', () => {
  it('recovers patch folders and, for opus, the whole mods tree whatever its case', async () => {
    const store = new MemoryStore(files());
    expect(await recoverDrive(store, op1Opus)).toEqual(['synth/user/a.aif', 'MODS/x.opmod']);
    expect(store.snapshot()).toEqual({
      'synth/user/a.aif': b(1),
      'MODS/x.opmod': b(3),
      'tape/.co-op-old-track.aif': b(5),
    });
  });

  it('leaves mods alone for a profile that does not back it up', async () => {
    const store = new MemoryStore(files());
    expect(await recoverDrive(store, op1Stock)).toEqual(['synth/user/a.aif']);
    expect(Object.keys(store.snapshot())).toContain('MODS/.co-op-old-x.opmod');
  });

  it('does nothing on a read-only store', async () => {
    expect(await recoverDrive(new MemoryStore(files(), { write: false }), op1Opus)).toEqual([]);
  });
});
