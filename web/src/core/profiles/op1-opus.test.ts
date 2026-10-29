import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { op1Opus, OPUS_FX_TYPES, OPUS_MODS_MATCH, OPUS_SNIFF } from './op1-opus';
import { op1Stock, STOCK_SYNTH_ENGINES, STOCK_FX_TYPES, STOCK_LFO_TYPES, STOCK_DRUM_ENGINES } from './op1-stock';
import { detectProfiles, profiles, suggestProfile } from './registry';
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
const fifth = (value: number) => [0, 0, 0, 0, value, 0, 0, 0];
const STOCK_DRIVE = { drum: {}, synth: {}, tape: {} };

describe('op1/opus identity and layout', () => {
  it('is the opus OP-1 profile with the stock patch roots and a mods folder', () => {
    expect(op1Opus).toMatchObject({ id: 'op1/opus', device: 'op1', firmware: 'opus', label: 'OP-1 (opus)' });
    expect(op1Opus.layout.patchRoots).toEqual(op1Stock.layout.patchRoots);
    expect(op1Opus.layout.otherRoots).toEqual(['tape', 'album', 'mods']);
    expect(op1Opus.layout.backupOtherRoots).toEqual(['mods']);
    expect(op1Opus.patchKinds).toEqual(op1Stock.patchKinds);
  });

  it('supports the stock names plus the opus effects', () => {
    const { supports } = op1Opus;
    expect(supports.synthEngines).toEqual(STOCK_SYNTH_ENGINES);
    expect(supports.drumEngines).toEqual(STOCK_DRUM_ENGINES);
    expect(supports.lfo).toEqual(STOCK_LFO_TYPES);
    expect(supports.fx).toEqual([...STOCK_FX_TYPES, ...OPUS_FX_TYPES]);
    for (const name of ['bitcrush', 'chorus', 'plate', 'filter', 'opus.bitcrush']) expect(supports.fx).toContain(name);
  });

  it('is registered last', () => {
    expect(profiles.map((p) => p.id)).toEqual(['op1/stock', 'op1/hundekraft', 'op1/opus']);
  });
});

describe('op1/opus detection', () => {
  it('is very confident with a mods folder holding .opmod plugins', async () => {
    const root = new MemoryListing({ ...STOCK_DRIVE, mods: { 'a.OPMOD': 'file', dx: {} } });
    const result = await op1Opus.detect(root);
    expect(result).toEqual({ confidence: OPUS_MODS_MATCH, reason: 'found mods folder with opus plugins' });
    expect(OPUS_MODS_MATCH).toBe(0.97);
    expect((await detectProfiles(root))[0]?.profile.id).toBe('op1/opus');
  });

  it('is unsure with an empty mods folder and ignores folder case', async () => {
    const result = await op1Opus.detect(new MemoryListing({ ...STOCK_DRIVE, MODS: { 'readme.txt': 'file' } }));
    expect(result.confidence).toBe(0.6);
    expect(result.reason).toMatch(/no opus plugins/);
  });

  it('falls back to the stock folders capped at 0.5, so stock wins on a plain drive', async () => {
    const root = new MemoryListing(STOCK_DRIVE);
    expect((await op1Opus.detect(root)).confidence).toBe(0.5);
    expect((await detectProfiles(root))[0]?.profile.id).toBe('op1/stock');
    expect((await op1Opus.detect(new MemoryListing({ Music: {} }))).confidence).toBe(0);
  });

  it('treats a mods file, not folder, as no mods folder', async () => {
    expect((await op1Opus.detect(new MemoryListing({ ...STOCK_DRIVE, mods: 'file' }))).confidence).toBe(0.5);
  });
});

describe('op1/opus compatibility', () => {
  it('accepts stock sounds and opus effects saved by name', () => {
    for (const name of ['synth-cluster.aif', 'drum-boombap.aif', 'real/synth.aif', 'real/drum.aif']) {
      expect(op1Opus.compatibility(fixture(name)), name).toEqual({ level: 'ok' });
    }
    expect(op1Opus.compatibility(synth({ fx_type: 'bitcrush' }))).toEqual({ level: 'ok' });
  });

  it('warns that an opus engine sound needs its plugin', () => {
    const compat = op1Opus.compatibility(synth({ type: 'iter', knobs: fifth(0x100), opus: { plugin: 'opus.dx' } }));
    expect(compat).toEqual({ level: 'warn', reason: 'needs plugin opus.dx' });
  });

  it('warns that opus effect slots need their plugins', () => {
    const compat = op1Opus.compatibility(synth({ opus: { fx2_type: 'opus.chorus', fx2_active: 1 } }));
    expect(compat).toEqual({ level: 'warn', reason: 'needs plugin opus.chorus' });
  });

  it('warns that hundekraft sounds need the opus bridge plugin', () => {
    expect(op1Opus.compatibility(synth({ type: 'supersaw', knobs: fifth(0x5301) }))).toEqual({
      level: 'warn',
      reason: 'needs the opus bridge plugin',
    });
    expect(op1Opus.compatibility(synth({ fx_type: 'in a room' })).level).toBe('warn');
    expect(op1Opus.compatibility(synth({ lfo_type: 'quad' })).level).toBe('warn');
    const gin = withMeta(fixture('drum-boombap.aif'), { ...DRUM_META, type: 'gin', gin_state: 'x' } as Op1Meta);
    expect(op1Opus.compatibility(gin)).toEqual({ level: 'warn', reason: 'needs the opus bridge plugin' });
  });

  it('warns about op1r selectors on otherwise stock sounds', () => {
    expect(op1Opus.compatibility(synth({ knobs: fifth(0x5301) }))).toEqual({
      level: 'warn',
      reason: 'needs the opus bridge plugin',
    });
  });

  it('keeps names no firmware we know uses unsupported', () => {
    expect(op1Opus.compatibility(synth({ type: 'mystery' }))).toEqual({
      level: 'unsupported',
      reason: 'engine "mystery" is not part of this firmware',
    });
    expect(op1Opus.compatibility(fixture('no-metadata.aif')).level).toBe('unsupported');
  });

  it('stays strict on stock for opus sounds', () => {
    expect(op1Stock.compatibility(synth({ type: 'iter', opus: { plugin: 'opus.dx' } })).level).toBe('unsupported');
  });
});

describe('op1/opus sniff', () => {
  it('is confident when a loaded patch was saved on opus', () => {
    const result = op1Opus.sniff!([fixture('synth-cluster.aif'), synth({ opus: { plugin: 'opus.dx' } })].map(traitsOf));
    expect(result).toEqual({ confidence: OPUS_SNIFF, reason: '1 of 2 patches were saved on opus' });
    // Above stock's folder confidence (0.9), so a stock-looking drive of opus sounds gets the
    // suggestion; above hundekraft's sniff (0.95), because opus hosts hundekraft sounds through its
    // bridge; below the mods-folder match (0.97).
    expect(OPUS_SNIFF).toBe(0.96);
  });

  it('wins the suggestion over hundekraft for a drive mixing both', () => {
    const mixed = [synth({ type: 'supersaw' }), synth({ opus: { plugin: 'opus.dx' } })].map(traitsOf);
    expect(suggestProfile(mixed, op1Stock, 0.9)?.profile.id).toBe('op1/opus');
    expect(suggestProfile([traitsOf(synth({ opus: { plugin: 'opus.dx' } }))], op1Stock, 0.9)?.profile.id).toBe('op1/opus');
  });

  it('is not confident about stock or hundekraft sounds', () => {
    const result = op1Opus.sniff!([fixture('synth-cluster.aif'), synth({ type: 'supersaw' })].map(traitsOf));
    expect(result.confidence).toBe(0);
  });
});
