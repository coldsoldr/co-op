import { describe, it, expect } from 'vitest';
import {
  freeName,
  renameFile,
  setPatchName,
  movePatch,
  copyPatch,
  deletePatch,
  ensureFolder,
  OperationError,
} from './operations';
import { MemoryStore } from '../store/memory-store';
import { StoreError } from '../store/store';
import { readPatch } from '../codec/patch';
import { fixtureBytes } from '../../../test/fixture-bytes';
import { expectSameBytes, expectSameFiles } from '../../../test/same-bytes';
import { nonCanonical } from '../../../test/non-canonical';

const realDrum = fixtureBytes('real/drum.aif');
const realSynth = fixtureBytes('real/synth.aif');
const cluster = fixtureBytes('synth-cluster.aif');
const noMeta = fixtureBytes('no-metadata.aif');

const drive = (options: { write?: boolean } = {}): MemoryStore =>
  new MemoryStore(
    {
      'synth/user/cluster.aif': cluster,
      'synth/user/plain.aif': noMeta,
      'synth/real/cello.aif': realSynth,
      'drum/user/808.aif': realDrum,
      'drum/user/kit.aif': Uint8Array.from([9]),
      'drum/user/kit-2.aif': Uint8Array.from([8]),
    },
    options,
  );

const ssndOf = (bytes: Uint8Array): Uint8Array | undefined =>
  readPatch(bytes).form.chunks.find((c) => c.id === 'SSND')?.data;

describe('freeName', () => {
  it('returns the name itself when it is free', async () => {
    expect(await freeName(drive(), 'synth/user', 'new.aif')).toBe('new.aif');
    expect(await freeName(drive(), 'synth/empty', 'cluster.aif')).toBe('cluster.aif');
  });

  it('counts up from -2 past every taken name, keeping the extension', async () => {
    expect(await freeName(drive(), 'synth/user', 'cluster.aif')).toBe('cluster-2.aif');
    expect(await freeName(drive(), 'drum/user', 'kit.aif')).toBe('kit-3.aif');
  });

  it('appends the counter to names without an extension', async () => {
    const store = new MemoryStore({ 'synth/user/notes': Uint8Array.from([1]) });
    expect(await freeName(store, 'synth/user', 'notes')).toBe('notes-2');
  });
});

describe('renameFile', () => {
  it('renames within the folder, keeping the bytes, and returns the new path', async () => {
    const store = drive();
    expect(await renameFile(store, 'drum/user/808.aif', 'tr808.aif')).toBe('drum/user/tr808.aif');
    const files = store.snapshot();
    expect(files['drum/user/808.aif']).toBeUndefined();
    expectSameBytes(files['drum/user/tr808.aif'], realDrum);
  });

  it('returns the same path when the name is unchanged', async () => {
    const store = drive();
    expect(await renameFile(store, 'drum/user/808.aif', '808.aif')).toBe('drum/user/808.aif');
    expectSameBytes(store.snapshot()['drum/user/808.aif'], realDrum);
  });

  it.each(['', 'a/b.aif', '.hidden.aif', 'a\\b.aif'])('rejects the file name %j', async (name) => {
    const store = drive();
    await expect(renameFile(store, 'drum/user/808.aif', name)).rejects.toThrow(OperationError);
    expectSameBytes(store.snapshot()['drum/user/808.aif'], realDrum);
  });

  it('rejects an existing target', async () => {
    const store = drive();
    await expect(renameFile(store, 'drum/user/808.aif', 'kit.aif')).rejects.toThrow(/kit\.aif/);
    expect(store.snapshot()['drum/user/kit.aif']).toEqual(Uint8Array.from([9]));
  });
});

describe('setPatchName', () => {
  it('rewrites only the metadata name, keeping the audio', async () => {
    const store = drive();
    await setPatchName(store, 'synth/real/cello.aif', 'warm cello');
    const after = store.snapshot()['synth/real/cello.aif'] as Uint8Array;
    const patch = readPatch(after);
    expect(patch.name).toBe('warm cello');
    expect(patch.meta).toEqual({ ...readPatch(realSynth).meta, name: 'warm cello' });
    expectSameBytes(ssndOf(after), ssndOf(realSynth) as Uint8Array);
  });

  it('leaves no temporary files behind', async () => {
    const store = drive();
    await setPatchName(store, 'synth/user/cluster.aif', 'x');
    expect(Object.keys(store.snapshot()).filter((p) => p.includes('/.'))).toEqual([]);
  });

  it('accepts 24 characters and rejects 25 and empty names', async () => {
    const store = drive();
    await setPatchName(store, 'synth/user/cluster.aif', 'a'.repeat(24));
    await expect(setPatchName(store, 'synth/user/cluster.aif', 'a'.repeat(25))).rejects.toThrow(OperationError);
    await expect(setPatchName(store, 'synth/user/cluster.aif', '')).rejects.toThrow(OperationError);
    await expect(setPatchName(store, 'synth/user/cluster.aif', '   ')).rejects.toThrow(OperationError);
    expect(readPatch(store.snapshot()['synth/user/cluster.aif'] as Uint8Array).name).toBe('a'.repeat(24));
  });

  it('rejects a file with no op-1 metadata without changing it', async () => {
    const store = drive();
    await expect(setPatchName(store, 'synth/user/plain.aif', 'x')).rejects.toThrow(/no op-1 metadata/);
    expectSameBytes(store.snapshot()['synth/user/plain.aif'], noMeta);
  });
});

describe('movePatch', () => {
  it('moves the raw bytes unchanged and returns the new path', async () => {
    const store = drive();
    expect(await movePatch(store, 'drum/user/808.aif', 'drum/classics')).toBe('drum/classics/808.aif');
    const files = store.snapshot();
    expect(files['drum/user/808.aif']).toBeUndefined();
    expectSameBytes(files['drum/classics/808.aif'], realDrum);
  });

  it('picks a free name on collision', async () => {
    const store = drive();
    await store.write('drum/other/kit.aif', Uint8Array.from([7]));
    expect(await movePatch(store, 'drum/user/kit.aif', 'drum/other')).toBe('drum/other/kit-2.aif');
    expect(store.snapshot()['drum/other/kit.aif']).toEqual(Uint8Array.from([7]));
    expect(store.snapshot()['drum/other/kit-2.aif']).toEqual(Uint8Array.from([9]));
  });

  it('is a no-op when the target folder is the current folder', async () => {
    const store = drive();
    expect(await movePatch(store, 'drum/user/kit.aif', 'drum/user/')).toBe('drum/user/kit.aif');
    expect(Object.keys(store.snapshot())).toEqual(Object.keys(drive().snapshot()));
  });
});

describe('copyPatch', () => {
  it('copies a real device file byte for byte', async () => {
    const store = drive();
    expect(await copyPatch(store, 'drum/user/808.aif', 'drum/backup')).toBe('drum/backup/808.aif');
    const files = store.snapshot();
    expectSameBytes(files['drum/backup/808.aif'], realDrum);
    expectSameBytes(files['drum/user/808.aif'], realDrum);
  });

  it('copies into the same folder under a free name', async () => {
    const store = drive();
    expect(await copyPatch(store, 'synth/user/cluster.aif', 'synth/user')).toBe('synth/user/cluster-2.aif');
    expectSameBytes(store.snapshot()['synth/user/cluster-2.aif'], cluster);
  });
});

describe('deletePatch', () => {
  it('removes the file', async () => {
    const store = drive();
    await deletePatch(store, 'drum/user/kit.aif');
    expect(store.snapshot()['drum/user/kit.aif']).toBeUndefined();
  });

  it('propagates a missing file as a StoreError', async () => {
    await expect(deletePatch(drive(), 'drum/user/none.aif')).rejects.toThrow(StoreError);
  });
});

describe('ensureFolder', () => {
  it('creates a folder under the root and returns its path; existing folders are fine', async () => {
    const store = drive();
    expect(await ensureFolder(store, 'synth', 'pads')).toBe('synth/pads');
    expect(await store.list('synth')).toContainEqual({ name: 'pads', kind: 'dir' });
    expect(await ensureFolder(store, 'synth', 'user')).toBe('synth/user');
  });

  it.each(['', 'a/b', '.hidden'])('rejects the folder name %j', async (name) => {
    await expect(ensureFolder(drive(), 'synth', name)).rejects.toThrow(OperationError);
  });
});

describe('read-only stores', () => {
  const ops: Array<[string, (store: MemoryStore) => Promise<unknown>]> = [
    ['renameFile', (s) => renameFile(s, 'drum/user/kit.aif', 'x.aif')],
    ['setPatchName', (s) => setPatchName(s, 'synth/user/cluster.aif', 'x')],
    ['movePatch', (s) => movePatch(s, 'drum/user/kit.aif', 'drum/other')],
    ['copyPatch', (s) => copyPatch(s, 'drum/user/kit.aif', 'drum/other')],
    ['deletePatch', (s) => deletePatch(s, 'drum/user/kit.aif')],
    ['ensureFolder', (s) => ensureFolder(s, 'synth', 'pads')],
  ];

  it.each(ops)('%s refuses with a StoreError and changes nothing', async (_name, op) => {
    const store = drive({ write: false });
    await expect(op(store)).rejects.toThrow(StoreError);
    expectSameFiles(store.snapshot(), drive().snapshot());
  });
});

describe('raw bytes rule', () => {
  // A real device file whose metadata writePatch would rewrite: copy and move must not touch it.
  const prettyDrum = nonCanonical(realDrum);

  it('copy and move keep a non-canonical real file byte for byte', async () => {
    const store = new MemoryStore({ 'drum/user/808.aif': prettyDrum });
    const copied = await copyPatch(store, 'drum/user/808.aif', 'drum/copies');
    const moved = await movePatch(store, copied, 'drum/moved');
    const renamed = await renameFile(store, moved, 'tr808.aif');
    const files = store.snapshot();
    expectSameBytes(files['drum/user/808.aif'], prettyDrum, 'original');
    expectSameBytes(files[renamed], prettyDrum, renamed);
    expect(Object.keys(files)).toEqual(['drum/moved/tr808.aif', 'drum/user/808.aif']);
  });
});

// The OP-1 drive is FAT: names are case-insensitive. MemoryStore is case-sensitive, so these
// prove the checks live in the operations rather than in the store.
describe('case-insensitive names', () => {
  const caseDrive = (): MemoryStore =>
    new MemoryStore({
      'drum/user/kick.aif': Uint8Array.from([1]),
      'drum/user/Snare.aif': Uint8Array.from([2]),
      'drum/other/KICK.AIF': Uint8Array.from([3]),
    });

  it('freeName treats names differing only in case as taken', async () => {
    expect(await freeName(caseDrive(), 'drum/user', 'KICK.aif')).toBe('KICK-2.aif');
    expect(await freeName(caseDrive(), 'drum/user', 'snare.AIF')).toBe('snare-2.AIF');
  });

  it('renameFile rejects a target that differs from another file only in case', async () => {
    const store = caseDrive();
    await expect(renameFile(store, 'drum/user/kick.aif', 'snare.aif')).rejects.toThrow(OperationError);
    expect(Object.keys(store.snapshot())).toEqual(Object.keys(caseDrive().snapshot()));
  });

  it('renameFile allows a case-only rename of the same file, leaving no temporary', async () => {
    const store = caseDrive();
    expect(await renameFile(store, 'drum/user/kick.aif', 'Kick.aif')).toBe('drum/user/Kick.aif');
    const files = store.snapshot();
    expect(Object.keys(files)).toEqual(['drum/other/KICK.AIF', 'drum/user/Kick.aif', 'drum/user/Snare.aif']);
    expect(files['drum/user/Kick.aif']).toEqual(Uint8Array.from([1]));
  });

  it('movePatch and copyPatch pick a free name when the target differs only in case', async () => {
    const store = caseDrive();
    expect(await copyPatch(store, 'drum/user/kick.aif', 'drum/other')).toBe('drum/other/kick-2.aif');
    expect(await movePatch(store, 'drum/user/kick.aif', 'drum/other')).toBe('drum/other/kick-3.aif');
    expect(store.snapshot()['drum/other/KICK.AIF']).toEqual(Uint8Array.from([3]));
  });

  it('ensureFolder returns an existing folder that differs only in case', async () => {
    const store = caseDrive();
    expect(await ensureFolder(store, 'drum', 'USER')).toBe('drum/user');
    expect(await store.list('drum')).toEqual([
      { name: 'other', kind: 'dir' },
      { name: 'user', kind: 'dir' },
    ]);
  });
});

/** Refuses a move whose target matches an existing name in any case, as a FAT drive would. */
class FatMoveStore extends MemoryStore {
  override async move(from: string, to: string): Promise<void> {
    const name = to.split('/').at(-1)?.toLowerCase();
    const folder = to.split('/').slice(0, -1).join('/');
    if ((await this.list(folder)).some((e) => e.name.toLowerCase() === name)) {
      throw new StoreError(`cannot move ${from} to ${to}: target exists`);
    }
    return super.move(from, to);
  }
}

describe('case-only rename on a case-insensitive drive', () => {
  it('goes through a temporary name so the drive never sees a clash', async () => {
    const store = new FatMoveStore({ 'drum/user/kick.aif': Uint8Array.from([1]) });
    expect(await renameFile(store, 'drum/user/kick.aif', 'KICK.aif')).toBe('drum/user/KICK.aif');
    expect(Object.keys(store.snapshot())).toEqual(['drum/user/KICK.aif']);
  });
});
