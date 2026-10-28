import { describe, it, expect } from 'vitest';
import { FsAccessStore, isFsAccessSupported } from './fs-access-store';
import { StoreError } from './store';
import { describeWritableStore } from '../../../test/store-contract';
import { fakeDirectory, FakeDirectoryHandle, FakeFileHandle } from '../../../test/fake-fs-access';

describeWritableStore('FsAccessStore with FileSystemFileHandle.move', (files) => new FsAccessStore(fakeDirectory(files).handle));
describeWritableStore(
  'FsAccessStore without FileSystemFileHandle.move',
  (files) => new FsAccessStore(fakeDirectory(files, 'OP-1', { withMove: false }).handle),
);

describe('FsAccessStore', () => {
  it('takes its label from the root folder name', () => {
    expect(new FsAccessStore(fakeDirectory({}, 'OP-1 drive').handle).label).toBe('OP-1 drive');
  });

  it('uses the native move when the handle has one', async () => {
    const { root, handle } = fakeDirectory({ 'synth/user/a.aif': Uint8Array.of(1) });
    const synth = root.children.get('synth') as FakeDirectoryHandle;
    const fileHandle = (synth.children.get('user') as FakeDirectoryHandle).children.get('a.aif') as FakeFileHandle;
    await new FsAccessStore(handle).move('synth/user/a.aif', 'synth/user/b.aif');
    expect(fileHandle.name).toBe('b.aif');
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
  it('is false where there is no showDirectoryPicker', () => {
    expect(isFsAccessSupported()).toBe(false);
  });
});
