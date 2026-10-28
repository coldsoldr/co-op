import { describe, it, expect } from 'vitest';
import { MemoryStore } from './memory-store';
import { StoreError } from './store';
import { describeWritableStore } from '../../../test/store-contract';

describeWritableStore('MemoryStore', (files) => new MemoryStore(files));

describe('MemoryStore', () => {
  const files = (): Record<string, Uint8Array> => ({ 'synth/user/a.aif': Uint8Array.of(1, 2) });

  it('defaults to writable with a generic label', () => {
    const store = new MemoryStore({});
    expect(store.capabilities.write).toBe(true);
    expect(store.label).toBe('memory');
  });

  it('takes a label and a read-only flag', () => {
    const store = new MemoryStore({}, { write: false, label: 'OP-1' });
    expect(store.capabilities.write).toBe(false);
    expect(store.label).toBe('OP-1');
  });

  it('lists an empty root', async () => {
    expect(await new MemoryStore({}).list('')).toEqual([]);
  });

  it('rejects every change when read-only, naming the path', async () => {
    const store = new MemoryStore(files(), { write: false });
    await expect(store.write('x.aif', Uint8Array.of(1))).rejects.toThrow(StoreError);
    await expect(store.write('x.aif', Uint8Array.of(1))).rejects.toThrow('x.aif');
    await expect(store.move('synth/user/a.aif', 'b.aif')).rejects.toThrow(StoreError);
    await expect(store.remove('synth/user/a.aif')).rejects.toThrow(StoreError);
    await expect(store.mkdir('synth/new')).rejects.toThrow(StoreError);
    expect(Object.keys(store.snapshot())).toEqual(['synth/user/a.aif']);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1, 2));
    expect(await store.exists('synth/new')).toBe(false);
  });

  it('snapshots files by path, sorted, as copies', async () => {
    const store = new MemoryStore(files());
    await store.write('drum/user/k.aif', Uint8Array.of(3));
    const snap = store.snapshot();
    expect(Object.keys(snap)).toEqual(['drum/user/k.aif', 'synth/user/a.aif']);
    expect(snap).toEqual({ 'drum/user/k.aif': Uint8Array.of(3), 'synth/user/a.aif': Uint8Array.of(1, 2) });
    snap['drum/user/k.aif']?.fill(42);
    expect(await store.read('drum/user/k.aif')).toEqual(Uint8Array.of(3));
  });

  it('does not alias the constructor input', async () => {
    const input = files();
    const store = new MemoryStore(input);
    input['synth/user/a.aif']?.fill(42);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1, 2));
  });

  it('rejects a file path that is also a folder, in either input order', () => {
    const file = Uint8Array.of(1);
    expect(() => new MemoryStore({ 'a/b': file, 'a/b/c.aif': file })).toThrow(StoreError);
    expect(() => new MemoryStore({ 'a/b/c.aif': file, 'a/b': file })).toThrow(StoreError);
  });

  it('returns copies from read', async () => {
    const store = new MemoryStore(files());
    (await store.read('synth/user/a.aif')).fill(42);
    expect(await store.read('synth/user/a.aif')).toEqual(Uint8Array.of(1, 2));
  });
});
