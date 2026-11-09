import { describe, expect, it } from 'vitest';
import { soundTraits } from '../codec/sound-traits';
import { op1Coop } from './op1-coop';
import type { Op1Meta } from '../codec/op1-meta';
import { SYNTH_META } from '../../../test/fixtures/meta';

const traits = (meta: Record<string, unknown>) => {
  const t = soundTraits({ ...SYNTH_META, type: 'fm', knobs: [0, 0, 0, 0, 0, 0, 0, 0], ...meta } as Op1Meta);
  if (!t) throw new Error('no traits');
  return t;
};

describe('op1/coop', () => {
  it('is an OP-1 profile with the stock layout', () => {
    expect(op1Coop.id).toBe('op1/coop');
    expect(op1Coop.supports.synthEngines).toContain('fm');
  });

  it('sniffs sounds saved on coop by their coop member', () => {
    expect(op1Coop.sniff?.([traits({ coop: { bend: 12 } }), traits({})]).confidence).toBe(0.95);
    expect(op1Coop.sniff?.([traits({})]).confidence).toBe(0);
  });

  it('warns that an engine plugin that is not loaded plays as iter', () => {
    const context = { library: new Map([['saw', 'saw']]), loaded: new Set<string>() };
    const judge = op1Coop.pluginCompatibility?.(context);
    const result = judge?.(traits({ type: 'saw', knobs: [0, 0, 0, 0, 0x5320, 0, 0, 0] }));
    expect(result?.level).toBe('warn');
    expect(result?.reason).toMatch(/needs plugin "saw"/);
    const loaded = op1Coop.pluginCompatibility?.({ ...context, loaded: new Set(['saw']) });
    expect(loaded?.(traits({ type: 'saw', knobs: [0, 0, 0, 0, 0x5320, 0, 0, 0] }))?.level).toBe('ok');
  });
});
