import { describe, it, expect } from 'vitest';
import {
  removeFolder,
  freeName,
  renameFile,
  renamePatch,
  setPatchName,
  movePatch,
  copyPatch,
  deletePatch,
  ensureFolder,
  OperationError,
} from './operations';
import { MemoryStore } from '../store/memory-store';
import { StoreError } from '../store/store';
import { recoverStranded } from '../store/atomic';
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

describe('removeFolder', () => {
  const store = () => new MemoryStore({ 'synth/pads/a.aif': Uint8Array.of(1), 'synth/pads/.DS_Store': Uint8Array.of(2), 'synth/user/b.aif': Uint8Array.of(3) });

  it('removes the folder and every file in it', async () => {
    const drive = store();
    await removeFolder(drive, 'synth/pads');
    expect(await drive.exists('synth/pads')).toBe(false);
    expect(await drive.exists('synth/user/b.aif')).toBe(true);
  });

  it('never removes the preset folder, in any case', async () => {
    const drive = store();
    await expect(removeFolder(drive, 'synth/user')).rejects.toThrow(OperationError);
    await expect(removeFolder(drive, 'synth/USER')).rejects.toThrow(OperationError);
    expect(await drive.exists('synth/user/b.aif')).toBe(true);
  });

  it('deletes nothing from a folder holding a folder', async () => {
    const drive = store();
    await drive.mkdir('synth/pads/soft');
    await expect(removeFolder(drive, 'synth/pads')).rejects.toThrow(OperationError);
    expect(await drive.exists('synth/pads/a.aif')).toBe(true);
  });
});

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

  it('uses a temp name of its own per call, so concurrent copies of one file do not collide', async () => {
    const store = drive();
    const temps: string[] = [];
    const write = store.write.bind(store);
    store.write = async (path, bytes) => {
      temps.push(path);
      await write(path, bytes);
    };
    const copies = await Promise.all([
      copyPatch(store, 'synth/user/cluster.aif', 'synth/user'),
      copyPatch(store, 'synth/user/cluster.aif', 'synth/user'),
    ]);
    expect(new Set(temps).size).toBe(2);
    for (const temp of temps) expect(temp).toMatch(/^synth\/user\/\.co-op-tmp-/);
    expect(new Set(copies).size).toBe(2);
    for (const path of copies) expectSameBytes(store.snapshot()[path], cluster);
    expect(Object.keys(store.snapshot()).filter((p) => p.includes('.co-op-'))).toEqual([]);
  });

  it('leaves a temp that recoverStranded cleans up', async () => {
    const store = drive();
    store.move = async () => {
      throw new StoreError('disk full');
    };
    store.remove = async () => {
      throw new StoreError('disk full');
    };
    await expect(copyPatch(store, 'synth/user/cluster.aif', 'synth/user')).rejects.toThrow('disk full');
    const left = Object.keys(store.snapshot()).filter((p) => p.includes('.co-op-tmp-'));
    expect(left).toHaveLength(1);
    const clean = new MemoryStore(store.snapshot());
    await recoverStranded(clean, 'synth/user');
    expect(Object.keys(clean.snapshot()).filter((p) => p.includes('.co-op-'))).toEqual([]);
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

  it.each(['', 'a/b', '.hidden', 'con', 'LPT1', 'aux.old'])('rejects the folder name %j', async (name) => {
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

/** Records every move; `failMoves` lists 1-based move calls that throw instead. */
class ScriptedMoveStore extends MemoryStore {
  moves: Array<[string, string]> = [];
  failMoves = new Set<number>();
  override async move(from: string, to: string): Promise<void> {
    this.moves.push([from, to]);
    if (this.failMoves.has(this.moves.length)) throw new StoreError(`move ${this.moves.length} failed`);
    return super.move(from, to);
  }
}

describe('case-only rename recovery', () => {
  it('parks the file at .co-op-old-<original name>, where recoverStranded finds it', async () => {
    const store = new ScriptedMoveStore({ 'drum/user/kick.aif': Uint8Array.from([1]) });
    await renameFile(store, 'drum/user/kick.aif', 'Kick.aif');
    expect(store.moves).toEqual([
      ['drum/user/kick.aif', 'drum/user/.co-op-old-kick.aif'],
      ['drum/user/.co-op-old-kick.aif', 'drum/user/Kick.aif'],
    ]);
  });

  it('moves the file back and rethrows when the second move fails', async () => {
    const store = new ScriptedMoveStore({ 'drum/user/kick.aif': Uint8Array.from([1]) });
    store.failMoves.add(2);
    await expect(renameFile(store, 'drum/user/kick.aif', 'Kick.aif')).rejects.toThrow('move 2 failed');
    expect(Object.keys(store.snapshot())).toEqual(['drum/user/kick.aif']);
  });

  it('names where the file was kept when it cannot be moved back, and recoverStranded restores it', async () => {
    const store = new ScriptedMoveStore({ 'drum/user/kick.aif': Uint8Array.from([1]) });
    store.failMoves.add(2);
    store.failMoves.add(3);
    const error = await renameFile(store, 'drum/user/kick.aif', 'Kick.aif').catch((e: unknown) => e);
    expect(error).toBeInstanceOf(StoreError);
    expect((error as Error).message).toContain('drum/user/.co-op-old-kick.aif');
    expect((error as Error).cause).toBeInstanceOf(StoreError);
    expect(Object.keys(store.snapshot())).toEqual(['drum/user/.co-op-old-kick.aif']);
    expect(await recoverStranded(store, 'drum/user')).toEqual(['drum/user/kick.aif']);
    expect(store.snapshot()['drum/user/kick.aif']).toEqual(Uint8Array.from([1]));
  });

  it('clears a stale .co-op-old- leftover before parking the file', async () => {
    const store = new MemoryStore({
      'drum/user/kick.aif': Uint8Array.from([1]),
      'drum/user/.co-op-old-kick.aif': Uint8Array.from([0]),
    });
    expect(await renameFile(store, 'drum/user/kick.aif', 'KICK.aif')).toBe('drum/user/KICK.aif');
    expect(store.snapshot()).toEqual({ 'drum/user/KICK.aif': Uint8Array.from([1]) });
  });
});

describe('FAT-safe names', () => {
  it.each([
    'kick', 'kick.wav', 'kick.aif.txt', 'a:b.aif', 'a*b.aif', 'a?b.aif', 'a"b.aif', 'a<b.aif', 'a>b.aif',
    'a|b.aif', 'a\u0001b.aif', 'a\u007fb.aif', 'kick.aif ', 'kick.aif.',
  ])('renameFile rejects %j', async (name) => {
    const store = drive();
    await expect(renameFile(store, 'drum/user/kit.aif', name)).rejects.toThrow(OperationError);
    expect(Object.keys(store.snapshot())).toEqual(Object.keys(drive().snapshot()));
  });

  it.each(['Kick.AIF', 'kick.aiff', 'Kick Drum 2.AiFf'])('renameFile accepts %j', async (name) => {
    expect(await renameFile(drive(), 'drum/user/kit.aif', name)).toBe(`drum/user/${name}`);
  });

  it.each(['a:b', 'a*b', 'a?b', 'a"b', 'a<b', 'a>b', 'a|b', 'a\u0001b', 'pads ', 'pads.', 'a\\b'])(
    'ensureFolder rejects %j',
    async (name) => {
      await expect(ensureFolder(drive(), 'synth', name)).rejects.toThrow(OperationError);
    },
  );

  it('ensureFolder accepts names without an extension', async () => {
    expect(await ensureFolder(drive(), 'synth', 'soft pads')).toBe('synth/soft pads');
  });

  it('ensureFolder refuses a name used by a file in any case', async () => {
    const store = new MemoryStore({ 'synth/pads.aif': Uint8Array.from([1]) });
    await expect(ensureFolder(store, 'synth', 'PADS.aif')).rejects.toThrow(OperationError);
  });
});

describe('folder resolution and races', () => {
  it('movePatch treats a target folder differing only in case as the same folder', async () => {
    const store = drive();
    expect(await movePatch(store, 'drum/user/kit.aif', 'DRUM/User')).toBe('drum/user/kit.aif');
    expect(Object.keys(store.snapshot())).toEqual(Object.keys(drive().snapshot()));
  });

  it('movePatch and copyPatch resolve the target folder case-insensitively', async () => {
    const store = drive();
    expect(await movePatch(store, 'drum/user/kit.aif', 'SYNTH/User')).toBe('synth/user/kit.aif');
    expect(await copyPatch(store, 'synth/user/kit.aif', 'Drum/USER/New')).toBe('drum/user/New/kit.aif');
  });

  it('copyPatch takes the next free name when the target appears while copying', async () => {
    class RacingStore extends MemoryStore {
      raced = false;
      override async move(from: string, to: string): Promise<void> {
        if (!this.raced && to === 'drum/other/kit.aif') {
          this.raced = true;
          await this.write(to, Uint8Array.from([7]));
        }
        return super.move(from, to);
      }
    }
    const store = new RacingStore({ 'drum/user/kit.aif': Uint8Array.from([9]), 'drum/other/x.aif': Uint8Array.from([1]) });
    expect(await copyPatch(store, 'drum/user/kit.aif', 'drum/other')).toBe('drum/other/kit-2.aif');
    expect(store.snapshot()).toEqual({
      'drum/other/kit-2.aif': Uint8Array.from([9]),
      'drum/other/kit.aif': Uint8Array.from([7]),
      'drum/other/x.aif': Uint8Array.from([1]),
      'drum/user/kit.aif': Uint8Array.from([9]),
    });
  });

  it('copyPatch removes its temporary file when the move fails for another reason', async () => {
    const store = new ScriptedMoveStore({ 'drum/user/kit.aif': Uint8Array.from([9]) });
    store.failMoves.add(1);
    await expect(copyPatch(store, 'drum/user/kit.aif', 'drum/other')).rejects.toThrow('move 1 failed');
    expect(Object.keys(store.snapshot())).toEqual(['drum/user/kit.aif']);
  });
});

describe('setPatchName trimming', () => {
  it('stores the trimmed name, and measures length after trimming', async () => {
    const store = drive();
    await setPatchName(store, 'synth/user/cluster.aif', `  ${'a'.repeat(24)}  `);
    expect(readPatch(store.snapshot()['synth/user/cluster.aif'] as Uint8Array).name).toBe('a'.repeat(24));
  });
});

describe('renamePatch', () => {
  const cluster = fixtureBytes('synth-cluster.aif');

  it('changes the file name and the name inside the patch, keeping the extension', async () => {
    const store = new MemoryStore({ 'synth/user/old.AIFF': cluster });
    expect(await renamePatch(store, 'synth/user/old.AIFF', ' new pad ')).toBe('synth/user/new pad.AIFF');
    expect(await store.exists('synth/user/old.AIFF')).toBe(false);
    expect(readPatch(await store.read('synth/user/new pad.AIFF')).name).toBe('new pad');
  });

  it('changes the name inside when the file name stays the same', async () => {
    const store = new MemoryStore({ 'synth/user/same.aif': cluster });
    expect(await renamePatch(store, 'synth/user/same.aif', 'same')).toBe('synth/user/same.aif');
    expect(readPatch(await store.read('synth/user/same.aif')).name).toBe('same');
  });

  it('only renames the file of a patch with no metadata', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': fixtureBytes('no-metadata.aif') });
    expect(await renamePatch(store, 'synth/user/a.aif', 'a name longer than the patch name limit')).toBe('synth/user/a name longer than the patch name limit.aif');
  });

  it('changes nothing when the name is taken or cannot be a file name', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': cluster, 'synth/user/b.aif': cluster });
    await expect(renamePatch(store, 'synth/user/a.aif', 'b')).rejects.toThrow(/already exists/);
    await expect(renamePatch(store, 'synth/user/a.aif', 'bad/name')).rejects.toThrow(/cannot hold/);
    expect(await store.read('synth/user/a.aif')).toEqual(cluster);
  });

  it('refuses a name too long for the OP-1 before changing anything', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': cluster });
    await expect(renamePatch(store, 'synth/user/a.aif', 'x'.repeat(25))).rejects.toThrow(/longer/);
    expect(await store.exists('synth/user/a.aif')).toBe(true);
  });
});
