import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
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
} from './op1-stock';
import { MemoryListing } from '../store/memory-listing';
import { readPatch, withMeta } from '../codec/patch';
import type { Op1Meta } from '../codec/op1-meta';
import { SYNTH_META, DRUM_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

/** The hundekraft firmware insert (gitignored, GPL): read only to check names, never copied. */
const FIRMWARE_TABLE =
  process.env['CO_OP_FIRMWARE_TABLE'] ??
  fileURLToPath(new URL('../../../../vendor/hundekraft/op1-workshop-main/base/OP1_vdk.ldr.insert.bin', import.meta.url));

/** Every committed fixture with op-1 metadata, real device files included. */
const META_FIXTURES = ['real/synth.aif', 'real/drum.aif', 'synth-cluster.aif', 'synth-sampler-sowt.aif', 'drum-boombap.aif'];

/** The committed test-tones pack, which declares op1/stock. */
const TEST_TONES_DIR = fileURLToPath(new URL('../../../../packs/test-tones', import.meta.url));
const TEST_TONES = ['synth', 'drum'].flatMap((kind) =>
  readdirSync(`${TEST_TONES_DIR}/${kind}`).map((name) => `${kind}/${name}`),
);

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

  it.each(META_FIXTURES)('accepts %s', (name) => {
    expect(op1Stock.compatibility(fixture(name))).toEqual({ level: 'ok' });
  });

  it.each(TEST_TONES)('accepts the test-tones pack patch %s', (file) => {
    const patch = readPatch(new Uint8Array(readFileSync(`${TEST_TONES_DIR}/${file}`)));
    expect(op1Stock.compatibility(patch)).toEqual({ level: 'ok' });
  });

  it.each([
    ['engine', { type: 'wavetable' }, /engine "wavetable"/],
    ['effect', { fx_type: 'tape' }, /effect "tape"/],
    ['effect with spaces', { fx_type: 'in a room' }, /effect "in a room"/],
    ['LFO', { lfo_type: 'quad' }, /LFO "quad"/],
    ['opus member', { opus: { plugin: 'opus.dx' } }, /opus/],
  ])('marks a non-stock %s unsupported because it may stop the OP-1 at boot', (_what, change, reason) => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, ...change } as Op1Meta);
    const compat = op1Stock.compatibility(patch);
    expect(compat.level).toBe('unsupported');
    expect(compat.reason).toMatch(reason);
    expect(compat.reason).toMatch(/may stop a stock OP-1 at boot/);
  });

  it('only warns about a selector-like fifth knob on an otherwise stock sound', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, knobs: [0, 0, 0, 0, 0x5301, 0, 0, 0] });
    expect(op1Stock.compatibility(patch)).toEqual({
      level: 'warn',
      reason: 'fifth knob value matches a hundekraft engine selector; probably fine on stock',
    });
  });

  it('only warns about a selector-like fifth effect value on an otherwise stock sound', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, fx_params: [0, 0, 0, 0, 0x5300, 0, 0, 0] });
    expect(op1Stock.compatibility(patch)).toEqual({
      level: 'warn',
      reason: 'fifth effect value matches a hundekraft effect selector; probably fine on stock',
    });
  });

  it('does not add the selector note when the sound is already unsupported', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), {
      ...SYNTH_META,
      type: 'supersaw',
      knobs: [0, 0, 0, 0, 0x5301, 0, 0, 0],
    });
    expect(op1Stock.compatibility(patch)).toEqual({
      level: 'unsupported',
      reason: 'engine "supersaw" is not part of this firmware; may stop a stock OP-1 at boot',
    });
  });

  it('names every problem in one reason', () => {
    const patch = withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, type: 'supersaw', fx_type: 'tape' });
    expect(op1Stock.compatibility(patch).reason).toBe(
      'engine "supersaw" is not part of this firmware; effect "tape" is not part of this firmware; may stop a stock OP-1 at boot',
    );
  });

  it('accepts a DBox kit but not a hundekraft Gin kit', () => {
    const kit = fixture('drum-boombap.aif');
    expect(op1Stock.compatibility(withMeta(kit, { ...DRUM_META, type: 'dbox' } as Op1Meta)).level).toBe('ok');
    const gin = op1Stock.compatibility(withMeta(kit, { ...DRUM_META, type: 'gin', gin_state: 'x' } as Op1Meta));
    expect(gin.level).toBe('unsupported');
    expect(gin.reason).toMatch(/drum engine "gin"/);
  });

  it('declares the stock lists as what it supports', () => {
    expect(op1Stock.supports).toEqual({
      synthEngines: STOCK_SYNTH_ENGINES,
      drumEngines: STOCK_DRUM_ENGINES,
      fx: STOCK_FX_TYPES,
      lfo: STOCK_LFO_TYPES,
    });
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

describe('op1/stock layout', () => {
  it('scans synth and drum as patch roots', () => {
    expect(op1Stock.layout.patchRoots).toEqual([
      { path: 'synth', kind: 'synth' },
      { path: 'drum', kind: 'drum' },
    ]);
  });
});
