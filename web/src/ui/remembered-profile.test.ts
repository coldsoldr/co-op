import { beforeEach, describe, expect, it } from 'vitest';
import { STORAGE_KEY, loadRememberedProfile, rememberProfile, type KeyValueStore } from './remembered-profile';

class FakeStorage implements KeyValueStore {
  readonly items = new Map<string, string>();
  getItem(key: string): string | null {
    return this.items.get(key) ?? null;
  }
  setItem(key: string, value: string): void {
    this.items.set(key, value);
  }
  removeItem(key: string): void {
    this.items.delete(key);
  }
}

describe('remembered profile', () => {
  let storage: FakeStorage;
  beforeEach(() => {
    storage = new FakeStorage();
  });

  it('reads null when nothing is stored', () => {
    expect(loadRememberedProfile(storage)).toBeNull();
    expect(loadRememberedProfile(null)).toBeNull();
  });

  it('round-trips a chosen profile with the time it was chosen', () => {
    rememberProfile('op1/hundekraft', storage);
    const stored = JSON.parse(storage.items.get(STORAGE_KEY) ?? '') as { id: string; at: string };
    expect(stored.id).toBe('op1/hundekraft');
    expect(new Date(stored.at).toISOString()).toBe(stored.at);
    expect(loadRememberedProfile(storage)).toBe('op1/hundekraft');
  });

  it('forgets the choice with null', () => {
    rememberProfile('op1/opus', storage);
    rememberProfile(null, storage);
    expect(storage.items.has(STORAGE_KEY)).toBe(false);
    expect(loadRememberedProfile(storage)).toBeNull();
  });

  it('reads an id that is not in the registry as null', () => {
    rememberProfile('op1/gone', storage);
    expect(loadRememberedProfile(storage)).toBeNull();
  });

  it('ignores malformed stored values', () => {
    for (const raw of ['not json', 'null', '[]', '"op1/stock"', '{}', '{"id":1}', '{"at":"2026-01-01T00:00:00.000Z"}']) {
      storage.items.set(STORAGE_KEY, raw);
      expect(loadRememberedProfile(storage), raw).toBeNull();
    }
    storage.items.set(STORAGE_KEY, '{"id":"op1/stock"}');
    expect(loadRememberedProfile(storage)).toBe('op1/stock');
  });

  it('copes with a browser that refuses storage', () => {
    const broken: KeyValueStore = {
      getItem: () => {
        throw new Error('blocked');
      },
      setItem: () => {
        throw new Error('blocked');
      },
      removeItem: () => {
        throw new Error('blocked');
      },
    };
    expect(loadRememberedProfile(broken)).toBeNull();
    expect(() => rememberProfile('op1/stock', broken)).not.toThrow();
    expect(() => rememberProfile(null, broken)).not.toThrow();
  });

  it('uses localStorage by default and works where there is none', () => {
    // vitest runs in node, where localStorage may be missing or unusable.
    expect(() => rememberProfile('op1/stock')).not.toThrow();
    expect(() => loadRememberedProfile()).not.toThrow();
  });
});
