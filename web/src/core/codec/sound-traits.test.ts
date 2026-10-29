import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { unzipSync } from 'fflate';
import { soundTraits } from './sound-traits';
import { readPatch } from './patch';
import type { Op1Meta } from './op1-meta';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';

const meta = (extra: Record<string, unknown>): Op1Meta => ({ ...SYNTH_META, ...extra }) as Op1Meta;

describe('soundTraits', () => {
  it('returns null without metadata', () => {
    expect(soundTraits(null)).toBeNull();
  });

  it('reads a stock synth', () => {
    expect(soundTraits(SYNTH_META)).toEqual({
      kind: 'synth',
      engine: 'cluster',
      fx: 'nitro',
      lfo: 'value',
      engineSelector: null,
      opusMark: null,
      fxSelector: null,
      hasGin: false,
      hasOpus: false,
      opusPlugin: null,
      opusEffects: [],
    });
    expect(soundTraits(SAMPLER_META)?.engine).toBe('sampler');
  });

  it('reads a stock drum kit', () => {
    const traits = soundTraits(DRUM_META);
    expect(traits?.kind).toBe('drum');
    expect(traits?.engine).toBe('drum');
    expect(traits?.engineSelector).toBeNull();
    expect(traits?.fx).toBe('delay');
  });

  it('keeps the effect and LFO names even when switched off', () => {
    const traits = soundTraits(meta({ fx_active: false, lfo_active: false, fx_type: 'tape', lfo_type: 'quad' }));
    expect(traits?.fx).toBe('tape');
    expect(traits?.lfo).toBe('quad');
  });

  it('treats missing or empty effect and LFO names as none', () => {
    expect(soundTraits(meta({ fx_type: '', lfo_type: undefined }))).toMatchObject({ fx: null, lfo: null });
  });

  it('picks up op1r selectors in knobs[4] and fx_params[4]', () => {
    const traits = soundTraits(
      meta({ type: 'supersaw', knobs: [0, 0, 0, 0, 0x5301, 0, 0, 0], fx_params: [0, 0, 0, 0, 0x5300, 0, 0, 0] }),
    );
    expect(traits?.engineSelector).toBe(0x5301);
    expect(traits?.fxSelector).toBe(0x5300);
  });

  it('ignores values just outside the selector range', () => {
    const traits = soundTraits(
      meta({ knobs: [0, 0, 0, 0, 0x52ff, 0, 0, 0], fx_params: [0, 0, 0, 0, 0x5400, 0, 0, 0] }),
    );
    expect(traits?.engineSelector).toBeNull();
    expect(traits?.fxSelector).toBeNull();
  });

  it('reads an opus engine mark only when there is an opus member', () => {
    const knobs = [0, 0, 0, 0, 0x100, 0, 0, 0];
    expect(soundTraits(meta({ type: 'iter', knobs }))).toMatchObject({ engineSelector: null, opusMark: null });
    const opus = soundTraits(meta({ type: 'iter', knobs, opus: { plugin: 'opus.dx' } }));
    expect(opus?.opusMark).toBe(0x100);
    expect(opus?.engineSelector).toBeNull();
    expect(opus?.hasOpus).toBe(true);
    expect(opus?.opusPlugin).toBe('opus.dx');
  });

  it('lists the opus effect slots and falls back to any string for the plugin', () => {
    const traits = soundTraits(
      meta({ opus: { fx2_type: 'opus.bitcrush', fx2_active: 1, fx2_params: [1, 2], fx3_type: '', thing: 'opus.amp' } }),
    );
    expect(traits?.opusEffects).toEqual(['opus.bitcrush']);
    expect(traits?.opusPlugin).toBe('opus.amp');
  });

  it('only falls back to strings that look like an opus plugin id', () => {
    expect(soundTraits(meta({ opus: { note: 'hello there' } }))?.opusPlugin).toBeNull();
    expect(soundTraits(meta({ opus: { note: 'opus.bad id' } }))?.opusPlugin).toBeNull();
    expect(soundTraits(meta({ opus: { note: 'opus.out_a-time2' } }))?.opusPlugin).toBe('opus.out_a-time2');
  });

  it('has no plugin when the opus member names none', () => {
    const traits = soundTraits(meta({ opus: { fx2_type: 'opus.chorus' } }));
    expect(traits?.hasOpus).toBe(true);
    expect(traits?.opusPlugin).toBeNull();
  });

  it('spots a Gin kit', () => {
    const gin = {
      ...DRUM_META,
      type: 'gin',
      dbox_data: Array.from({ length: 24 }, () => [0, 0, 0, 0, 0, 0, 0, 0]),
      gin_state: 'x',
    } as unknown as Op1Meta;
    expect(soundTraits(gin)).toMatchObject({ kind: 'drum', engine: 'gin', hasGin: true });
    expect(soundTraits({ ...DRUM_META, type: 'dbox' } as Op1Meta)).toMatchObject({ kind: 'drum', engine: 'dbox', hasGin: false });
    expect(soundTraits({ ...DRUM_META, type: 'gin' } as Op1Meta)?.hasGin).toBe(true);
  });

  it('does not take dbox_data alone for a Gin kit: stock DBox kits may carry it too', () => {
    const dbox = { ...DRUM_META, type: 'dbox', dbox_data: [[0, 0, 0, 0, 0, 0, 0, 0]] } as unknown as Op1Meta;
    expect(soundTraits(dbox)?.hasGin).toBe(false);
  });
});

/** hundekraft's factory presets (gitignored, GPL): read in place, never copied into the repo. */
const PRESETS =
  process.env['CO_OP_PRESETS_ZIP'] ??
  fileURLToPath(new URL('../../../../vendor/hundekraft/op1-workshop-main/variants/7ff/presets.zip', import.meta.url));

/**
 * Read lazily: vitest runs a describe callback even when the block is skipped, so reading the zip
 * there fails on machines without vendor/ (CI).
 */
let cached: { name: string; traits: ReturnType<typeof soundTraits> }[] | null = null;
function presets() {
  cached ??= Object.entries(unzipSync(new Uint8Array(readFileSync(PRESETS))))
    .filter(([name]) => /\.aiff?$/i.test(name))
    .map(([name, bytes]) => ({ name, traits: soundTraits(readPatch(bytes).meta) }));
  return cached;
}

describe.skipIf(!existsSync(PRESETS))('soundTraits on the hundekraft factory presets', () => {

  it('reads all 152 presets as synths', () => {
    expect(presets()).toHaveLength(152);
    for (const { name, traits } of presets()) {
      expect(traits?.kind, name).toBe('synth');
      expect(traits?.engine, name).not.toBe('');
    }
  });

  it('finds an engine selector on every non-iter engine and none on iter', () => {
    for (const { name, traits } of presets()) {
      if (traits?.engine === 'iter') expect(traits.engineSelector, name).toBeNull();
      else expect(traits?.engineSelector, name).not.toBeNull();
    }
  });

  it('has no opus or Gin traits', () => {
    for (const { traits } of presets()) expect(traits).toMatchObject({ hasOpus: false, hasGin: false, opusEffects: [] });
  });
});
