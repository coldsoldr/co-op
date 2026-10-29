import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { traitCompatibility, type ProfileSupports } from './compat';
import { readPatch, withMeta } from '../codec/patch';
import type { Op1Meta } from '../codec/op1-meta';
import { SYNTH_META } from '../../../test/fixtures/meta';

const fixture = (name: string) =>
  readPatch(new Uint8Array(readFileSync(fileURLToPath(new URL(`../../../test/fixtures/${name}`, import.meta.url)))));

const SUPPORTS: ProfileSupports = {
  synthEngines: ['cluster'],
  drumEngines: ['drum'],
  fx: ['nitro', 'delay'],
  lfo: ['value', 'tremolo'],
};

const synth = (change: Record<string, unknown>) =>
  withMeta(fixture('synth-cluster.aif'), { ...SYNTH_META, ...change } as Op1Meta);

describe('traitCompatibility', () => {
  const compat = traitCompatibility(SUPPORTS);

  it('accepts names it supports', () => {
    expect(compat(fixture('synth-cluster.aif'))).toEqual({ level: 'ok' });
    expect(compat(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('marks unsupported names unsupported', () => {
    expect(compat(synth({ type: 'fm' }))).toEqual({ level: 'unsupported', reason: 'engine "fm" is not part of this firmware' });
    expect(compat(synth({ lfo_type: 'crank' })).reason).toBe('LFO "crank" is not part of this firmware');
  });

  it('treats a missing effect or LFO as fine', () => {
    expect(compat(synth({ fx_type: '', lfo_type: undefined }))).toEqual({ level: 'ok' });
  });

  it('keeps patches without metadata or of unknown kind unsupported', () => {
    expect(compat(fixture('no-metadata.aif'))).toEqual({ level: 'unsupported', reason: 'file has no op-1 metadata' });
    const { synth_version: _dropped, ...rest } = SYNTH_META;
    expect(compat(withMeta(fixture('synth-cluster.aif'), rest as unknown as Op1Meta))).toEqual({
      level: 'unsupported',
      reason: 'op-1 metadata is of an unrecognised kind',
    });
  });

  it('lets a profile rule add, drop or soften findings', () => {
    const lenient = traitCompatibility(SUPPORTS, {
      rule: (traits, findings) => [
        ...findings.map((f) => ({ ...f, level: 'warn' as const })),
        ...(traits.lfo === 'value' ? [{ level: 'warn' as const, reason: 'value LFO is wobbly' }] : []),
      ],
    });
    expect(lenient(synth({ type: 'fm' }))).toEqual({
      level: 'warn',
      reason: 'engine "fm" is not part of this firmware; value LFO is wobbly',
    });
    expect(lenient(fixture('drum-boombap.aif'))).toEqual({ level: 'ok' });
  });

  it('reports the worst level and adds the note only to unsupported results', () => {
    const noted = traitCompatibility(SUPPORTS, {
      rule: (traits, findings) => (traits.engine === 'cluster' ? [{ level: 'warn', reason: 'hmm' }] : [...findings]),
      unsupportedNote: 'beware',
    });
    expect(noted(fixture('synth-cluster.aif'))).toEqual({ level: 'warn', reason: 'hmm' });
    expect(noted(synth({ type: 'fm' }))).toEqual({
      level: 'unsupported',
      reason: 'engine "fm" is not part of this firmware; beware',
    });
    expect(noted(fixture('no-metadata.aif')).reason).toBe('file has no op-1 metadata');
  });
});
