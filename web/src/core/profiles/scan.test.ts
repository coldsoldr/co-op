import { describe, it, expect } from 'vitest';
import { scanDrive } from './scan';
import { op1Stock } from './op1-stock';
import { MemoryListing } from '../store/memory-listing';

describe('scanDrive', () => {
  const root = new MemoryListing({
    synth: {
      user: { '1.aif': 'file', '2.AIF': 'file', 'notes.txt': 'file' },
      snapshot: { '20240101_1200.aif': 'file' },
      'stray.aif': 'file',
    },
    drum: { user: { 'kit.aif': 'file' }, empty: {} },
    tape: { 'track1.aif': 'file' },
  });

  it('finds patches one folder below each patch root, matching extensions case-insensitively', async () => {
    const found = await scanDrive(root, op1Stock);
    expect(found).toEqual([
      { path: 'synth/snapshot/20240101_1200.aif', folder: 'snapshot', fileName: '20240101_1200.aif', kind: 'synth' },
      { path: 'synth/user/1.aif', folder: 'user', fileName: '1.aif', kind: 'synth' },
      { path: 'synth/user/2.AIF', folder: 'user', fileName: '2.AIF', kind: 'synth' },
      { path: 'drum/user/kit.aif', folder: 'user', fileName: 'kit.aif', kind: 'drum' },
    ]);
  });

  it('returns nothing for a drive without patch roots', async () => {
    expect(await scanDrive(new MemoryListing({ Music: {} }), op1Stock)).toEqual([]);
  });
});
