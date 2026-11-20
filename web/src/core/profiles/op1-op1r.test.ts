import { describe, expect, it } from 'vitest';
import { soundTraits } from '../codec/sound-traits';
import { op1Op1r } from './op1-op1r';
import type { Op1Meta } from '../codec/op1-meta';
import { SYNTH_META } from '../../../test/fixtures/meta';

const traits = (meta: Record<string, unknown>) => {
  const t = soundTraits({ ...SYNTH_META, type: 'fm', knobs: [0, 0, 0, 0, 0, 0, 0, 0], ...meta } as Op1Meta);
  if (!t) throw new Error('no traits');
  return t;
};

describe('op1/op1r', () => {
  it('is an OP-1 profile with the stock engines and its own LFOs', () => {
    expect(op1Op1r.id).toBe('op1/op1r');
    expect(op1Op1r.supports.synthEngines).toContain('fm');
    expect(op1Op1r.supports.lfo).toEqual(expect.arrayContaining(['quad', 'pond']));
    expect(op1Op1r.sniff).toBeUndefined();
  });

  it('accepts a stock sound', () => {
    const judge = op1Op1r.pluginCompatibility?.({ library: new Map(), loaded: new Set() });
    expect(judge?.(traits({}))?.level).toBe('ok');
  });

  it('warns that a plugin engine that is not loaded plays as iter, and is fine once it is', () => {
    const context = { library: new Map([['supersaw', 'supersaw']]), loaded: new Set<string>() };
    const sound = traits({ type: 'supersaw', knobs: [0, 0, 0, 0, 0x5320, 0, 0, 0] });
    const result = op1Op1r.pluginCompatibility?.(context)?.(sound);
    expect(result?.level).toBe('warn');
    expect(result?.reason).toMatch(/needs plugin "supersaw"/);
    expect(op1Op1r.pluginCompatibility?.({ ...context, loaded: new Set(['supersaw']) })?.(sound)?.level).toBe('ok');
  });
});
