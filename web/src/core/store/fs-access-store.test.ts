import { describe, it, expect, vi, afterEach } from 'vitest';
import { FsAccessStore, isFsAccessSupported } from './fs-access-store';
import { StoreError } from './store';
import { describeWritableStore } from '../../../test/store-contract';
import { fakeDirectory, FakeDirectoryHandle, FakeFileHandle } from '../../../test/fake-fs-access';

describeWritableStore('FsAccessStore with FileSystemFileHandle.move', (files) => new FsAccessStore(fakeDirectory(files).handle));
describeWritableStore(
  'FsAccessStore without FileSystemFileHandle.move',
  (files) => new FsAccessStore(fakeDirectory(files, 'OP-1', { withMove: false }).handle),
);
describeWritableStore(
  'FsAccessStore on a case-insensitive (FAT) drive',
  (files) => new FsAccessStore(fakeDirectory(files, 'OP-1', { caseInsensitive: true }).handle),
  { caseDistinctNames: false },
);

describe('FsAccessStore', () => {
  it('takes its label from the root folder name', () => {
    expect(new FsAccessStore(fakeDirectory({}, 'OP-1 drive').handle).label).toBe('OP-1 drive');
  });

  it('uses the native move when the handle has one', async () => {
    const { root, handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1) });
    const synth = root.lookup('synth') as FakeDirectoryHandle;
    const fileHandle = (synth.lookup('user') as FakeDirectoryHandle).lookup('a.aif') as FakeFileHandle;
    await new FsAccessStore(handle).move('synth/user/a.aif', 'synth/user/b.aif');
    expect(fileHandle.name).toBe('b.aif');
  });

  it('falls back to read, write and remove when the native move is not supported', async () => {
    const { handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1) }, 'OP-1', { moveError: 'NotSupportedError' });
    const store = new FsAccessStore(handle);
    await store.move('synth/user/a.aif', 'synth/other/b.aif');
    expect(await store.exists('synth/user/a.aif')).toBe(false);
    expect(await store.read('synth/other/b.aif')).toEqual(Uint8Array.of(1));
  });

  it('does not fall back when the native move reports the source missing', async () => {
    const { handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1) }, 'OP-1', { moveError: 'NotFoundError' });
    const store = new FsAccessStore(handle);
    await expect(store.move('synth/user/a.aif', 'synth/user/b.aif')).rejects.toThrow(StoreError);
    expect(await store.exists('synth/user/b.aif')).toBe(false);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1));
  });

  it('leaves no new file behind when writing fails', async () => {
    const { handle } = fakeDirectory({}, 'OP-1', { writeError: 'QuotaExceededError' });
    const store = new FsAccessStore(handle);
    await expect(store.write('synth/user/a.aif', Uint8Array.of(1))).rejects.toThrow(StoreError);
    await expect(store.write('synth/user/a.aif', Uint8Array.of(1))).rejects.toThrow('synth/user/a.aif');
    expect(await store.exists('synth/user/a.aif')).toBe(false);
  });

  it('keeps an existing file unchanged when overwriting it fails', async () => {
    const { handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1, 2) }, 'OP-1', { writeError: 'QuotaExceededError' });
    const store = new FsAccessStore(handle);
    await expect(store.write('synth/user/a.aif', Uint8Array.of(9))).rejects.toThrow(StoreError);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1, 2));
  });

  it('on a case-insensitive drive, finds names in any case and overwrites the existing entry', async () => {
    const { root, handle } = fakeDirectory({ 'synth/user/B.aif': Uint8Array.of(1) }, 'OP-1', { caseInsensitive: true });
    const store = new FsAccessStore(handle);
    expect(await store.exists('SYNTH/user/b.aif')).toBe(true);
    await store.write('synth/user/b.aif', Uint8Array.of(2));
    expect(await store.list('synth/user')).toEqual([{ name: 'B.aif', kind: 'file' }]);
    expect(await store.read('synth/user/B.aif')).toEqual(Uint8Array.of(2));
    expect((root.lookup('synth') as FakeDirectoryHandle).children.size).toBe(1);
  });

  it('turns unexpected browser errors into StoreErrors naming the path, even in list and exists', async () => {
    const { root, handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1) });
    root.getDirectoryHandle = async () => {
      throw new DOMException('permission revoked', 'NotAllowedError');
    };
    const store = new FsAccessStore(handle);
    await expect(store.list('synth')).rejects.toThrow(StoreError);
    await expect(store.exists('synth/user/a.aif')).rejects.toThrow(StoreError);
    await expect(store.read('synth/user/a.aif')).rejects.toThrow('synth/user/a.aif');
    await expect(store.read('synth/user/a.aif')).rejects.toThrow('permission revoked');
  });
});

describe('isFsAccessSupported', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('is false where there is no window', () => {
    expect(isFsAccessSupported()).toBe(false);
  });

  it('is false where window has no showDirectoryPicker', () => {
    vi.stubGlobal('window', {});
    expect(isFsAccessSupported()).toBe(false);
  });

  it('is true where window has showDirectoryPicker', () => {
    vi.stubGlobal('window', { showDirectoryPicker: async () => undefined });
    expect(isFsAccessSupported()).toBe(true);
  });
});
