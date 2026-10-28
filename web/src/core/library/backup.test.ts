import { describe, it, expect } from 'vitest';
import { createHash } from 'node:crypto';
import { unzipSync, zipSync, strFromU8, strToU8, type Unzipped } from 'fflate';
import {
  createBackup,
  readBackup,
  diffBackup,
  applyRestore,
  BackupError,
  RestoreError,
  MANIFEST_FILE,
  type BackupManifest,
} from './backup';
import { MemoryStore } from '../store/memory-store';
import { StoreError } from '../store/store';
import { op1Stock } from '../profiles/op1-stock';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { expectSameBytes, expectSameFiles } from '../../../test/same-bytes';
import { nonCanonical } from '../../../test/non-canonical';

const b = (...values: number[]): Uint8Array => Uint8Array.from(values);
const sha = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');
const NOW = new Date('2026-10-28T12:34:56.000Z');
const now = (): Date => NOW;

const realDrum = fixtureBytes('real/drum.aif');
const prettyDrum = nonCanonical(realDrum);
const cluster = fixtureBytes('synth-cluster.aif');

const driveFiles = (): Record<string, Uint8Array> => ({
  'synth/user/cluster.aif': cluster,
  'synth/user/notes.txt': b(1, 2),
  'synth/user/.co-op-tmp-x.aif': b(9),
  'synth/.hidden/h.aif': b(9),
  'synth/deep/er/z.aif': b(3),
  'drum/user/808.aif': prettyDrum,
  'drum/user/real.aif': realDrum,
  'tape/track1.aif': b(5),
});
const backedUp = [
  'drum/user/808.aif',
  'drum/user/real.aif',
  'synth/deep/er/z.aif',
  'synth/user/cluster.aif',
  'synth/user/notes.txt',
];

const drive = (): MemoryStore => new MemoryStore(driveFiles(), { label: 'OP-1' });

const bytesOf = async (blob: Blob): Promise<Uint8Array> => new Uint8Array(await blob.arrayBuffer());
const backupZip = async (store: MemoryStore = drive(), paths?: string[]): Promise<Uint8Array> =>
  bytesOf(await createBackup(store, op1Stock, paths ? { now, paths } : { now }));

const manifestOf = (entries: Unzipped): BackupManifest =>
  JSON.parse(strFromU8(entries[MANIFEST_FILE] as Uint8Array)) as BackupManifest;

/** Rebuilds a backup zip after `edit` changes its entries. */
const tamper = (zip: Uint8Array, edit: (entries: Unzipped) => void): Uint8Array => {
  const entries = unzipSync(zip);
  edit(entries);
  return zipSync(entries);
};

/** A hand-made backup zip listing `files` (path to bytes) under the given profile. */
const handMade = (files: Record<string, Uint8Array>, profile = 'op1/stock', extra: Record<string, Uint8Array> = {}) => {
  const manifest: BackupManifest = {
    format: 1,
    createdAt: NOW.toISOString(),
    profile,
    label: 'x',
    files: Object.entries(files).map(([path, bytes]) => ({ path, size: bytes.length, sha256: sha(bytes) })),
  };
  return zipSync({ ...files, ...extra, [MANIFEST_FILE]: strToU8(JSON.stringify(manifest)) });
};

describe('createBackup', () => {
  it('returns a zip Blob', async () => {
    const blob = await createBackup(drive(), op1Stock, { now });
    expect(blob).toBeInstanceOf(Blob);
    expect(blob.type).toBe('application/zip');
  });

  it('zips every non-dot file under the patch roots, at any depth, with raw bytes', async () => {
    const entries = unzipSync(await backupZip());
    expect(Object.keys(entries).sort()).toEqual([...backedUp, MANIFEST_FILE].sort());
    for (const path of backedUp) expectSameBytes(entries[path], driveFiles()[path] as Uint8Array, path);
  });

  it('writes a manifest with format, time, profile, label and per-file size and sha256', async () => {
    const manifest = manifestOf(unzipSync(await backupZip()));
    expect(manifest).toEqual({
      format: 1,
      createdAt: '2026-10-28T12:34:56.000Z',
      profile: 'op1/stock',
      label: 'OP-1',
      files: backedUp.map((path) => {
        const bytes = driveFiles()[path] as Uint8Array;
        return { path, size: bytes.length, sha256: sha(bytes) };
      }),
    });
  });

  it('is deterministic for a given time', async () => {
    expectSameBytes(await backupZip(), await backupZip(), 'backup zip');
  });

  it('reports progress per file', async () => {
    const progress: Array<[number, number]> = [];
    await createBackup(drive(), op1Stock, { now, onProgress: (done, total) => progress.push([done, total]) });
    expect(progress).toEqual([[1, 5], [2, 5], [3, 5], [4, 5], [5, 5]]);
  });

  it('works on a read-only store', async () => {
    const store = new MemoryStore(driveFiles(), { write: false });
    expect(manifestOf(unzipSync(await backupZip(store))).files).toHaveLength(5);
  });

  it('backs up only the selected paths', async () => {
    const entries = unzipSync(await backupZip(drive(), ['synth/user/notes.txt', 'drum/user/808.aif']));
    expect(Object.keys(entries).sort()).toEqual([MANIFEST_FILE, 'drum/user/808.aif', 'synth/user/notes.txt']);
    expect(manifestOf(entries).files.map((f) => f.path)).toEqual(['drum/user/808.aif', 'synth/user/notes.txt']);
  });

  it.each(['tape/track1.aif', 'synth/user/none.aif', 'synth/user/.co-op-tmp-x.aif'])(
    'refuses the selected path %j, which is not a backed-up file under the patch roots',
    async (path) => {
      await expect(createBackup(drive(), op1Stock, { now, paths: [path] })).rejects.toThrow(BackupError);
    },
  );
});

describe('readBackup', () => {
  it('returns the manifest and every listed file, from bytes or a Blob', async () => {
    const zip = await backupZip();
    for (const input of [zip, new Blob([zip as Uint8Array<ArrayBuffer>])]) {
      const backup = await readBackup(input);
      expect(backup.manifest.createdAt).toBe('2026-10-28T12:34:56.000Z');
      expect([...backup.files.keys()]).toEqual(backedUp);
      expectSameBytes(backup.files.get('drum/user/808.aif'), prettyDrum, '808');
    }
  });

  it('never returns entries the manifest does not list', async () => {
    const zip = handMade({ 'synth/user/a.aif': b(1) }, 'op1/stock', { 'synth/user/extra.aif': b(2) });
    expect([...(await readBackup(zip)).files.keys()]).toEqual(['synth/user/a.aif']);
  });

  it('rejects something that is not a zip', async () => {
    await expect(readBackup(b(1, 2, 3, 4))).rejects.toThrow(BackupError);
  });

  it('rejects a zip without a manifest', async () => {
    await expect(readBackup(zipSync({ 'synth/user/a.aif': b(1) }))).rejects.toThrow(/co-op-backup\.json/);
  });

  it('rejects a manifest that is not JSON', async () => {
    await expect(readBackup(zipSync({ [MANIFEST_FILE]: strToU8('{nope') }))).rejects.toThrow(BackupError);
  });

  it('rejects an unknown format', async () => {
    const zip = tamper(await backupZip(), (e) => {
      e[MANIFEST_FILE] = strToU8(JSON.stringify({ ...manifestOf(e), format: 2 }));
    });
    await expect(readBackup(zip)).rejects.toThrow(/format/);
  });

  it('rejects a backup with a listed file missing', async () => {
    const zip = tamper(await backupZip(), (e) => {
      delete e['synth/user/notes.txt'];
    });
    await expect(readBackup(zip)).rejects.toThrow(/synth\/user\/notes\.txt/);
  });

  it('rejects a tampered file whose sha256 no longer matches', async () => {
    const zip = tamper(await backupZip(), (e) => {
      const bytes = (e['drum/user/808.aif'] as Uint8Array).slice();
      bytes[bytes.length - 1] = (bytes[bytes.length - 1] as number) ^ 0xff;
      e['drum/user/808.aif'] = bytes;
    });
    await expect(readBackup(zip)).rejects.toThrow(BackupError);
    await expect(readBackup(zip)).rejects.toThrow(/drum\/user\/808\.aif/);
  });

  it.each(['../evil.aif', '/abs.aif', 'synth/../x.aif', 'synth/.hidden.aif', ''])(
    'rejects the unsafe path %j',
    async (path) => {
      await expect(readBackup(handMade({ [path]: b(1) }))).rejects.toThrow(BackupError);
    },
  );

  it.each(['tape/track1.aif', 'album/side_a.aif', 'stray.aif'])(
    'rejects %j, which is outside the profile patch roots',
    async (path) => {
      await expect(readBackup(handMade({ [path]: b(1) }))).rejects.toThrow(/outside/);
    },
  );

  it('rejects paths that differ only in case', async () => {
    const zip = handMade({ 'synth/user/a.aif': b(1), 'synth/User/A.aif': b(2) });
    await expect(readBackup(zip)).rejects.toThrow(/case/);
  });

  it('accepts any folder path for a profile it does not know, but not a top-level file', async () => {
    const ok = handMade({ 'presets/a.aif': b(1) }, 'future/device');
    expect([...(await readBackup(ok)).files.keys()]).toEqual(['presets/a.aif']);
    await expect(readBackup(handMade({ 'a.aif': b(1) }, 'future/device'))).rejects.toThrow(/outside/);
  });
});

describe('diffBackup and applyRestore', () => {
  /** Backs up the drive, then changes one file, adds one and deletes one. */
  const scenario = async () => {
    const store = drive();
    const backup = await readBackup(await backupZip(store));
    await store.write('synth/user/cluster.aif', b(0));
    await store.write('drum/user/added.aif', b(7));
    await store.remove('drum/user/808.aif');
    return { store, backup };
  };

  it('classifies backed-up files and lists device-only files under the patch roots', async () => {
    const { store, backup } = await scenario();
    expect(await diffBackup(store, backup)).toEqual({
      entries: [
        { path: 'drum/user/808.aif', status: 'new' },
        { path: 'drum/user/real.aif', status: 'same' },
        { path: 'synth/deep/er/z.aif', status: 'same' },
        { path: 'synth/user/cluster.aif', status: 'changed' },
        { path: 'synth/user/notes.txt', status: 'same' },
      ],
      onDeviceOnly: ['drum/user/added.aif'],
    });
  });

  it('reports a backed-up path that is now a folder on the device as changed', async () => {
    const backup = await readBackup(handMade({ 'synth/user/a.aif': b(1) }));
    const store = new MemoryStore({ 'synth/user/a.aif/inner.aif': b(2) });
    expect((await diffBackup(store, backup)).entries).toEqual([{ path: 'synth/user/a.aif', status: 'changed' }]);
  });

  it('uses the top-level folders of the backup for a profile it does not know', async () => {
    const backup = await readBackup(handMade({ 'presets/a.aif': b(1) }, 'future/device'));
    const store = new MemoryStore({ 'presets/a.aif': b(1), 'presets/b.aif': b(2), 'synth/user/c.aif': b(3) });
    expect(await diffBackup(store, backup)).toEqual({
      entries: [{ path: 'presets/a.aif', status: 'same' }],
      onDeviceOnly: ['presets/b.aif'],
    });
  });

  it('restores only the selected paths, byte for byte, deleting nothing, reporting each path', async () => {
    const { store, backup } = await scenario();
    const restored: string[] = [];
    await applyRestore(store, backup, ['drum/user/808.aif', 'synth/user/cluster.aif'], (path) => restored.push(path));
    expect(restored).toEqual(['drum/user/808.aif', 'synth/user/cluster.aif']);
    const expected = driveFiles();
    expected['drum/user/added.aif'] = b(7);
    expectSameFiles(store.snapshot(), expected);
    expect((await diffBackup(store, backup)).entries.every((e) => e.status === 'same')).toBe(true);
  });

  it('leaves unselected changes alone', async () => {
    const { store, backup } = await scenario();
    await applyRestore(store, backup, ['drum/user/808.aif']);
    expect(store.snapshot()['synth/user/cluster.aif']).toEqual(b(0));
  });

  it('rejects a path that is not in the backup before writing anything', async () => {
    const { store, backup } = await scenario();
    const before = store.snapshot();
    await expect(applyRestore(store, backup, ['drum/user/808.aif', 'synth/user/none.aif'])).rejects.toThrow(
      /synth\/user\/none\.aif/,
    );
    expectSameFiles(store.snapshot(), before);
  });

  it('reports the paths already restored when a write fails part-way', async () => {
    class FailOn extends MemoryStore {
      override async write(path: string, bytes: Uint8Array): Promise<void> {
        if (path.endsWith('cluster.aif')) throw new StoreError('disk full');
        return super.write(path, bytes);
      }
    }
    const backup = await readBackup(await backupZip());
    const store = new FailOn({});
    const error = await applyRestore(store, backup, ['drum/user/808.aif', 'synth/user/cluster.aif', 'synth/user/notes.txt'])
      .catch((e: unknown) => e);
    expect(error).toBeInstanceOf(RestoreError);
    expect(error).toBeInstanceOf(BackupError);
    expect((error as RestoreError).restored).toEqual(['drum/user/808.aif']);
    expect((error as Error).message).toMatch(/synth\/user\/cluster\.aif/);
    expect((error as Error).cause).toBeInstanceOf(StoreError);
  });

  it('refuses a read-only store', async () => {
    const backup = await readBackup(await backupZip());
    const store = new MemoryStore({}, { write: false });
    await expect(applyRestore(store, backup, ['drum/user/808.aif'])).rejects.toThrow(StoreError);
  });

  it('carries a non-canonical real device file through backup and restore byte for byte', async () => {
    const source = new MemoryStore({ 'drum/user/808.aif': prettyDrum, 'drum/user/real.aif': realDrum });
    const backup = await readBackup(await backupZip(source));
    const target = new MemoryStore({ 'drum/user/808.aif': b(1) });
    await applyRestore(target, backup, ['drum/user/808.aif', 'drum/user/real.aif']);
    const files = target.snapshot();
    expect(Object.keys(files)).toEqual(['drum/user/808.aif', 'drum/user/real.aif']);
    expectSameBytes(files['drum/user/808.aif'], prettyDrum, 'non-canonical drum');
    expectSameBytes(files['drum/user/real.aif'], realDrum, 'real drum');
  });
});
