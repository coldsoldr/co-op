import { describe, it, expect } from 'vitest';
import { compatView, dateStamp, formatSize, groupByFolder, kindMismatchText, rootOf, traitDetails } from './format';
import { soundTraits } from '../core/codec/sound-traits';
import { SYNTH_META } from '../../test/fixtures/meta';
import {
  displayName,
  filterRows,
  mergeRows,
  sortRows,
  toRow,
  type LibraryRow,
  type PatchInfo,
} from './rows';
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

const info = (name: string, kind: PatchInfo['kind'] = 'synth'): PatchInfo => ({
  name,
  kind,
  hasMeta: true,
  canonical: true,
  traits: null,
});

const row = (path: string, extra: Partial<LibraryRow> = {}): LibraryRow => ({ ...toRow(entry(path)), ...extra });

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
    expect(rootOf(row('synth/user/a.aif'))).toBe('synth');
    const groups = groupByFolder([row('synth/user/a.aif'), row('drum/user/b.aif'), row('synth/user/c.aif')]);
    expect(groups.map((g) => [g.folder, g.rows.map((e) => e.fileName)])).toEqual([
      ['synth/user', ['a.aif', 'c.aif']],
      ['drum/user', ['b.aif']],
    ]);
  });

  it('names a row by its metadata name, else its file name', () => {
    expect(displayName(row('synth/user/a.aif'))).toBe('a.aif');
    expect(displayName(row('synth/user/a.aif', { info: info('pad') }))).toBe('pad');
    expect(displayName(row('synth/user/a.aif', { info: info('') }))).toBe('a.aif');
  });

  it('describes compatibility, unreadable and still-loading rows', () => {
    expect(compatView(row('synth/user/a.aif'))).toEqual({ level: 'loading', label: 'reading', detail: null });
    expect(compatView(row('synth/user/a.aif', { compat: { level: 'ok' } }))).toEqual({
      level: 'ok',
      label: 'ok',
      detail: null,
    });
    expect(compatView(row('synth/user/a.aif', { compat: { level: 'warn', reason: 'engine x' } })).detail).toBe(
      'engine x',
    );
    expect(compatView(row('synth/user/a.aif', { error: 'bad FORM' }))).toEqual({
      level: 'unreadable',
      label: 'unreadable',
      detail: 'bad FORM',
    });
  });

  it('words a kind mismatch from the folder and metadata kinds', () => {
    expect(kindMismatchText(row('synth/user/a.aif'))).toBeNull();
    expect(kindMismatchText(row('synth/user/a.aif', { kindMismatch: true, info: info('k', 'drum') }))).toBe(
      'in synth folder, but metadata says drum',
    );
  });
});

describe('toRow', () => {
  it('keeps only the patch details the table needs, not its bytes', () => {
    const patch = { name: 'pad', kind: 'synth', meta: SYNTH_META, op1Canonical: false, raw: new Uint8Array(9) } as unknown as Patch;
    const r = toRow(entry('synth/user/a.aif', { patch }));
    expect(r.info).toEqual({ name: 'pad', kind: 'synth', hasMeta: true, canonical: false, traits: soundTraits(SYNTH_META) });
    expect('patch' in r).toBe(false);
  });
});

describe('mergeRows', () => {
  it('keeps read rows whose path and size are unchanged and loads the rest', () => {
    const kept = row('synth/user/a.aif', { info: info('a'), compat: { level: 'ok' } });
    const resized = row('synth/user/b.aif', { info: info('b') });
    const touched = row('synth/user/c.aif', { info: info('c') });
    const unread = row('synth/user/d.aif');
    const previous = new Map([kept, resized, touched, unread].map((r) => [r.path, r]));
    const listed = [
      entry('synth/user/a.aif'),
      entry('synth/user/b.aif', { sizeBytes: 99 }),
      entry('synth/user/c.aif'),
      entry('synth/user/d.aif'),
      entry('synth/user/e.aif'),
    ];
    const { rows, toLoad } = mergeRows(listed, previous, new Set(['synth/user/c.aif']));
    expect(rows[0]).toBe(kept);
    expect(rows.map((r) => r.path)).toEqual(listed.map((e) => e.path));
    expect(rows.slice(1).every((r) => r.info === null)).toBe(true);
    expect(toLoad.map((e) => e.fileName)).toEqual(['b.aif', 'c.aif', 'd.aif', 'e.aif']);
  });
});

describe('filterRows and sortRows', () => {
  const rows = [
    row('synth/user/b.aif', { info: info('Warm Pad'), sizeBytes: 300, compat: { level: 'warn' } }),
    row('drum/user/a.aif', { info: info('kit', 'drum'), sizeBytes: 100, compat: { level: 'ok' } }),
    row('synth/user/c.aif', { error: 'bad', sizeBytes: 200 }),
  ];

  it('filters by metadata name or file name, ignoring case', () => {
    expect(filterRows(rows, 'PAD').map((r) => r.fileName)).toEqual(['b.aif']);
    expect(filterRows(rows, 'c.AIF').map((r) => r.fileName)).toEqual(['c.aif']);
    expect(filterRows(rows, '  ')).toHaveLength(3);
  });

  it('sorts by each column in either direction', () => {
    const files = (key: Parameters<typeof sortRows>[1], dir: Parameters<typeof sortRows>[2]): string[] =>
      sortRows(rows, key, dir).map((r) => r.fileName);
    expect(files('size', 'ascending')).toEqual(['a.aif', 'c.aif', 'b.aif']);
    expect(files('size', 'descending')).toEqual(['b.aif', 'c.aif', 'a.aif']);
    expect(files('file', 'ascending')).toEqual(['a.aif', 'b.aif', 'c.aif']);
    expect(files('name', 'ascending')).toEqual(['c.aif', 'a.aif', 'b.aif']);
    expect(files('kind', 'ascending')).toEqual(['a.aif', 'b.aif', 'c.aif']);
    expect(files('compat', 'ascending')).toEqual(['a.aif', 'b.aif', 'c.aif']);
  });
});

describe('traitDetails', () => {
  it('lists engine, effect, LFO and selectors for a synth', () => {
    const traits = soundTraits({ ...SYNTH_META, type: 'supersaw', knobs: [0, 0, 0, 0, 0x5301, 0, 0, 0] })!;
    expect(traitDetails(traits)).toEqual([
      ['Engine', 'supersaw'],
      ['Effect', 'nitro'],
      ['LFO', 'value'],
      ['Engine selector', '0x5301'],
    ]);
  });

  it('names the opus plugin and effects and a Gin kit', () => {
    const opus = soundTraits({ ...SYNTH_META, type: 'iter', opus: { plugin: 'opus.dx', fx2_type: 'opus.chorus' } })!;
    expect(traitDetails(opus)).toContainEqual(['Plugin', 'opus.dx']);
    expect(traitDetails(opus)).toContainEqual(['opus effects', 'opus.chorus']);
    const gin = soundTraits({ ...SYNTH_META, fx_type: '', lfo_type: '', type: 'gin', gin_state: 'x' } as never)!;
    expect(traitDetails(gin)).toEqual([
      ['Engine', 'gin'],
      ['Gin kit', 'yes'],
    ]);
  });
});
