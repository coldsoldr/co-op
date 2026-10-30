import { describe, expect, it } from 'vitest';
import { MemoryStore } from '../store/memory-store';
import { forEachPath } from './bulk';
import { copyFiles } from './copy-to-folder';
import { findFolders, folderTree, type LocalPatch } from './local-library';
import { removeFolderTree } from './operations';

const one = Uint8Array.of(1, 2, 3);
const two = Uint8Array.of(4, 5, 6);

describe('forEachPath', () => {
  it('collects what each operation returned', async () => {
    expect(await forEachPath(['a', 'b'], async (p) => `x/${p}`)).toEqual({ done: ['x/a', 'x/b'], failed: [] });
  });

  it('carries on after a failure', async () => {
    const seen: number[] = [];
    const result = await forEachPath(
      ['a', 'b', 'c'],
      async (p) => {
        if (p === 'b') throw new Error('locked');
        return p;
      },
      (done) => seen.push(done),
    );
    expect(result).toEqual({ done: ['a', 'c'], failed: [{ path: 'b', message: 'locked' }] });
    expect(seen).toEqual([1, 2, 3]);
  });
});

describe('removeFolderTree', () => {
  it('removes a folder and everything inside it', async () => {
    const store = new MemoryStore({ 'keep/a.aif': one, 'gone/b.aif': one, 'gone/deep/er/c.aif': one });
    await removeFolderTree(store, 'gone');
    expect(await store.exists('gone')).toBe(false);
    expect(await store.exists('keep/a.aif')).toBe(true);
  });

  it('refuses the chosen folder itself', async () => {
    await expect(removeFolderTree(new MemoryStore({ 'a.aif': one }), '')).rejects.toThrow(/folder/);
  });
});

describe('findFolders', () => {
  it('lists folders at any depth and skips dot folders', async () => {
    const store = new MemoryStore({ 'a/b/c.aif': one, '.hidden/d.aif': one });
    await store.mkdir('empty');
    expect((await findFolders(store)).sort()).toEqual(['a', 'a/b', 'empty']);
  });
});

describe('folderTree', () => {
  it('shows folders that hold no patches', () => {
    const patch = { folder: 'a' } as LocalPatch;
    expect(folderTree([patch], ['a', 'a/empty', 'other'])).toEqual([
      { name: 'a', path: 'a', count: 1, children: [{ name: 'empty', path: 'a/empty', count: 0, children: [] }] },
      { name: 'other', path: 'other', count: 0, children: [] },
    ]);
  });
});

describe('copyFiles', () => {
  it('writes new files, skips identical ones and keeps both on a clash', async () => {
    const store = new MemoryStore({ 'op/same.aif': one, 'op/clash.aif': two });
    const read = async () => one;
    const result = await copyFiles(store, [
      { path: 'op/new.aif', read },
      { path: 'op/same.aif', read },
      { path: 'op/clash.aif', read },
    ]);
    expect(result).toEqual({ copied: ['op/new.aif', 'op/clash-2.aif'], already: ['op/same.aif'], failed: [] });
  });

  it('reports a file that cannot be read', async () => {
    const result = await copyFiles(new MemoryStore({}), [
      {
        path: 'op/bad.aif',
        read: async () => {
          throw new Error('drive gone');
        },
      },
    ]);
    expect(result.failed).toEqual([{ path: 'op/bad.aif', message: 'drive gone' }]);
  });
});
