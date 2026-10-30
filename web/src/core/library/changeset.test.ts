import { describe, it, expect } from 'vitest';
import { unzipSync, strFromU8 } from 'fflate';
import { StagedStore, countChanges, exportChangeSet, CHANGES_FILE, type Change } from './changeset';
import { copyPatch, deletePatch, ensureFolder, movePatch, renameFile, setPatchName } from './operations';
import { MemoryStore } from '../store/memory-store';
import { StoreError } from '../store/store';
import { writeAtomic } from '../store/atomic';
import { readPatch } from '../codec/patch';
import { describeWritableStore } from '../../../test/store-contract';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { expectSameBytes, expectSameFiles } from '../../../test/same-bytes';
import { nonCanonical } from '../../../test/non-canonical';

const b = (...values: number[]): Uint8Array => Uint8Array.from(values);
const cluster = fixtureBytes('synth-cluster.aif');
const prettyDrum = nonCanonical(fixtureBytes('real/drum.aif'));

const baseFiles = (): Record<string, Uint8Array> => ({
  'synth/user/cluster.aif': cluster,
  'synth/user/old.aif': b(1),
  'drum/user/808.aif': prettyDrum,
  'tape/track1.aif': b(5),
});

const staged = (): { base: MemoryStore; store: StagedStore } => {
  const base = new MemoryStore(baseFiles(), { write: false, label: 'OP-1' });
  return { base, store: new StagedStore(base) };
};

const summary = (changes: Change[]): string[] => changes.map((c) => `${c.op} ${c.path}`);

// A staged store over an empty read-only base must behave like any writable store.
describeWritableStore('StagedStore', (files) => {
  const store = new StagedStore(new MemoryStore({}, { write: false }));
  return (async () => {
    for (const [path, bytes] of Object.entries(files)) await store.write(path, bytes);
    return store;
  })();
});

// ...and over a base holding the files, where every change is staged against base entries.
describeWritableStore('StagedStore over a populated base', (files) => new StagedStore(new MemoryStore(files, { write: false })));

describe('StagedStore', () => {
  it('is writable, keeps the base label, and never changes the base', async () => {
    const { base, store } = staged();
    expect(store.capabilities.write).toBe(true);
    expect(store.label).toBe('OP-1');
    await store.write('synth/user/new.aif', b(9));
    await store.remove('synth/user/old.aif');
    expectSameFiles(base.snapshot(), baseFiles());
  });

  it('reads staged bytes first, then the base, and hides removed files', async () => {
    const { store } = staged();
    await store.write('synth/user/old.aif', b(7, 7));
    expect(await store.read('synth/user/old.aif')).toEqual(b(7, 7));
    expect(await store.size('synth/user/old.aif')).toBe(2);
    expect(await store.read('tape/track1.aif')).toEqual(b(5));
    await store.remove('tape/track1.aif');
    expect(await store.exists('tape/track1.aif')).toBe(false);
    await expect(store.read('tape/track1.aif')).rejects.toThrow(StoreError);
    expect(await store.list('tape')).toEqual([]);
    expect(await store.exists('tape')).toBe(true);
  });

  it('lists base and staged entries merged in code-point order', async () => {
    const { store } = staged();
    await store.write('synth/user/A.aif', b(1));
    await store.mkdir('synth/pads');
    await store.remove('synth/user/old.aif');
    expect(await store.list('synth')).toEqual([
      { name: 'pads', kind: 'dir' },
      { name: 'user', kind: 'dir' },
    ]);
    expect(await store.list('synth/user')).toEqual([
      { name: 'A.aif', kind: 'file' },
      { name: 'cluster.aif', kind: 'file' },
    ]);
  });

  it('starts with no changes', () => {
    expect(staged().store.changes()).toEqual([]);
  });
});

describe('changes()', () => {
  it('reduces to the final state: one write per changed file, one remove per deleted base file', async () => {
    const { store } = staged();
    await store.write('synth/user/new.aif', b(1));
    await store.write('synth/user/new.aif', b(2));
    await store.remove('synth/user/old.aif');
    await store.write('synth/user/temp.aif', b(3));
    await store.remove('synth/user/temp.aif');
    expect(summary(store.changes())).toEqual(['write synth/user/new.aif', 'remove synth/user/old.aif']);
    expect(store.changes()[0]).toEqual({ op: 'write', path: 'synth/user/new.aif', bytes: b(2) });
  });

  it('turns a written then removed base file into a single remove', async () => {
    const { store } = staged();
    await store.write('synth/user/old.aif', b(4));
    await store.remove('synth/user/old.aif');
    expect(store.changes()).toEqual([{ op: 'remove', path: 'synth/user/old.aif' }]);
  });

  it('turns a removed then rewritten base file into a single write', async () => {
    const { store } = staged();
    await store.remove('synth/user/old.aif');
    await store.write('synth/user/old.aif', b(4));
    expect(summary(store.changes())).toEqual(['write synth/user/old.aif']);
  });

  it('drops a write that leaves a base file byte-identical', async () => {
    const { store } = staged();
    await writeAtomic(store, 'synth/user/old.aif', b(1));
    expect(store.changes()).toEqual([]);
  });

  it('records the minimal changes for every library operation', async () => {
    const { store } = staged();
    await copyPatch(store, 'drum/user/808.aif', 'drum/backup');
    await movePatch(store, 'synth/user/old.aif', 'synth/archive');
    await renameFile(store, 'tape/track1.aif', 'track2.aif');
    await setPatchName(store, 'synth/user/cluster.aif', 'renamed');
    await ensureFolder(store, 'synth', 'pads');
    await deletePatch(store, 'synth/archive/old.aif');
    const changes = store.changes();
    expect(summary(changes)).toEqual([
      'write drum/backup/808.aif',
      'mkdir synth/pads',
      'write synth/user/cluster.aif',
      'remove synth/user/old.aif',
      'remove tape/track1.aif',
      'write tape/track2.aif',
    ]);
    const byPath = new Map(changes.map((c) => [c.path, c]));
    const copy = byPath.get('drum/backup/808.aif');
    expectSameBytes(copy?.op === 'write' ? copy.bytes : undefined, prettyDrum, 'copied drum');
    const named = byPath.get('synth/user/cluster.aif');
    expect(readPatch(named?.op === 'write' ? named.bytes : b()).name).toBe('renamed');
  });

  it('leaves no temporary files from atomic writes in the changes', async () => {
    const { store } = staged();
    await setPatchName(store, 'synth/user/cluster.aif', 'x');
    expect(store.changes().map((c) => c.path)).toEqual(['synth/user/cluster.aif']);
  });
});
describe('removeFolder on a staged source', () => {
  const staged = () => new StagedStore(new MemoryStore({ 'synth/pads/a.aif': Uint8Array.of(1) }));

  it('lists a base folder for removal after its files, and tells the user to delete it', async () => {
    const store = staged();
    await store.remove('synth/pads/a.aif');
    await store.removeFolder('synth/pads');
    expect(await store.exists('synth/pads')).toBe(false);
    expect((await store.list('synth')).map((e) => e.name)).toEqual([]);
    expect(store.changes()).toEqual([
      { op: 'rmdir', path: 'synth/pads' },
      { op: 'remove', path: 'synth/pads/a.aif' },
    ]);
    const text = strFromU8(unzipSync(exportChangeSet(store.changes()))[CHANGES_FILE] as Uint8Array);
    expect(text).toContain('synth/pads/');
    expect(text).toContain('Then delete these folders');
  });

  it('leaves no trace of a folder made here, and undoes a removal when it is made again', async () => {
    const store = staged();
    await store.mkdir('drum/new');
    await store.removeFolder('drum/new');
    expect(store.changes()).toEqual([]);
    await store.remove('synth/pads/a.aif');
    await store.removeFolder('synth/pads');
    await store.mkdir('synth/pads');
    expect(store.changes()).toEqual([{ op: 'remove', path: 'synth/pads/a.aif' }]);
  });

  it('refuses a folder with files left', async () => {
    await expect(staged().removeFolder('synth/pads')).rejects.toThrow(StoreError);
  });
});

describe('exportChangeSet', () => {
  const textOf = (zip: Uint8Array): string => strFromU8(unzipSync(zip)[CHANGES_FILE] as Uint8Array);

  it('zips every written file at its path plus CHANGES.txt', async () => {
    const { store } = staged();
    await copyPatch(store, 'drum/user/808.aif', 'drum/backup');
    await deletePatch(store, 'synth/user/old.aif');
    const entries = unzipSync(exportChangeSet(store.changes()));
    expect(Object.keys(entries).sort()).toEqual([CHANGES_FILE, 'drum/backup/808.aif']);
    expectSameBytes(entries['drum/backup/808.aif'], prettyDrum, 'zipped drum');
  });

  it('lists deletions first, then each destination folder with the files to copy into it', () => {
    const text = textOf(
      exportChangeSet([
        { op: 'write', path: 'drum/backup/808.aif', bytes: b(1) },
        { op: 'write', path: 'synth/user/a.aif', bytes: b(2) },
        { op: 'write', path: 'synth/user/b.aif', bytes: b(3) },
        { op: 'remove', path: 'synth/user/old.aif' },
      ]),
    );
    const lines = text.split(/\r?\n/);
    const at = (needle: string): number => lines.findIndex((line) => line.trim() === needle);
    expect(at('synth/user/old.aif')).toBeGreaterThan(-1);
    expect(at('drum/backup/')).toBeGreaterThan(at('synth/user/old.aif'));
    expect(lines[at('drum/backup/') + 1]?.trim()).toBe('808.aif');
    expect(lines.slice(at('synth/user/') + 1, at('synth/user/') + 3).map((l) => l.trim())).toEqual(['a.aif', 'b.aif']);
    expect(text).not.toMatch(/copy these folders/i);
  });

  it('warns Mac users never to replace a folder', () => {
    const text = textOf(exportChangeSet([{ op: 'write', path: 'synth/user/a.aif', bytes: b(1) }]));
    expect(text).toContain(
      'On a Mac, never choose Replace for a folder; copy the files themselves, or hold Option and choose Merge.',
    );
  });

  it('says there is nothing to delete when there are no removals', () => {
    const entries = unzipSync(exportChangeSet([{ op: 'write', path: 'synth/user/a.aif', bytes: b(1) }]));
    expect(Object.keys(entries).sort()).toEqual([CHANGES_FILE, 'synth/user/a.aif']);
    expect(strFromU8(entries[CHANGES_FILE] as Uint8Array)).toMatch(/nothing to delete/i);
  });

  it('carries new empty folders as directory entries and lists them', async () => {
    const { store } = staged();
    await ensureFolder(store, 'synth', 'pads');
    await store.mkdir('drum/new/deeper');
    await store.mkdir('synth/user');
    expect(summary(store.changes())).toEqual(['mkdir drum/new/deeper', 'mkdir synth/pads']);
    const zip = exportChangeSet(store.changes());
    expect(Object.keys(unzipSync(zip)).sort()).toEqual([CHANGES_FILE, 'drum/new/deeper/', 'synth/pads/']);
    const lines = textOf(zip).split(/\r?\n/).map((l) => l.trim());
    expect(lines).toContain('drum/new/deeper/');
    expect(lines).toContain('synth/pads/');
  });

  it('does not list a made folder that ends up holding staged files', async () => {
    const { store } = staged();
    await ensureFolder(store, 'synth', 'pads');
    await copyPatch(store, 'synth/user/cluster.aif', 'synth/pads');
    expect(summary(store.changes())).toEqual(['write synth/pads/cluster.aif']);
  });

  it('is deterministic for a given time', () => {
    const changes: Change[] = [
      { op: 'write', path: 'synth/user/a.aif', bytes: b(1) },
      { op: 'mkdir', path: 'synth/pads' },
    ];
    const now = (): Date => new Date('2026-10-28T12:00:00Z');
    expectSameBytes(exportChangeSet(changes, now), exportChangeSet(changes, now), 'zip');
    const later = (): Date => new Date('2027-01-01T00:00:00Z');
    expect(exportChangeSet(changes, later)).not.toEqual(exportChangeSet(changes, now));
  });
});

describe('case-only renames on a staged source', () => {
  it('keep the file: the removal of the old spelling is dropped', async () => {
    const base = new MemoryStore({ 'drum/user/kick.aif': prettyDrum }, { write: false });
    const store = new StagedStore(base);
    await renameFile(store, 'drum/user/kick.aif', 'Kick.aif');
    const changes = store.changes();
    expect(summary(changes)).toEqual(['write drum/user/Kick.aif', 'rename drum/user/kick.aif']);
    const text = strFromU8(unzipSync(exportChangeSet(changes))[CHANGES_FILE] as Uint8Array);
    expect(text).toMatch(/nothing to delete/i);
    expect(text).not.toMatch(/^\s*drum\/user\/kick\.aif\s*$/m);
  });

  it('are reported as a rename to make by hand, after the copy step', async () => {
    const base = new MemoryStore({ 'drum/user/kick.aif': prettyDrum }, { write: false });
    const store = new StagedStore(base);
    await renameFile(store, 'drum/user/kick.aif', 'Kick.aif');
    expect(store.changes().filter((c) => c.op === 'rename')).toEqual([
      { op: 'rename', path: 'drum/user/kick.aif', to: 'Kick.aif' },
    ]);
    const text = strFromU8(unzipSync(exportChangeSet(store.changes()))[CHANGES_FILE] as Uint8Array);
    const lines = text.split(/\r?\n/);
    const heading = lines.findIndex((l) =>
      l.includes(
        'Rename these files on the OP-1 drive by hand (the drive keeps the old spelling when a file is replaced):',
      ),
    );
    expect(heading).toBeGreaterThan(lines.findIndex((l) => l.includes('Copy the files from this zip')));
    expect(lines[heading + 1]?.trim()).toBe('drum/user/kick.aif -> Kick.aif');
  });

  it('count as one change with their write', async () => {
    const base = new MemoryStore({ 'drum/user/kick.aif': prettyDrum, 'drum/user/snare.aif': b(1) }, { write: false });
    const store = new StagedStore(base);
    await renameFile(store, 'drum/user/kick.aif', 'Kick.aif');
    expect(countChanges(store.changes())).toBe(1);
    await deletePatch(store, 'drum/user/snare.aif');
    expect(countChanges(store.changes())).toBe(2);
    expect(countChanges([])).toBe(0);
  });

  it('are not reported when nothing was renamed by case', () => {
    const text = strFromU8(
      unzipSync(exportChangeSet([{ op: 'write', path: 'synth/user/a.aif', bytes: b(1) }]))[CHANGES_FILE] as Uint8Array,
    );
    expect(text).not.toContain('Rename these files');
  });

  it('do not put the renamed file in the zip twice', async () => {
    const base = new MemoryStore({ 'drum/user/kick.aif': prettyDrum }, { write: false });
    const store = new StagedStore(base);
    await renameFile(store, 'drum/user/kick.aif', 'Kick.aif');
    expect(Object.keys(unzipSync(exportChangeSet(store.changes()))).sort()).toEqual([CHANGES_FILE, 'drum/user/Kick.aif']);
  });
});

describe('raw bytes through a staged source', () => {
  it('move and rename keep a non-canonical real file byte for byte', async () => {
    const { store } = staged();
    const moved = await movePatch(store, 'drum/user/808.aif', 'drum/moved');
    const renamed = await renameFile(store, moved, 'tr808.aif');
    const changes = store.changes();
    expect(summary(changes)).toEqual(['write drum/moved/tr808.aif', 'remove drum/user/808.aif']);
    const write = changes[0];
    expectSameBytes(write?.op === 'write' ? write.bytes : undefined, prettyDrum, renamed);
    expectSameBytes(unzipSync(exportChangeSet(changes))[renamed], prettyDrum, 'zipped');
  });
});
