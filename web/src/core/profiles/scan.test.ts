import { describe, it, expect } from 'vitest';
import { scanDrive } from './scan';
import { op1Stock } from './op1-stock';
import type { DeviceProfile } from './types';
import { MemoryListing } from '../store/memory-listing';

describe('scanDrive', () => {
  const root = new MemoryListing({
    synth: {
      user: { '1.aif': 'file', '2.AIF': 'file', 'notes.txt': 'file', '._1.aif': 'file', deeper: { 'x.aif': 'file' } },
      snapshot: { '20240101_1200.aif': 'file' },
      'stray.aif': 'file',
      '.hidden': { 'h.aif': 'file' },
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

  it('skips dot-prefixed files and folders such as AppleDouble ._ files', async () => {
    const paths = (await scanDrive(root, op1Stock)).map((p) => p.path);
    expect(paths).not.toContain('synth/user/._1.aif');
    expect(paths.some((p) => p.startsWith('synth/.hidden/'))).toBe(false);
  });

  it('ignores files directly under a patch root and folders deeper than one level', async () => {
    const paths = (await scanDrive(root, op1Stock)).map((p) => p.path);
    expect(paths).not.toContain('synth/stray.aif');
    expect(paths).not.toContain('synth/user/deeper/x.aif');
  });

  it('returns nothing for a drive without patch roots', async () => {
    expect(await scanDrive(new MemoryListing({ Music: {} }), op1Stock)).toEqual([]);
  });

  it('throws when a patch root names a kind the profile has no spec for', async () => {
    const misconfigured: DeviceProfile = {
      ...op1Stock,
      id: 'test/misconfigured',
      layout: { patchRoots: [{ path: 'synth', kind: 'synth' }], otherRoots: [] },
      patchKinds: [{ kind: 'drum', extensions: ['.aif'] }],
    };
    await expect(scanDrive(root, misconfigured)).rejects.toThrow(/synth/);
  });
});
