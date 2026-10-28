import { describe, it, expect } from 'vitest';
import { isSynthMeta, isDrumMeta, metaKind } from './op1-meta';
import { SYNTH_META, SAMPLER_META, DRUM_META } from '../../../test/fixtures/meta';

describe('op1 metadata guards', () => {
  it('recognises synth metadata including samplers', () => {
    expect(isSynthMeta(SYNTH_META)).toBe(true);
    expect(isSynthMeta(SAMPLER_META)).toBe(true);
    expect(isSynthMeta(DRUM_META)).toBe(false);
  });

  it('recognises drum metadata', () => {
    expect(isDrumMeta(DRUM_META)).toBe(true);
    expect(isDrumMeta(SYNTH_META)).toBe(false);
  });

  it('maps metadata to a patch kind', () => {
    expect(metaKind(SYNTH_META)).toBe('synth');
    expect(metaKind(DRUM_META)).toBe('drum');
    expect(metaKind({ name: 'mystery' })).toBe('unknown');
    expect(metaKind(null)).toBe('unknown');
  });

  it('tolerates extra fields from newer firmware', () => {
    expect(isSynthMeta({ ...SYNTH_META, future_field: [1, 2, 3] })).toBe(true);
  });
});
