import { describe, expect, it } from 'vitest';
import type { PluginBuild, PluginIndex, PluginIndexEntry } from '@co-op/shared/plugins';
import type { Inventory, InventoryRecord } from '../usbmidi/inventory';
import { compareVersions, coopCoreOf, coreSatisfies, listUpdates, parseVersion } from './updates';

/** A coop plugin of the catalogue, hosted, at `version`. */
const coopPlugin = (id: string, version: string, more: Partial<PluginIndexEntry> = {}): PluginIndexEntry => ({
  id,
  name: id.replaceAll('_', ' '),
  firmware: 'op1/coop',
  kind: 'fx',
  author: 'coldsoldr',
  version,
  description: 'An effect.',
  file: `${id}.coop`,
  screenshot: `${id}/screenshot.png`,
  manual: `${id}/manual.md`,
  requires: 'coop e10',
  tags: [],
  sizeBytes: 1000,
  sha256: 'a'.repeat(64),
  savedName: id.replaceAll('_', ' '),
  ...more,
});

const indexOf = (plugins: PluginIndexEntry[]): PluginIndex => ({ format: 1, generatedAt: '2026-11-21T00:00:00.000Z', plugins });

const record = (id: string, version: string, more: Partial<InventoryRecord> = {}): InventoryRecord => ({
  kind: 'fx',
  name: id,
  source: 'coop',
  path: `plugins/${id}.coop`,
  version,
  author: 'coldsoldr',
  size: 900,
  crc: 0,
  state: 'loaded',
  claims: [],
  reason: '',
  enabled: true,
  ...more,
});

const inventory = (records: InventoryRecord[], build: string | null = 'e12'): Inventory => ({ core: null, presets: null, records, build });

describe('parseVersion and compareVersions', () => {
  it('reads dotted numbers, with a v or spaces around them', () => {
    expect(parseVersion('0.10')).toEqual([0, 10]);
    expect(parseVersion(' v2.1.0 ')).toEqual([2, 1, 0]);
    expect(parseVersion('3')).toEqual([3]);
  });

  it('reads nothing else', () => {
    for (const bad of ['', '–', 'beta', '1.2b', '1..2', '.5', '1.', '0.3-rc1']) expect(parseVersion(bad), bad).toBeNull();
  });

  it('compares segment by segment, as numbers', () => {
    expect(compareVersions('0.10', '0.9')).toBeGreaterThan(0);
    expect(compareVersions('0.9', '0.5')).toBeGreaterThan(0);
    expect(compareVersions('0.5', '0.10')).toBeLessThan(0);
    expect(compareVersions('1.0', '0.99')).toBeGreaterThan(0);
    expect(compareVersions('0.3', '0.3')).toBe(0);
    expect(compareVersions('1', '1.0.0')).toBe(0);
    expect(compareVersions('1.0.1', '1')).toBeGreaterThan(0);
    expect(compareVersions('v0.4', '0.3')).toBeGreaterThan(0);
  });

  it('does not compare what it cannot read', () => {
    expect(compareVersions('0.3', '')).toBeNull();
    expect(compareVersions('beta', '0.3')).toBeNull();
  });
});

describe('the core a build needs', () => {
  it('reads the core number of a requirement', () => {
    expect(coopCoreOf('coop e10')).toBe(10);
    expect(coopCoreOf('coop e4b')).toBe(4);
    expect(coopCoreOf('hundekraft 3.0.3')).toBeNull();
    expect(coopCoreOf(undefined)).toBeNull();
  });

  it('is met by that core and every later one', () => {
    expect(coreSatisfies('coop e10', 'e10')).toBe(true);
    expect(coreSatisfies('coop e10', 'e12')).toBe(true);
    expect(coreSatisfies('coop e12', 'e11')).toBe(false);
    expect(coreSatisfies('coop e4b', 'e4')).toBe(true);
    // Nothing to judge by: the OP-1 decides at boot.
    expect(coreSatisfies(undefined, 'e3')).toBe(true);
    expect(coreSatisfies('coop e12', null)).toBe(true);
    expect(coreSatisfies('coop e12', 'custom')).toBe(true);
  });
});

describe('listUpdates', () => {
  const index = indexOf([
    coopPlugin('tape_echo', '0.4'),
    coopPlugin('rings', '0.10', { kind: 'synth' }),
    coopPlugin('saw', '0.2', { kind: 'synth' }),
    coopPlugin('drive', '0.2'),
    coopPlugin('fickle', '0.3', { requires: 'coop e12' }),
  ]);

  it('lists the files whose catalogue build is newer, in the inventory’s order', () => {
    const list = listUpdates(inventory([record('rings', '0.9'), record('saw', '0.2'), record('tape_echo', '0.3'), record('fickle', '0.2')]), index);
    expect(list.updates.map((u) => [u.file, u.from, u.to])).toEqual([
      ['rings.coop', '0.9', '0.10'],
      ['tape_echo.coop', '0.3', '0.4'],
      ['fickle.coop', '0.2', '0.3'],
    ]);
    expect(list.blocked).toEqual([]);
    expect(list.updates[0]?.plugin).toMatchObject({ id: 'rings', file: 'rings.coop', version: '0.10' });
  });

  it('never offers the same or an older version', () => {
    const list = listUpdates(inventory([record('rings', '0.10'), record('tape_echo', '0.5'), record('drive', '1'), record('saw', '0.2.0')]), index);
    expect(list).toEqual({ updates: [], blocked: [] });
  });

  it('offers the catalogue’s build to a file whose version it cannot read, as unknown', () => {
    const list = listUpdates(inventory([record('saw', ''), record('drive', 'beta'), record('rings', '0.10')]), index);
    expect(list.updates.map((u) => [u.file, u.from, u.to])).toEqual([
      ['saw.coop', null, '0.2'],
      ['drive.coop', null, '0.2'],
    ]);
  });

  it('leaves alone the files the catalogue does not hold', () => {
    expect(listUpdates(inventory([record('my_synth', '0.1'), record('mine', '')]), index).updates).toEqual([]);
  });

  it('updates a file that is switched off, failed or came after the boot, by the version the inventory gives', () => {
    const list = listUpdates(
      inventory([
        record('tape_echo', '0.3', { enabled: false, state: 'off' }),
        record('rings', '0.5', { state: 'failed', reason: 'over the plugin memory budget' }),
        record('saw', '0.1', { state: 'pending' }),
        record('drive', '0.2', { state: 'pending' }),
      ]),
      index,
    );
    expect(list.updates.map((u) => u.file)).toEqual(['tape_echo.coop', 'rings.coop', 'saw.coop']);
  });

  it('sets aside a build that needs a later core than the OP-1 runs', () => {
    const list = listUpdates(inventory([record('fickle', '0.2'), record('tape_echo', '0.3')], 'e11'), index);
    expect(list.updates.map((u) => u.file)).toEqual(['tape_echo.coop']);
    expect(list.blocked.map((b) => [b.file, b.from, b.to, b.needs])).toEqual([['fickle.coop', '0.2', '0.3', 'coop e12']]);
    // Without a core tag there is nothing to hold it back by.
    expect(listUpdates(inventory([record('fickle', '0.2')], null), index).updates).toHaveLength(1);
  });

  it('takes the coop build of a plugin with a file per firmware, and no build of another firmware', () => {
    const knl: PluginBuild = { firmware: 'op1/hundekraft', file: 'rings.knl', version: '0.1', sizeBytes: 8, sha256: 'b'.repeat(64) };
    const coop: PluginBuild = { firmware: 'op1/coop', file: 'rings.coop', version: '0.10', requires: 'coop e5', sizeBytes: 9, sha256: 'c'.repeat(64) };
    const both = indexOf([{ ...coopPlugin('rings', '0.1'), ...knl, builds: [knl, coop] }, coopPlugin('only_knl', '9.0', { firmware: 'op1/hundekraft', file: 'only_knl.knl' })]);
    const list = listUpdates(inventory([record('rings', '0.5'), record('only_knl', '0.1')]), both);
    expect(list.updates.map((u) => [u.file, u.to, u.plugin.sizeBytes])).toEqual([['rings.coop', '0.10', 9]]);
  });

  it('leaves out a listed build, which co-op has no file of', () => {
    const listed = indexOf([{ ...coopPlugin('saw', '0.9'), file: undefined, sizeBytes: undefined, sha256: undefined, link: 'https://example.org/saw' } as unknown as PluginIndexEntry]);
    expect(listUpdates(inventory([record('saw', '0.1')]), listed).updates).toEqual([]);
  });

  it('has nothing to say without an inventory or an index', () => {
    expect(listUpdates(null, index)).toEqual({ updates: [], blocked: [] });
    expect(listUpdates(inventory([record('saw', '0.1')]), null)).toEqual({ updates: [], blocked: [] });
  });

  it('matches hundekraft files the same way, without the coop core check', () => {
    const hk = indexOf([coopPlugin('grids', '1.2', { firmware: 'op1/hundekraft', file: 'grids.knl', requires: 'hundekraft 3.0.3' })]);
    const inv = inventory([{ ...record('grids', '1.1'), source: 'knl', path: 'plugins/grids.knl' }], null);
    expect(listUpdates(inv, hk, 'op1/hundekraft').updates.map((u) => [u.file, u.from, u.to])).toEqual([['grids.knl', '1.1', '1.2']]);
  });
});
