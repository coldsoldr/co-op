import { describe, it, expect } from 'vitest';
import { compatView, dateStamp, displayName, formatSize, groupByFolder, kindMismatchText, rootOf } from './format';
import type { LibraryEntry } from '../core/library/library';
import type { Patch } from '../core/codec/patch';

const entry = (path: string, extra: Partial<LibraryEntry> = {}): LibraryEntry => {
  const parts = path.split('/');
  return {
    path,
    folder: parts[1] as string,
    fileName: parts[2] as string,
    kind: parts[0] === 'drum' ? 'drum' : 'synth',
    sizeBytes: 10,
    patch: null,
    error: null,
    compat: null,
    kindMismatch: false,
    ...extra,
  };
};

const patch = (name: string, kind: Patch['kind']): Patch => ({ name, kind }) as Patch;

describe('format helpers', () => {
  it('stamps the local date', () => {
    expect(dateStamp(new Date(2026, 0, 5, 23, 59))).toBe('2026-01-05');
  });

  it('formats sizes', () => {
    expect(formatSize(512)).toBe('512 B');
    expect(formatSize(2048)).toBe('2.0 KB');
    expect(formatSize(3 * 1024 * 1024)).toBe('3.0 MB');
  });

  it('finds the patch root and groups by folder in first-seen order', () => {
    expect(rootOf(entry('synth/user/a.aif'))).toBe('synth');
    const groups = groupByFolder([entry('synth/user/a.aif'), entry('drum/user/b.aif'), entry('synth/user/c.aif')]);
    expect(groups.map((g) => [g.folder, g.entries.map((e) => e.fileName)])).toEqual([
      ['synth/user', ['a.aif', 'c.aif']],
      ['drum/user', ['b.aif']],
    ]);
  });

  it('names an entry by its metadata name, else its file name', () => {
    expect(displayName(entry('synth/user/a.aif'))).toBe('a.aif');
    expect(displayName(entry('synth/user/a.aif', { patch: patch('pad', 'synth') }))).toBe('pad');
    expect(displayName(entry('synth/user/a.aif', { patch: patch('', 'synth') }))).toBe('a.aif');
  });

  it('describes compatibility, unreadable and still-loading entries', () => {
    expect(compatView(entry('synth/user/a.aif'))).toEqual({ level: 'loading', label: 'reading', detail: null });
    expect(compatView(entry('synth/user/a.aif', { compat: { level: 'ok' } }))).toEqual({
      level: 'ok',
      label: 'ok',
      detail: null,
    });
    expect(compatView(entry('synth/user/a.aif', { compat: { level: 'warn', reason: 'engine x' } })).detail).toBe('engine x');
    expect(compatView(entry('synth/user/a.aif', { error: 'bad FORM' }))).toEqual({
      level: 'unreadable',
      label: 'unreadable',
      detail: 'bad FORM',
    });
  });

  it('words a kind mismatch from the folder and metadata kinds', () => {
    expect(kindMismatchText(entry('synth/user/a.aif'))).toBeNull();
    expect(kindMismatchText(entry('synth/user/a.aif', { kindMismatch: true, patch: patch('k', 'drum') }))).toBe(
      'in synth folder, but metadata says drum',
    );
  });
});
