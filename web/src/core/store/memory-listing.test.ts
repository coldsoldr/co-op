import { describe, it, expect } from 'vitest';
import { MemoryListing } from './memory-listing';

describe('MemoryListing', () => {
  const listing = new MemoryListing({
    synth: { user: { 'a.aif': 'file', 'b.aif': 'file' }, snapshot: {} },
    drum: { user: {} },
    tape: { 'track1.aif': 'file' },
  });

  it('lists the root', async () => {
    const entries = await listing.list('');
    expect(entries).toEqual([
      { name: 'drum', kind: 'dir' },
      { name: 'synth', kind: 'dir' },
      { name: 'tape', kind: 'dir' },
    ]);
  });

  it('lists nested folders with files', async () => {
    expect(await listing.list('synth/user')).toEqual([
      { name: 'a.aif', kind: 'file' },
      { name: 'b.aif', kind: 'file' },
    ]);
  });

  it('returns an empty list for a missing path', async () => {
    expect(await listing.list('nope/nothing')).toEqual([]);
  });
});
