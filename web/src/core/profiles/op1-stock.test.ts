import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  op1Stock,
  STOCK_SYNTH_ENGINES,
  STOCK_DRUM_ENGINES,
  STOCK_FX_TYPES,
  STOCK_LFO_TYPES,
  FULL_MATCH,
  PARTIAL_MATCH,
  folderDetect,
  engineCompatibility,
} from './op1-stock';
import { MemoryListing } from '../store/memory-listing';
import { readPatch, withMeta } from '../codec/patch';
import type { Op1Meta } from '../codec/op1-meta';
import { SYNTH_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

/** The hundekraft firmware insert (gitignored, GPL): read only to check names, never copied. */
const FIRMWARE_TABLE = fileURLToPath(
  new URL('../../../../vendor/hundekraft/op1-workshop-main/base/OP1_vdk.ldr.insert.bin', import.meta.url),
);

/** Every committed fixture with op-1 metadata, real device files included. */
const META_FIXTURES = ['real/synth.aif', 'real/drum.aif', 'synth-cluster.aif', 'synth-sampler-sowt.aif', 'drum-boombap.aif'];

describe('stock name lists', () => {
  it('lists the stock effects, LFOs and drum engines', () => {
    expect([...STOCK_FX_TYPES].sort()).toEqual(['cwo', 'delay', 'grid', 'nitro', 'phone', 'punch', 'spring']);
    expect([...STOCK_LFO_TYPES].sort()).toEqual(['bend', 'crank', 'element', 'midi', 'random', 'tremolo', 'value']);
    expect([...STOCK_DRUM_ENGINES].sort()).toEqual(['dbox', 'drum']);
  });

  it.each(META_FIXTURES)('%s uses only stock names', (name) => {
    const meta = fixture(name).meta!;
    expect(STOCK_FX_TYPES).toContain(meta.fx_type);
    expect(STOCK_LFO_TYPES).toContain(meta.lfo_type);
    expect([...STOCK_SYNTH_ENGINES, ...STOCK_DRUM_ENGINES]).toContain(meta.type);
  });

  it.skipIf(!existsSync(FIRMWARE_TABLE))('finds every stock name in the firmware string table', () => {
    const table = readFileSync(FIRMWARE_TABLE).toString('latin1');
    const names = [...STOCK_SYNTH_ENGINES, ...STOCK_DRUM_ENGINES, ...STOCK_FX_TYPES, ...STOCK_LFO_TYPES];
    for (const name of names) expect(table, name).toContain(`\0${name}\0`);
  });
});

describe('op1/stock detection', () => {
  it('is confident when synth, drum and tape folders exist', async () => {
    const root = new MemoryListing({ album: {}, drum: {}, synth: {}, tape: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBeGreaterThanOrEqual(0.9);
    expect(result.confidence).toBe(FULL_MATCH);
    expect(result.reason).toMatch(/synth, drum and tape/);
  });

  it('stays below certain because the OP-1 Field shows the same folders', () => {
    expect(FULL_MATCH).toBeLessThan(1);
    expect(PARTIAL_MATCH).toBeLessThan(FULL_MATCH);
  });

  it('ignores folder name case', async () => {
    const root = new MemoryListing({ DRUM: {}, SYNTH: {}, TAPE: {} });
    expect((await op1Stock.detect(root)).confidence).toBeGreaterThanOrEqual(0.9);
  });

  it('is unsure with only two of the folders', async () => {
    const root = new MemoryListing({ drum: {}, synth: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBe(0.5);
    expect(result.reason).toMatch(/tape/);
  });

  it('rejects a single signature folder but names it', async () => {
    const root = new MemoryListing({ synth: {}, Documents: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBe(0);
    expect(result.reason).toBe('found only synth');
  });

  it('rejects an unrelated folder', async () => {
    const root = new MemoryListing({ Documents: {}, Music: {} });
    const result = await op1Stock.detect(root);
    expect(result.confidence).toBe(0);
    expect(result.reason).toBe('no OP-1 folders found');
  });

  it('ignores files that happen to share a signature name', async () => {
    const root = new MemoryListing({ synth: 'file', drum: 'file', tape: 'file' });
    expect((await op1Stock.detect(root)).confidence).toBe(0);
  });
});

describe('folderDetect factory', () => {
  const detect = folderDetect(['plugins', 'presets']);

  it('builds a detector for custom signature folders', async () => {
    expect((await detect(new MemoryListing({ plugins: {}, presets: {} }))).confidence).toBe(FULL_MATCH);
    expect((await detect(new MemoryListing({ plugins: {} }))).confidence).toBe(0);
    expect((await detect(new MemoryListing({ synth: {} }))).confidence).toBe(0);
  });
});

describe('op1/stock compatibility', () => {
  it('accepts stock synth and drum patches', () => {
    expect(op1Stock.compatibility(fixture('synth-cluster.aif'))).toEqual({ level: 'ok' });
    expect(op1Stock.compatibility(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('warns on an engine stock firmware does not have', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'wavetable' });
    const compat = op1Stock.compatibility(patch);
    expect(compat.level).toBe('warn');
    expect(compat.reason).toMatch(/wavetable/);
  });

  it('marks files without op-1 metadata unsupported', () => {
    const compat = op1Stock.compatibility(fixture('no-metadata.aif'));
    expect(compat.level).toBe('unsupported');
    expect(compat.reason).toBe('file has no op-1 metadata');
  });

  it('marks metadata of an unrecognised kind unsupported', () => {
    const { synth_version: _dropped, ...rest } = SYNTH_META;
    const mystery = { ...rest, type: 'mystery' } as unknown as Op1Meta;
    const patch = withMeta(fixture('synth-cluster.aif'), mystery);
    expect(patch.kind).toBe('unknown');
    expect(patch.meta).not.toBeNull();
    const compat = op1Stock.compatibility(patch);
    expect(compat.level).toBe('unsupported');
    expect(compat.reason).toBe('op-1 metadata is of an unrecognised kind');
  });

  it('knows every stock engine', () => {
    expect([...STOCK_SYNTH_ENGINES].sort()).toEqual([
      'cluster',
      'digital',
      'dna',
      'drwave',
      'dsynth',
      'fm',
      'iter',
      'phase',
      'pulse',
      'sampler',
      'string',
      'voltage',
    ]);
  });
});

describe('engineCompatibility factory', () => {
  it('accepts whatever engines it is given', () => {
    const compat = engineCompatibility(['cluster', 'wavetable']);
    const wavetable = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'wavetable' });
    expect(compat(wavetable)).toEqual({ level: 'ok' });
    expect(compat(fixture('synth-cluster.aif'))).toEqual({ level: 'ok' });
    expect(compat(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('still warns on engines outside its list', () => {
    const compat = engineCompatibility(['cluster']);
    const sampler = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'sampler' });
    expect(compat(sampler).level).toBe('warn');
    expect(compat(sampler).reason).toMatch(/sampler/);
  });
});

describe('op1/stock layout', () => {
  it('scans synth and drum as patch roots', () => {
    expect(op1Stock.layout.patchRoots).toEqual([
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ]);
  });
});
