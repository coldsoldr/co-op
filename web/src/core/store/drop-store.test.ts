import { describe, it, expect } from 'vitest';
import { DropStore, fromInputFiles, filesFromDataTransfer } from './drop-store';
import { StoreError } from './store';
import { describeReadableStore } from '../../../test/store-contract';

const file = (name: string, ...values: number[]): File => new File([Uint8Array.from(values)], name);

function dropped(): DropStore {
  return new DropStore([
    { path: 'OP-1/synth/user/b.aif', file: file('b.aif', 2, 2) },
    { path: 'OP-1/synth/user/a.aif', file: file('a.aif', 1) },
    { path: 'OP-1/synth/user/B.aif', file: file('B.aif', 3, 3, 3) },
    { path: 'OP-1/drum/user/kit.aif', file: file('kit.aif', 4) },
    { path: 'OP-1/tape/track1.aif', file: file('track1.aif', 5) },
  ]);
}

describeReadableStore('DropStore', (files) =>
  new DropStore(Object.entries(files).map(([path, bytes]) => ({ path: `OP-1/${path}`, file: new File([bytes.slice()], path) }))),
);

describe('DropStore', () => {
  it('is read-only and labelled with the dropped folder name', () => {
    const store = dropped();
    expect(store.capabilities.write).toBe(false);
    expect(store.label).toBe('OP-1');
  });

  it('lists paths relative to the dropped folder', async () => {
    const store = dropped();
    expect(await store.list('')).toEqual([
      { name: 'drum', kind: 'dir' },
      { name: 'synth', kind: 'dir' },
      { name: 'tape', kind: 'dir' },
    ]);
    expect(await store.list('synth/user')).toEqual([
      { name: 'B.aif', kind: 'file' },
      { name: 'a.aif', kind: 'file' },
      { name: 'b.aif', kind: 'file' },
    ]);
  });

  it('lists [] for a missing path and for a file', async () => {
    const store = dropped();
    expect(await store.list('nope')).toEqual([]);
    expect(await store.list('tape/track1.aif')).toEqual([]);
  });

  it('reads bytes and sizes', async () => {
    const store = dropped();
    expect(await store.read('synth/user/b.aif')).toEqual(Uint8Array.of(2, 2));
    expect(await store.size('synth/user/B.aif')).toBe(3);
  });

  it('rejects reading a missing path or a folder with a StoreError naming the path', async () => {
    const store = dropped();
    await expect(store.read('synth/user/zz.aif')).rejects.toThrow(StoreError);
    await expect(store.read('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
    await expect(store.read('synth/user')).rejects.toThrow(StoreError);
    await expect(store.size('synth/user/zz.aif')).rejects.toThrow('synth/user/zz.aif');
  });

  it('reports whether files and folders exist', async () => {
    const store = dropped();
    expect(await store.exists('synth/user/a.aif')).toBe(true);
    expect(await store.exists('synth')).toBe(true);
    expect(await store.exists('')).toBe(true);
    expect(await store.exists('synth/user/zz.aif')).toBe(false);
    expect(await store.exists('OP-1/synth')).toBe(false);
  });

  it('rejects every change as a read-only source, naming the path', async () => {
    const store = dropped();
    await expect(store.write('x.aif', Uint8Array.of(1))).rejects.toThrow(StoreError);
    await expect(store.write('x.aif', Uint8Array.of(1))).rejects.toThrow('read-only source: cannot change x.aif');
    await expect(store.move('synth/user/a.aif', 'synth/user/c.aif')).rejects.toThrow('read-only source');
    await expect(store.remove('synth/user/a.aif')).rejects.toThrow('read-only source');
    await expect(store.mkdir('synth/new')).rejects.toThrow('read-only source');
    expect(await store.exists('x.aif')).toBe(false);
    expect(await store.exists('synth/user/a.aif')).toBe(true);
  });

  it('accepts an empty drop with an empty label', async () => {
    const store = new DropStore([]);
    expect(store.label).toBe('');
    expect(await store.list('')).toEqual([]);
  });

  it('treats several top-level folders as the contents of the drive root', async () => {
    const store = new DropStore([
      { path: 'synth/user/a.aif', file: file('a.aif', 1) },
      { path: 'drum/user/kit.aif', file: file('kit.aif', 4) },
    ]);
    expect(store.label).toBe('OP-1');
    expect(await store.list('')).toEqual([
      { name: 'drum', kind: 'dir' },
      { name: 'synth', kind: 'dir' },
    ]);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1));
  });

  it('keeps a single dropped OP-1 drive folder (synth, drum, tape, album, mods) as the drive root content', async () => {
    for (const name of ['synth', 'Drum', 'TAPE', 'album', 'mods']) {
      const store = new DropStore([{ path: `${name}/user/a.aif`, file: file('a.aif', 1) }]);
      expect(store.label).toBe('OP-1');
      expect(await store.list('')).toEqual([{ name, kind: 'dir' }]);
      expect(await store.read(`${name}/user/a.aif`)).toEqual(Uint8Array.of(1));
    }
  });

  it('still strips a single dropped folder with any other name', async () => {
    const store = new DropStore([{ path: 'synths/synth/user/a.aif', file: file('a.aif', 1) }]);
    expect(store.label).toBe('synths');
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1));
  });

  it('takes the label for a multi-folder drop from the options', () => {
    const store = new DropStore(
      [
        { path: 'synth/user/a.aif', file: file('a.aif') },
        { path: 'drum/user/kit.aif', file: file('kit.aif') },
      ],
      { label: 'my drive' },
    );
    expect(store.label).toBe('my drive');
  });

  it('lets the label option override the dropped folder name', () => {
    expect(new DropStore([{ path: 'OP-1/synth/user/a.aif', file: file('a.aif') }], { label: 'x' }).label).toBe('x');
  });

  it('refuses loose top-level files mixed in with folders', () => {
    expect(
      () =>
        new DropStore([
          { path: 'synth/user/a.aif', file: file('a.aif') },
          { path: 'readme.txt', file: file('readme.txt') },
        ]),
    ).toThrow(StoreError);
  });

  it('refuses loose files that are not inside a folder', () => {
    expect(() => new DropStore([{ path: 'a.aif', file: file('a.aif') }])).toThrow(StoreError);
  });
});

describe('fromInputFiles', () => {
  it('builds a store from the webkitRelativePath of each file', async () => {
    const withPath = (f: File, path: string): File => Object.defineProperty(f, 'webkitRelativePath', { value: path });
    const store = fromInputFiles([
      withPath(file('a.aif', 1), 'drive/synth/user/a.aif'),
      withPath(file('kit.aif', 4), 'drive/drum/user/kit.aif'),
    ]);
    expect(store.label).toBe('drive');
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1));
    expect(await store.list('drum/user')).toEqual([{ name: 'kit.aif', kind: 'file' }]);
  });
});

describe('filesFromDataTransfer', () => {
  type Entry = FakeFileEntry | FakeDirectoryEntry;
  type Fail = (error: DOMException) => void;
  interface FakeFileEntry {
    isFile: true;
    isDirectory: false;
    name: string;
    fullPath: string;
    file(success: (f: File) => void, error: Fail): void;
  }
  interface FakeDirectoryEntry {
    isFile: false;
    isDirectory: true;
    name: string;
    fullPath: string;
    createReader(): { readEntries(success: (entries: Entry[]) => void, error: Fail): void };
  }

  const nameOf = (fullPath: string): string => fullPath.split('/').at(-1) ?? '';

  const fileEntry = (fullPath: string, ...values: number[]): FakeFileEntry => ({
    isFile: true,
    isDirectory: false,
    name: nameOf(fullPath),
    fullPath,
    file: (success) => success(file(nameOf(fullPath), ...values)),
  });

  const unreadableFile = (fullPath: string): FakeFileEntry => ({
    ...fileEntry(fullPath),
    file: (_success, error) => setTimeout(() => error(new DOMException('gone', 'NotFoundError')), 0),
  });

  /** Hands out children two at a time, like Chromium's 100-entry batches. */
  const dirEntry = (fullPath: string, children: Entry[]): FakeDirectoryEntry => ({
    isFile: false,
    isDirectory: true,
    name: nameOf(fullPath),
    fullPath,
    createReader: () => {
      let offset = 0;
      return {
        readEntries: (success) => {
          const batch = children.slice(offset, offset + 2);
          offset += batch.length;
          setTimeout(() => success(batch), 0);
        },
      };
    },
  });

  const unreadableDir = (fullPath: string): FakeDirectoryEntry => ({
    ...dirEntry(fullPath, []),
    createReader: () => ({
      readEntries: (_success, error) => setTimeout(() => error(new DOMException('denied', 'SecurityError')), 0),
    }),
  });

  const transfer = (items: Array<{ kind: string; entry: Entry | null }>): DataTransfer =>
    ({ items: items.map(({ kind, entry }) => ({ kind, webkitGetAsEntry: () => entry })) }) as unknown as DataTransfer;

  it('walks dropped folders recursively, keeping paths that start with the folder name', async () => {
    const tree = dirEntry('/OP-1', [
      dirEntry('/OP-1/synth', [
        dirEntry('/OP-1/synth/user', [
          fileEntry('/OP-1/synth/user/a.aif', 1),
          fileEntry('/OP-1/synth/user/b.aif', 2),
          fileEntry('/OP-1/synth/user/c.aif', 3),
        ]),
      ]),
      dirEntry('/OP-1/tape', []),
      fileEntry('/OP-1/readme.txt', 9),
    ]);
    const { files, skipped } = await filesFromDataTransfer(
      transfer([
        { kind: 'string', entry: null },
        { kind: 'file', entry: tree },
      ]),
    );
    expect(skipped).toEqual([]);
    expect(files.map((f) => f.path).sort()).toEqual([
      'OP-1/readme.txt',
      'OP-1/synth/user/a.aif',
      'OP-1/synth/user/b.aif',
      'OP-1/synth/user/c.aif',
    ]);
    const store = new DropStore(files);
    expect(store.label).toBe('OP-1');
    expect(await store.read('synth/user/c.aif')).toEqual(Uint8Array.of(3));
  });

  it('accepts several folders dropped together as the drive root', async () => {
    const { files } = await filesFromDataTransfer(
      transfer([
        { kind: 'file', entry: dirEntry('/synth', [dirEntry('/synth/user', [fileEntry('/synth/user/a.aif', 1)])]) },
        { kind: 'file', entry: dirEntry('/drum', [dirEntry('/drum/user', [fileEntry('/drum/user/k.aif', 2)])]) },
      ]),
    );
    const store = new DropStore(files);
    expect(store.label).toBe('OP-1');
    expect(await store.read('drum/user/k.aif')).toEqual(Uint8Array.of(2));
  });

  it('skips files and folders that cannot be read, reporting their paths', async () => {
    const tree = dirEntry('/OP-1', [
      dirEntry('/OP-1/synth', [
        dirEntry('/OP-1/synth/user', [fileEntry('/OP-1/synth/user/a.aif', 1), unreadableFile('/OP-1/synth/user/b.aif')]),
        unreadableDir('/OP-1/synth/locked'),
      ]),
    ]);
    const { files, skipped } = await filesFromDataTransfer(transfer([{ kind: 'file', entry: tree }]));
    expect(files.map((f) => f.path)).toEqual(['OP-1/synth/user/a.aif']);
    expect(skipped.sort()).toEqual(['OP-1/synth/locked', 'OP-1/synth/user/b.aif']);
  });

  it('skips items without an entry', async () => {
    expect(await filesFromDataTransfer(transfer([{ kind: 'file', entry: null }]))).toEqual({ files: [], skipped: [] });
  });
});
