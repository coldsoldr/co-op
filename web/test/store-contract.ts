import { describe, it, expect } from 'vitest';
import { StoreError, type DeviceStore } from '../src/core/store/store';

const bytes = (...values: number[]): Uint8Array => Uint8Array.from(values);

/**
 * Behaviour every writable DeviceStore must share. `make` builds a store holding exactly the given
 * files (folders implied by their paths).
 */
export function describeWritableStore(
  name: string,
  make: (files: Record<string, Uint8Array>) => DeviceStore | Promise<DeviceStore>,
): void {
  const seed = (): Record<string, Uint8Array> => ({
    'synth/user/b.aif': bytes(2, 2),
    'synth/user/a.aif': bytes(1),
    'synth/user/B.aif': bytes(3, 3, 3),
    'drum/user/kit.aif': bytes(4),
    'tape/track1.aif': bytes(5),
  });

  describe(`${name} (DeviceStore contract)`, () => {
    it('is writable', async () => {
      const store = await make(seed());
      expect(store.capabilities.write).toBe(true);
    });

    it('lists the root with folders implied by file paths', async () => {
      const store = await make(seed());
      expect(await store.list('')).toEqual([
        { name: 'drum', kind: 'dir' },
        { name: 'synth', kind: 'dir' },
        { name: 'tape', kind: 'dir' },
      ]);
    });

    it('lists a folder in code-point order', async () => {
      const store = await make(seed());
      expect(await store.list('synth/user')).toEqual([
        { name: 'B.aif', kind: 'file' },
        { name: 'a.aif', kind: 'file' },
        { name: 'b.aif', kind: 'file' },
      ]);
    });

    it('lists [] for a missing path and for a file', async () => {
      const store = await make(seed());
      expect(await store.list('nope/nothing')).toEqual([]);
      expect(await store.list('synth/user/a.aif')).toEqual([]);
      expect(await store.list('synth/user/a.aif/deeper')).toEqual([]);
    });

    it('reads file bytes', async () => {
      const store = await make(seed());
      expect(await store.read('synth/user/b.aif')).toEqual(bytes(2, 2));
    });

    it('rejects reading a missing path or a folder with a StoreError naming the path', async () => {
      const store = await make(seed());
      await expect(store.read('synth/user/zz.aif')).rejects.toThrow(StoreError);
      await expect(store.read('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
      await expect(store.read('synth/user')).rejects.toThrow(StoreError);
      await expect(store.read('nope/zz.aif')).rejects.toThrow(StoreError);
    });

    it('reports sizes without reading', async () => {
      const store = await make(seed());
      expect(await store.size('synth/user/B.aif')).toBe(3);
      await expect(store.size('synth/user/zz.aif')).rejects.toThrow(StoreError);
      await expect(store.size('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
    });

    it('reports whether files and folders exist', async () => {
      const store = await make(seed());
      expect(await store.exists('synth/user/a.aif')).toBe(true);
      expect(await store.exists('synth/user')).toBe(true);
      expect(await store.exists('synth/user/zz.aif')).toBe(false);
      expect(await store.exists('nope/zz.aif')).toBe(false);
      expect(await store.exists('synth/user/a.aif/deeper')).toBe(false);
    });

    it('writes new files, creating parent folders', async () => {
      const store = await make(seed());
      await store.write('synth/new/deep/x.aif', bytes(9, 8));
      expect(await store.read('synth/new/deep/x.aif')).toEqual(bytes(9, 8));
      expect(await store.list('synth/new')).toEqual([{ name: 'deep', kind: 'dir' }]);
    });

    it('overwrites existing files', async () => {
      const store = await make(seed());
      await store.write('synth/user/a.aif', bytes(7, 7, 7, 7));
      expect(await store.read('synth/user/a.aif')).toEqual(bytes(7, 7, 7, 7));
      expect(await store.size('synth/user/a.aif')).toBe(4);
    });

    it('keeps its own copy of written bytes', async () => {
      const store = await make(seed());
      const data = bytes(1, 2, 3);
      await store.write('x.aif', data);
      data[0] = 99;
      expect(await store.read('x.aif')).toEqual(bytes(1, 2, 3));
    });

    it('refuses to write onto a folder', async () => {
      const store = await make(seed());
      await expect(store.write('synth/user', bytes(1))).rejects.toThrow(StoreError);
    });

    it('moves a file, creating the target folder', async () => {
      const store = await make(seed());
      await store.move('synth/user/a.aif', 'synth/other/a2.aif');
      expect(await store.exists('synth/user/a.aif')).toBe(false);
      expect(await store.read('synth/other/a2.aif')).toEqual(bytes(1));
    });

    it('refuses to move onto an existing path', async () => {
      const store = await make(seed());
      await expect(store.move('synth/user/a.aif', 'synth/user/b.aif')).rejects.toThrow(StoreError);
      expect(await store.read('synth/user/a.aif')).toEqual(bytes(1));
      expect(await store.read('synth/user/b.aif')).toEqual(bytes(2, 2));
    });

    it('refuses to move a missing file', async () => {
      const store = await make(seed());
      await expect(store.move('synth/user/zz.aif', 'synth/user/yy.aif')).rejects.toThrow(StoreError);
    });

    it('removes files and keeps the emptied folder', async () => {
      const store = await make(seed());
      await store.remove('tape/track1.aif');
      expect(await store.exists('tape/track1.aif')).toBe(false);
      expect(await store.list('tape')).toEqual([]);
      expect(await store.exists('tape')).toBe(true);
    });

    it('refuses to remove a missing file or a folder', async () => {
      const store = await make(seed());
      await expect(store.remove('tape/zz.aif')).rejects.toThrow(StoreError);
      await expect(store.remove('tape')).rejects.toThrow(StoreError);
      expect(await store.exists('tape/track1.aif')).toBe(true);
    });

    it('creates folders and parents with mkdir, visible in list', async () => {
      const store = await make(seed());
      await store.mkdir('synth/pads/soft');
      expect(await store.list('synth')).toEqual([
        { name: 'pads', kind: 'dir' },
        { name: 'user', kind: 'dir' },
      ]);
      expect(await store.list('synth/pads')).toEqual([{ name: 'soft', kind: 'dir' }]);
      expect(await store.exists('synth/pads/soft')).toBe(true);
    });

    it('treats mkdir of an existing folder as a no-op', async () => {
      const store = await make(seed());
      await store.mkdir('synth/user');
      expect(await store.list('synth/user')).toHaveLength(3);
    });

    it('refuses mkdir over a file', async () => {
      const store = await make(seed());
      await expect(store.mkdir('synth/user/a.aif')).rejects.toThrow(StoreError);
    });
  });
}
