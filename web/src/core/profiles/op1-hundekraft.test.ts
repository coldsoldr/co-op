import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { unzipSync } from 'fflate';
import {
  op1Hundekraft,
  HUNDEKRAFT_FULL_MATCH,
  HUNDEKRAFT_SYNTH_ENGINES,
  HUNDEKRAFT_FX_TYPES,
  HUNDEKRAFT_SNIFF,
} from './op1-hundekraft';
import { op1Stock, FULL_MATCH, STOCK_SYNTH_ENGINES, STOCK_FX_TYPES, STOCK_LFO_TYPES } from './op1-stock';
import { MemoryListing } from '../store/memory-listing';
import { readPatch, withMeta, type Patch } from '../codec/patch';
import type { Op1Meta } from '../codec/op1-meta';
import { soundTraits, type SoundTraits } from '../codec/sound-traits';
import { SYNTH_META, DRUM_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

const synth = (change: Record<string, unknown>) =>
  withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, ...change } as Op1Meta);

const traitsOf = (patch: Patch): SoundTraits => soundTraits(patch.meta) as SoundTraits;

const selector = (value: number) => [0, 0, 0, 0, value, 0, 0, 0];

describe('op1/hundekraft identity and layout', () => {
  it('is the hundekraft OP-1 profile with the stock layout', () => {
    expect(op1Hundekraft.id).toBe('op1/hundekraft');
    expect(op1Hundekraft.device).toBe('op1');
    expect(op1Hundekraft.firmware).toBe('hundekraft');
    expect(op1Hundekraft.label).toBe('OP-1 (hundekraft)');
    expect(op1Hundekraft.layout).toEqual(op1Stock.layout);
    expect(op1Hundekraft.patchKinds).toEqual(op1Stock.patchKinds);
  });

  it('supports every stock name plus its own', () => {
    const { supports } = op1Hundekraft;
    for (const name of STOCK_SYNTH_ENGINES) expect(supports.synthEngines).toContain(name);
    for (const name of STOCK_FX_TYPES) expect(supports.fx).toContain(name);
    for (const name of STOCK_LFO_TYPES) expect(supports.lfo).toContain(name);
    expect(supports.drumEngines).toEqual(['drum', 'dbox', 'gin']);
    expect(supports.lfo).toContain('quad');
    expect(HUNDEKRAFT_SYNTH_ENGINES).toContain('supersaw');
    expect(HUNDEKRAFT_FX_TYPES).toContain('in a room');
  });
});

describe('op1/hundekraft detection', () => {
  it('matches the stock folders a little below stock so stock wins on folders alone', async () => {
    const root = new MemoryListing({ drum: {}, synth: {}, tape: {} });
    const result = await op1Hundekraft.detect(root);
    expect(result.confidence).toBe(HUNDEKRAFT_FULL_MATCH);
    expect(HUNDEKRAFT_FULL_MATCH).toBe(0.85);
    expect(HUNDEKRAFT_FULL_MATCH).toBeLessThan(FULL_MATCH);
    expect((await op1Hundekraft.detect(new MemoryListing({ drum: {}, synth: {} }))).confidence).toBeLessThan(
      (await op1Stock.detect(new MemoryListing({ drum: {}, synth: {} }))).confidence,
    );
  });

  it('rejects an unrelated folder', async () => {
    expect((await op1Hundekraft.detect(new MemoryListing({ Music: {} }))).confidence).toBe(0);
  });
});

describe('op1/hundekraft sniff', () => {
  const sniff = (patches: Patch[]) => op1Hundekraft.sniff!(patches.map(traitsOf));

  it('is confident when a patch uses a hundekraft engine, effect, LFO or Gin kit', () => {
    for (const patch of [
      synth({ type: 'supersaw' }),
      synth({ fx_type: 'in a room' }),
      synth({ lfo_type: 'quad' }),
      withMeta(fixture('drum-boombap.aif'), { ...DRUM_META, type: 'gin', gin_state: 'x' } as Op1Meta),
    ]) {
      const result = sniff([fixture('synth-cluster.aif'), patch]);
      expect(result.confidence, JSON.stringify(patch.meta)).toBe(HUNDEKRAFT_SNIFF);
      expect(result.reason).toMatch(/1 of 2 patches/);
    }
    expect(HUNDEKRAFT_SNIFF).toBe(0.95);
  });

  it('is not confident about stock patches, iter, DBox, opus sounds or selector-like knob values', () => {
    const result = sniff([
      fixture('synth-cluster.aif'),
      fixture('real/synth.aif'),
      fixture('real/drum.aif'),
      synth({ type: 'iter' }),
      withMeta(fixture('drum-boombap.aif'), { ...DRUM_META, type: 'dbox' } as Op1Meta),
      synth({ type: 'iter', knobs: selector(0x100), opus: { plugin: 'opus.dx' } }),
      synth({ knobs: selector(0x5301) }),
      synth({ fx_params: selector(0x5304) }),
    ]);
    expect(result.confidence).toBe(0);
    expect(op1Hundekraft.sniff!([]).confidence).toBe(0);
  });
});

describe('op1/hundekraft compatibility', () => {
  it('accepts stock and hundekraft sounds', () => {
    for (const name of ['synth-cluster.aif', 'drum-boombap.aif', 'real/synth.aif', 'real/drum.aif']) {
      expect(op1Hundekraft.compatibility(fixture(name)), name).toEqual({ level: 'ok' });
    }
    expect(
      op1Hundekraft.compatibility(synth({ type: 'supersaw', knobs: selector(0x5301), fx_type: 'tape', lfo_type: 'quad' })),
    ).toEqual({ level: 'ok' });
    const gin = withMeta(fixture('drum-boombap.aif'), { ...DRUM_META, type: 'gin', gin_state: 'x' } as Op1Meta);
    expect(op1Hundekraft.compatibility(gin)).toEqual({ level: 'ok' });
  });

  it('warns that an unknown engine with a selector plays as stock Iter', () => {
    const compat = op1Hundekraft.compatibility(synth({ type: 'newthing', knobs: selector(0x5340) }));
    expect(compat.level).toBe('warn');
    expect(compat.reason).toMatch(/newthing/);
    expect(compat.reason).toMatch(/plugin may not be installed; plays as stock Iter/);
  });

  it('warns that an unknown effect with a selector plays as the stock filter', () => {
    const compat = op1Hundekraft.compatibility(synth({ fx_type: 'newfx', fx_params: selector(0x5340) }));
    expect(compat.level).toBe('warn');
    expect(compat.reason).toMatch(/newfx.*plays as the stock filter/);
  });

  it('keeps unknown names without a selector unsupported', () => {
    const compat = op1Hundekraft.compatibility(synth({ type: 'newthing' }));
    expect(compat).toEqual({ level: 'unsupported', reason: 'engine "newthing" is not part of this firmware' });
  });

  it('warns that opus sounds need the opus bridge or opus firmware', () => {
    const compat = op1Hundekraft.compatibility(synth({ type: 'iter', knobs: selector(0x100), opus: { plugin: 'opus.dx' } }));
    expect(compat).toEqual({ level: 'warn', reason: 'needs the opus bridge or opus firmware' });
  });

  it('marks files without metadata unsupported', () => {
    expect(op1Hundekraft.compatibility(fixture('no-metadata.aif')).level).toBe('unsupported');
  });
});

/** hundekraft's factory presets (gitignored, GPL): read in place, never copied into the repo. */
const PRESETS =
  process.env['CO_OP_PRESETS_ZIP'] ??
  fileURLToPath(new URL('../../../../vendor/hundekraft/op1-workshop-main/variants/7ff/presets.zip', import.meta.url));

/** Read lazily: vitest runs describe callbacks even for skipped blocks. */
let cached: Patch[] | null = null;
function presets(): Patch[] {
  cached ??= Object.entries(unzipSync(new Uint8Array(readFileSync(PRESETS))))
    .filter(([name]) => /\.aiff?$/i.test(name))
    .map(([, bytes]) => readPatch(bytes));
  return cached;
}

describe.skipIf(!existsSync(PRESETS))('op1/hundekraft on its factory presets', () => {
  it('accepts every preset', () => {
    for (const patch of presets()) expect(op1Hundekraft.compatibility(patch), patch.name).toEqual({ level: 'ok' });
  });

  it('sniffs the presets as hundekraft', () => {
    expect(op1Hundekraft.sniff!(presets().map(traitsOf)).confidence).toBe(HUNDEKRAFT_SNIFF);
  });

  it('splits them into stock-safe iter presets and the rest, which stock refuses', () => {
    const stockOk = presets().filter((p) => op1Stock.compatibility(p).level === 'ok');
    const refused = presets().filter((p) => op1Stock.compatibility(p).level === 'unsupported');
    expect(stockOk.length + refused.length).toBe(152);
    for (const patch of stockOk) expect(patch.meta?.type).toBe('iter');
    for (const patch of refused) expect(op1Stock.compatibility(patch).reason).toMatch(/may stop a stock OP-1 at boot/);
  });
});
