import { beforeEach, describe, expect, it } from 'vitest';
import { STORAGE_KEY, clearCredentials, loadCredentials, saveCredentials, type KeyValueStore } from './credentials';

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

const creds = { email: 'me@example.com', token: 'secret-token', username: 'alice' };

describe('op1.fun credentials', () => {
  let storage: FakeStorage;
  beforeEach(() => {
    storage = new FakeStorage();
    clearCredentials(storage);
  });

  it('keeps credentials in memory only, by default', () => {
    expect(saveCredentials(creds, false, storage)).toBe(true);
    expect(storage.items.size).toBe(0);
    expect(loadCredentials(storage)).toEqual({ ...creds, remember: false });
    expect(loadCredentials(null)).toEqual({ ...creds, remember: false });
  });

  it('stores them in localStorage only when remembered', () => {
    saveCredentials(creds, true, storage);
    expect(JSON.parse(storage.items.get(STORAGE_KEY) ?? '')).toEqual({
      email: 'me@example.com',
      token: 'secret-token',
      username: 'alice',
    });
    // A new page (empty memory) finds the remembered copy.
    clearCredentials(new FakeStorage());
    expect(loadCredentials(storage)).toEqual({ ...creds, remember: true });
  });

  it('removes the remembered copy when saved again without Remember', () => {
    saveCredentials(creds, true, storage);
    saveCredentials(creds, false, storage);
    expect(storage.items.has(STORAGE_KEY)).toBe(false);
    expect(loadCredentials(storage)?.remember).toBe(false);
  });

  it('forgets both copies', () => {
    saveCredentials(creds, true, storage);
    clearCredentials(storage);
    expect(storage.items.size).toBe(0);
    expect(loadCredentials(storage)).toBeNull();
  });

  it('ignores malformed stored values', () => {
    for (const raw of ['not json', 'null', '[]', '{"email":"a"}', '{"email":"","token":"t"}', '{"email":1,"token":"t"}']) {
      clearCredentials(storage);
      storage.items.set(STORAGE_KEY, raw);
      expect(loadCredentials(storage), raw).toBeNull();
    }
    storage.items.set(STORAGE_KEY, '{"email":"a@b.c","token":"t"}');
    expect(loadCredentials(storage)).toEqual({ email: 'a@b.c', token: 't', username: '', remember: true });
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
    expect(loadCredentials(broken)).toBeNull();
    expect(saveCredentials(creds, true, broken)).toBe(false);
    // Still usable for this page, but not marked as remembered.
    expect(loadCredentials(broken)).toEqual({ ...creds, remember: false });
    expect(saveCredentials(creds, false, broken)).toBe(true);
    expect(() => clearCredentials(broken)).not.toThrow();
    expect(loadCredentials(broken)).toBeNull();
  });
});
