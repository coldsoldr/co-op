import { describe, it, expect } from 'vitest';
import { writeAtomic, recoverStranded } from './atomic';
import { MemoryStore } from './memory-store';
import { StoreError } from './store';

/** A MemoryStore whose move throws once, for the first move whose arguments match. */
class FlakyMoveStore extends MemoryStore {
  readonly failure = new Error('disk unplugged');
  private armed = true;

  constructor(
    files: Record<string, Uint8Array>,
    private readonly shouldFail: (from: string, to: string) => boolean,
  ) {
    super(files);
  }

  override async move(from: string, to: string): Promise<void> {
    if (this.armed && this.shouldFail(from, to)) {
      this.armed = false;
      throw this.failure;
    }
    return super.move(from, to);
  }
}

const original = Uint8Array.of(1, 1, 1);
const replacement = Uint8Array.of(2, 2);

describe('writeAtomic', () => {
  it('writes a new file and leaves no temp behind', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': original });
    await writeAtomic(store, 'synth/user/b.aif', replacement);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original, 'synth/user/b.aif': replacement });
  });

  it('replaces an existing file and leaves no temp behind', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': original });
    await writeAtomic(store, 'synth/user/a.aif', replacement);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': replacement });
  });

  it('works for a top-level path', async () => {
    const store = new MemoryStore({});
    await writeAtomic(store, 'a.aif', replacement);
    expect(store.snapshot()).toEqual({ 'a.aif': replacement });
  });

  it('writes through a dot-named temp file in the same folder', async () => {
    const moves: Array<[string, string]> = [];
    class Spy extends MemoryStore {
      override async move(from: string, to: string): Promise<void> {
        moves.push([from, to]);
        return super.move(from, to);
      }
    }
    await writeAtomic(new Spy({}), 'synth/user/a.aif', replacement);
    expect(moves).toEqual([['synth/user/.co-op-tmp-a.aif', 'synth/user/a.aif']]);
  });

  it('leaves the target untouched and removes the temp when moving into place fails', async () => {
    const store = new FlakyMoveStore({ 'synth/user/a.aif': original }, (_from, to) => to === 'synth/user/a.aif');
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toBe(store.failure);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original });
  });

  it('leaves the target untouched when setting the old file aside fails', async () => {
    const store = new FlakyMoveStore({ 'synth/user/a.aif': original }, (from) => from === 'synth/user/a.aif');
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toBe(store.failure);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original });
  });

  it('creates nothing when moving a new file into place fails', async () => {
    const store = new FlakyMoveStore({}, () => true);
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toBe(store.failure);
    expect(store.snapshot()).toEqual({});
  });

  it('overwrites leftovers from an interrupted earlier write', async () => {
    const store = new MemoryStore({
      'synth/user/a.aif': original,
      'synth/user/.co-op-tmp-a.aif': Uint8Array.of(9),
      'synth/user/.co-op-old-a.aif': Uint8Array.of(8),
    });
    await writeAtomic(store, 'synth/user/a.aif', replacement);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': replacement });
  });

  it('rejects on a read-only store without changing it', async () => {
    const store = new MemoryStore({ 'synth/user/a.aif': original }, { write: false });
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toThrow(StoreError);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original });
  });

  it('removes a half-written temp when writing it fails', async () => {
    const failure = new Error('disk full');
    class HalfWrite extends MemoryStore {
      override async write(path: string, bytes: Uint8Array): Promise<void> {
        await super.write(path, bytes.slice(0, 1));
        throw failure;
      }
    }
    const store = new HalfWrite({ 'synth/user/a.aif': original });
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toBe(failure);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original });
  });

  it('first restores an old file stranded by an earlier failed write', async () => {
    const store = new MemoryStore({ 'synth/user/.co-op-old-a.aif': original });
    await writeAtomic(store, 'synth/user/a.aif', replacement);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': replacement });
  });

  it('keeps a restored stranded file when the new write then fails', async () => {
    const store = new FlakyMoveStore(
      { 'synth/user/.co-op-old-a.aif': original },
      (from, to) => from === 'synth/user/.co-op-tmp-a.aif' && to === 'synth/user/a.aif',
    );
    await expect(writeAtomic(store, 'synth/user/a.aif', replacement)).rejects.toBe(store.failure);
    expect(store.snapshot()).toEqual({ 'synth/user/a.aif': original });
  });

  it('names the kept old file when putting it back also fails', async () => {
    class StuckStore extends MemoryStore {
      readonly failure = new Error('disk unplugged');
      override async move(from: string, to: string): Promise<void> {
        if (to === 'synth/user/a.aif') throw this.failure;
        return super.move(from, to);
      }
    }
    const store = new StuckStore({ 'synth/user/a.aif': original });
    const error = await writeAtomic(store, 'synth/user/a.aif', replacement).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(StoreError);
    expect((error as Error).message).toContain('synth/user/.co-op-old-a.aif');
    expect((error as Error).cause).toBe(store.failure);
    expect(store.snapshot()).toEqual({ 'synth/user/.co-op-old-a.aif': original });
  });
});

describe('recoverStranded', () => {
  const bytes = (value: number): Uint8Array => Uint8Array.of(value);

  it('restores old files whose target is missing and returns their paths', async () => {
    const store = new MemoryStore({
      'synth/user/.co-op-old-a.aif': bytes(1),
      'synth/user/.co-op-old-b.aif': bytes(2),
      'synth/user/c.aif': bytes(3),
    });
    expect(await recoverStranded(store, 'synth/user')).toEqual(['synth/user/a.aif', 'synth/user/b.aif']);
    expect(store.snapshot()).toEqual({
      'synth/user/a.aif': bytes(1),
      'synth/user/b.aif': bytes(2),
      'synth/user/c.aif': bytes(3),
    });
  });

  it('removes temp leftovers and old files already superseded by their target', async () => {
    const store = new MemoryStore({
      'synth/user/.co-op-tmp-a.aif': bytes(9),
      'synth/user/.co-op-old-a.aif': bytes(8),
      'synth/user/a.aif': bytes(1),
      'synth/user/.DS_Store': bytes(7),
    });
    expect(await recoverStranded(store, 'synth/user')).toEqual([]);
    expect(store.snapshot()).toEqual({ 'synth/user/.DS_Store': bytes(7), 'synth/user/a.aif': bytes(1) });
  });

  it('works at the drive root and for a missing folder', async () => {
    const store = new MemoryStore({ '.co-op-old-a.aif': bytes(1) });
    expect(await recoverStranded(store, '')).toEqual(['a.aif']);
    expect(await recoverStranded(store, 'nope')).toEqual([]);
  });

  it('does nothing on a read-only store', async () => {
    const store = new MemoryStore({ 'synth/user/.co-op-old-a.aif': bytes(1) }, { write: false });
    expect(await recoverStranded(store, 'synth/user')).toEqual([]);
    expect(store.snapshot()).toEqual({ 'synth/user/.co-op-old-a.aif': bytes(1) });
  });
});
