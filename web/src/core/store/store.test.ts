import { describe, it, expect } from 'vitest';
import { joinPath, dirname, basename, StoreError } from './store';

describe('joinPath', () => {
  it('joins segments with /', () => {
    expect(joinPath('synth', 'user', 'a.aif')).toBe('synth/user/a.aif');
  });

  it('drops empty segments', () => {
    expect(joinPath('', 'a')).toBe('a');
    expect(joinPath('a', '', 'b')).toBe('a/b');
    expect(joinPath()).toBe('');
  });

  it('collapses duplicate separators and strips leading and trailing ones', () => {
    expect(joinPath('a/', '/b')).toBe('a/b');
    expect(joinPath('/a//b/', 'c/')).toBe('a/b/c');
  });
});

describe('dirname', () => {
  it('is empty for a top-level name', () => {
    expect(dirname('a.aif')).toBe('');
  });

  it('returns the parent folder path', () => {
    expect(dirname('synth/user/a.aif')).toBe('synth/user');
  });
});

describe('basename', () => {
  it('returns the last segment', () => {
    expect(basename('synth/user/a.aif')).toBe('a.aif');
    expect(basename('a.aif')).toBe('a.aif');
  });
});

describe('StoreError', () => {
  it('is an Error named StoreError', () => {
    const error = new StoreError('boom');
    expect(error).toBeInstanceOf(Error);
    expect(error.name).toBe('StoreError');
  });
});
