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

describe('MemoryListing ordering and lookup', () => {
  it('sorts entries by code-point order of name', async () => {
    const listing = new MemoryListing({
      'B.aif': 'file',
      'a.aif': 'file',
      '_x.aif': 'file',
      '2.aif': 'file',
    });
    expect((await listing.list('')).map((e) => e.name)).toEqual(['2.aif', 'B.aif', '_x.aif', 'a.aif']);
  });

  it('does not resolve inherited object properties as folders', async () => {
    const listing = new MemoryListing({ synth: {} });
    expect(await listing.list('constructor')).toEqual([]);
    expect(await listing.list('__proto__')).toEqual([]);
    expect(await listing.list('synth/constructor')).toEqual([]);
  });

  it('returns an empty list when the path names a file', async () => {
    const listing = new MemoryListing({ tape: { 'track1.aif': 'file' } });
    expect(await listing.list('tape/track1.aif')).toEqual([]);
  });
});
