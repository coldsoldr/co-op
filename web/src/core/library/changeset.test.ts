import { describe, it, expect } from 'vitest';
import { unzipSync, strFromU8 } from 'fflate';
import { StagedStore, exportChangeSet, CHANGES_FILE, type Change } from './changeset';
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

describe('exportChangeSet', () => {
  it('zips every written file at its path plus CHANGES.txt listing removals', async () => {
    const { store } = staged();
    await copyPatch(store, 'drum/user/808.aif', 'drum/backup');
    await deletePatch(store, 'synth/user/old.aif');
    const entries = unzipSync(exportChangeSet(store.changes()));
    expect(Object.keys(entries).sort()).toEqual([CHANGES_FILE, 'drum/backup/808.aif']);
    expectSameBytes(entries['drum/backup/808.aif'], prettyDrum, 'zipped drum');
    const text = strFromU8(entries[CHANGES_FILE] as Uint8Array);
    expect(text).toMatch(/copy these folders onto the OP-1 in disk mode, merging/i);
    expect(text).toMatch(/then delete the files listed below/i);
    expect(text).toContain('synth/user/old.aif');
  });

  it('says there is nothing to delete when there are no removals', () => {
    const entries = unzipSync(exportChangeSet([{ op: 'write', path: 'synth/user/a.aif', bytes: b(1) }]));
    expect(Object.keys(entries).sort()).toEqual([CHANGES_FILE, 'synth/user/a.aif']);
    expect(strFromU8(entries[CHANGES_FILE] as Uint8Array)).toMatch(/nothing to delete/i);
  });
});
