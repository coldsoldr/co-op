import { describe, it, expect } from 'vitest';
import { loadLibrary, loadEntryPatch, loadAllPatches, type LibraryEntry } from './library';
import { MemoryStore } from '../store/memory-store';
import { op1Stock } from '../profiles/op1-stock';
import { fixtureBytes } from '../../../test/fixture-bytes';

const cluster = fixtureBytes('synth-cluster.aif');
const boombap = fixtureBytes('drum-boombap.aif');
const noMeta = fixtureBytes('no-metadata.aif');
const garbage = Uint8Array.from([1, 2, 3, 4, 5, 6, 7]);

const drive = (): MemoryStore =>
  new MemoryStore({
    'synth/user/cluster.aif': cluster,
    'synth/user/kit-in-synth.aif': boombap,
    'synth/user/bad.aif': garbage,
    'synth/user/plain.aif': noMeta,
    'synth/user/.co-op-tmp-x.aif': cluster,
    'drum/user/boombap.aif': boombap,
    'tape/track1.aif': garbage,
  });

const byPath = (entries: LibraryEntry[], path: string): LibraryEntry => {
  const entry = entries.find((e) => e.path === path);
  if (!entry) throw new Error(`no entry ${path}`);
  return entry;
};

describe('loadLibrary', () => {
  it('scans the drive and fills sizes, leaving patches unloaded', async () => {
    const entries = await loadLibrary(drive(), op1Stock);
    expect(entries.map((e) => e.path)).toEqual([
      'synth/user/bad.aif',
      'synth/user/cluster.aif',
      'synth/user/kit-in-synth.aif',
      'synth/user/plain.aif',
      'drum/user/boombap.aif',
    ]);
    expect(byPath(entries, 'synth/user/cluster.aif')).toEqual({
      path: 'synth/user/cluster.aif',
      folder: 'user',
      fileName: 'cluster.aif',
      kind: 'synth',
      sizeBytes: cluster.length,
      patch: null,
      error: null,
      compat: null,
      kindMismatch: false,
    });
    expect(byPath(entries, 'synth/user/bad.aif').sizeBytes).toBe(garbage.length);
  });

  it('keeps an entry whose size cannot be read, with the error', async () => {
    class NoSize extends MemoryStore {
      override async size(): Promise<number> {
        throw new Error('size failed');
      }
    }
    const entries = await loadLibrary(new NoSize({ 'synth/user/cluster.aif': cluster }), op1Stock);
    expect(entries).toHaveLength(1);
    expect(entries[0]).toMatchObject({ sizeBytes: 0, error: 'size failed', patch: null });
  });
});

describe('loadEntryPatch', () => {
  it('parses a synth patch and fills compat', async () => {
    const store = drive();
    const [entry] = (await loadLibrary(store, op1Stock)).filter((e) => e.fileName === 'cluster.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry as LibraryEntry);
    expect(loaded.patch?.kind).toBe('synth');
    expect(loaded.patch?.name).toBe('default');
    expect(loaded.patch?.raw).toEqual(cluster);
    expect(loaded.compat).toEqual({ level: 'ok' });
    expect(loaded.kindMismatch).toBe(false);
    expect(loaded.error).toBeNull();
  });

  it('parses a drum patch in a drum folder without a mismatch', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'drum/user/boombap.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry);
    expect(loaded.kind).toBe('drum');
    expect(loaded.patch?.kind).toBe('drum');
    expect(loaded.patch?.name).toBe('boombap1');
    expect(loaded.compat).toEqual({ level: 'ok' });
    expect(loaded.kindMismatch).toBe(false);
  });

  it('flags a drum patch living in a synth folder, keeping the folder kind', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'synth/user/kit-in-synth.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry);
    expect(loaded.kind).toBe('synth');
    expect(loaded.patch?.kind).toBe('drum');
    expect(loaded.kindMismatch).toBe(true);
  });

  it('does not flag a mismatch when the metadata kind is unknown', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'synth/user/plain.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry);
    expect(loaded.patch?.kind).toBe('unknown');
    expect(loaded.kindMismatch).toBe(false);
    expect(loaded.compat?.level).toBe('unsupported');
    expect(loaded.error).toBeNull();
  });

  it('keeps an unparseable file as an entry with an error', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'synth/user/bad.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry);
    expect(loaded.path).toBe('synth/user/bad.aif');
    expect(loaded.patch).toBeNull();
    expect(loaded.compat).toBeNull();
    expect(loaded.kindMismatch).toBe(false);
    expect(loaded.error).toBe('not an AIFF file');
  });

  it('keeps an entry whose file can no longer be read, with the store error', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'synth/user/cluster.aif');
    await store.remove('synth/user/cluster.aif');
    const loaded = await loadEntryPatch(store, op1Stock, entry);
    expect(loaded.patch).toBeNull();
    expect(loaded.error).toMatch(/synth\/user\/cluster\.aif/);
  });

  it('does not mutate the entry it was given', async () => {
    const store = drive();
    const entry = byPath(await loadLibrary(store, op1Stock), 'synth/user/cluster.aif');
    await loadEntryPatch(store, op1Stock, entry);
    expect(entry.patch).toBeNull();
  });
});

/** A store whose reads finish in reverse path order and that records how many run at once. */
class SlowStore extends MemoryStore {
  inFlight = 0;
  maxInFlight = 0;
  constructor(files: Record<string, Uint8Array>, private readonly delays: Record<string, number>) {
    super(files);
  }
  override async read(path: string): Promise<Uint8Array> {
    this.inFlight++;
    this.maxInFlight = Math.max(this.maxInFlight, this.inFlight);
    await new Promise((resolve) => setTimeout(resolve, this.delays[path] ?? 0));
    this.inFlight--;
    return super.read(path);
  }
}

describe('loadAllPatches', () => {
  it('loads every entry with bounded concurrency, preserving order and reporting progress', async () => {
    const files: Record<string, Uint8Array> = {};
    const delays: Record<string, number> = {};
    for (let i = 0; i < 6; i++) {
      const path = `synth/user/p${i}.aif`;
      files[path] = i === 3 ? garbage : cluster;
      delays[path] = (6 - i) * 5;
    }
    const store = new SlowStore(files, delays);
    const entries = await loadLibrary(store, op1Stock);
    const progress: Array<[number, number]> = [];
    const loaded = await loadAllPatches(store, op1Stock, entries, (done, total) => progress.push([done, total]), 2);
    expect(loaded.map((e) => e.path)).toEqual(entries.map((e) => e.path));
    expect(loaded.map((e) => (e.error ? 'error' : e.patch?.name))).toEqual([
      'default', 'default', 'default', 'error', 'default', 'default',
    ]);
    expect(store.maxInFlight).toBe(2);
    expect(progress).toEqual([[1, 6], [2, 6], [3, 6], [4, 6], [5, 6], [6, 6]]);
  });

  it('returns [] for no entries', async () => {
    expect(await loadAllPatches(drive(), op1Stock, [])).toEqual([]);
  });
});

describe('bounded pools', () => {
  it.each([0, -1, Number.NaN, 2.5, Number.POSITIVE_INFINITY])(
    'loadAllPatches with concurrency %s still loads every entry in order',
    async (concurrency) => {
      const store = drive();
      const entries = await loadLibrary(store, op1Stock);
      const loaded = await loadAllPatches(store, op1Stock, entries, undefined, concurrency);
      expect(loaded.map((e) => e.path)).toEqual(entries.map((e) => e.path));
      expect(loaded.every((e) => e.patch !== null || e.error !== null)).toBe(true);
    },
  );

  it('loadLibrary sizes files with at most `concurrency` calls in flight', async () => {
    class SlowSize extends MemoryStore {
      inFlight = 0;
      maxInFlight = 0;
      override async size(path: string): Promise<number> {
        this.inFlight++;
        this.maxInFlight = Math.max(this.maxInFlight, this.inFlight);
        await new Promise((resolve) => setTimeout(resolve, 2));
        this.inFlight--;
        return super.size(path);
      }
    }
    const files: Record<string, Uint8Array> = {};
    for (let i = 0; i < 10; i++) files[`synth/user/p${i}.aif`] = cluster;
    const store = new SlowSize(files);
    const entries = await loadLibrary(store, op1Stock, 3);
    expect(entries.map((e) => e.fileName)).toEqual(Object.keys(files).map((p) => p.split('/')[2]));
    expect(store.maxInFlight).toBe(3);
    await loadLibrary(store, op1Stock);
    expect(store.maxInFlight).toBe(4);
  });
});
