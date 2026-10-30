import { describe, it, expect } from 'vitest';
import { StoreError, type DeviceStore } from '../src/core/store/store';

const bytes = (...values: number[]): Uint8Array => Uint8Array.from(values);

export type StoreFactory = (files: Record<string, Uint8Array>) => DeviceStore | Promise<DeviceStore>;

export interface ContractOptions {
  /**
   * Whether the backend can hold two names that differ only by case. False for FAT-style
   * case-insensitive drives (the OP-1's own disk mode); the seed then leaves out 'synth/user/B.aif'.
   * Defaults to true.
   */
  caseDistinctNames?: boolean;
}

function seedFor(options: ContractOptions): () => Record<string, Uint8Array> {
  const caseDistinct = options.caseDistinctNames ?? true;
  return () => ({
    'synth/user/b.aif': bytes(2, 2),
    'synth/user/a.aif': bytes(1),
    'synth/user/C.aif': bytes(3, 3, 3),
    ...(caseDistinct ? { 'synth/user/B.aif': bytes(6, 6, 6, 6) } : {}),
    'drum/user/kit.aif': bytes(4),
    'tape/track1.aif': bytes(5),
  });
}

/** The names in synth/user of the seed, in code-point order. */
function userNames(options: ContractOptions): string[] {
  return (options.caseDistinctNames ?? true) ? ['B.aif', 'C.aif', 'a.aif', 'b.aif'] : ['C.aif', 'a.aif', 'b.aif'];
}

/**
 * Read-side behaviour every DeviceStore must share, writable or not. `make` builds a store holding
 * exactly the given files (folders implied by their paths).
 */
export function describeReadableStore(name: string, make: StoreFactory, options: ContractOptions = {}): void {
  const seed = seedFor(options);

  describe(`${name} (readable DeviceStore contract)`, () => {
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
      expect(await store.list('synth/user')).toEqual(userNames(options).map((n) => ({ name: n, kind: 'file' })));
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

    it('reads the first bytes of a file with readHead', async () => {
      const store = await make(seed());
      expect(store.readHead).toBeTypeOf('function');
      expect(await store.readHead?.('synth/user/C.aif', 2)).toEqual(bytes(3, 3));
      expect(await store.readHead?.('synth/user/C.aif', 10)).toEqual(bytes(3, 3, 3));
      expect(await store.readHead?.('synth/user/C.aif', 0)).toEqual(bytes());
      await expect(store.readHead?.('synth/user/missing.aif', 2)).rejects.toBeInstanceOf(StoreError);
    });

    if (options.caseDistinctNames ?? true) {
      it('keeps names that differ only by case apart', async () => {
        const store = await make(seed());
        expect(await store.read('synth/user/B.aif')).toEqual(bytes(6, 6, 6, 6));
        expect(await store.read('synth/user/b.aif')).toEqual(bytes(2, 2));
      });
    }

    it('rejects reading a missing path or a folder with a StoreError naming the path', async () => {
      const store = await make(seed());
      await expect(store.read('synth/user/zz.aif')).rejects.toThrow(StoreError);
      await expect(store.read('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
      await expect(store.read('synth/user')).rejects.toThrow(StoreError);
      await expect(store.read('nope/zz.aif')).rejects.toThrow(StoreError);
    });

    it('reports sizes without reading', async () => {
      const store = await make(seed());
      expect(await store.size('synth/user/C.aif')).toBe(3);
      await expect(store.size('synth/user/zz.aif')).rejects.toThrow(StoreError);
      await expect(store.size('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
    });

    it('reports whether files and folders exist', async () => {
      const store = await make(seed());
      expect(await store.exists('')).toBe(true);
      expect(await store.exists('synth/user/a.aif')).toBe(true);
      expect(await store.exists('synth/user')).toBe(true);
      expect(await store.exists('synth/user/zz.aif')).toBe(false);
      expect(await store.exists('nope/zz.aif')).toBe(false);
      expect(await store.exists('synth/user/a.aif/deeper')).toBe(false);
    });
  });
}

/** Everything in describeReadableStore, plus the write side. */
export function describeWritableStore(name: string, make: StoreFactory, options: ContractOptions = {}): void {
  const seed = seedFor(options);

  describeReadableStore(name, make, options);

  describe(`${name} (writable DeviceStore contract)`, () => {
    it('is writable', async () => {
      const store = await make(seed());
      expect(store.capabilities.write).toBe(true);
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
      expect(await store.list('synth/user')).toHaveLength(userNames(options).length);
    });

    it('refuses mkdir over a file', async () => {
      const store = await make(seed());
      await expect(store.mkdir('synth/user/a.aif')).rejects.toThrow(StoreError);
    });

    it('removes an empty folder', async () => {
      const store = await make(seed());
      await store.mkdir('synth/pads');
      await store.removeFolder('synth/pads');
      expect(await store.exists('synth/pads')).toBe(false);
      expect((await store.list('synth')).map((e) => e.name)).not.toContain('pads');
    });

    it('refuses to remove a folder that holds anything, a file, or a missing folder', async () => {
      const store = await make(seed());
      await expect(store.removeFolder('synth/user')).rejects.toThrow(StoreError);
      await expect(store.removeFolder('synth/user/b.aif')).rejects.toThrow(StoreError);
      await expect(store.removeFolder('synth/none')).rejects.toThrow(StoreError);
      expect(await store.exists('synth/user/b.aif')).toBe(true);
    });
  });
}
