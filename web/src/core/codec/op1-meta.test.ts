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

  it('rejects metadata with wrongly typed or missing required fields', () => {
    expect(isSynthMeta({ ...SYNTH_META, synth_version: '2' })).toBe(false);
    expect(isSynthMeta({ ...SYNTH_META, name: 1 })).toBe(false);
    expect(isDrumMeta({ ...DRUM_META, start: undefined })).toBe(false);
    expect(isDrumMeta({ ...DRUM_META, name: 1 })).toBe(false);
    expect(metaKind([])).toBe('unknown');
  });
});

/** A DBox kit as far as we know its shape: stock drum fields without the per-slot sample arrays. */
const DBOX_META = {
  name: 'dbox kit',
  octave: 0,
  type: 'dbox',
  fx_active: false,
  fx_params: [8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000],
  fx_type: 'delay',
  lfo_active: false,
  lfo_params: [16000, 16000, 16000, 16000, 0, 0, 0, 0],
  lfo_type: 'tremolo',
};

/** A hundekraft Gin kit: DBox fields plus 24 slots of 8 numbers and a text state. */
const GIN_META = {
  ...DBOX_META,
  name: 'gin kit',
  type: 'gin',
  dbox_data: Array.from({ length: 24 }, () => [0, 1, 2, 3, 4, 5, 6, 7]),
  gin_state: 'v1;steps=16',
};

describe('drum synth kinds', () => {
  it('recognises stock DBox and hundekraft Gin kits as drums', () => {
    expect(isDrumMeta(DBOX_META)).toBe(true);
    expect(isDrumMeta(GIN_META)).toBe(true);
    expect(metaKind(DBOX_META)).toBe('drum');
    expect(metaKind(GIN_META)).toBe('drum');
  });

  it('never reads a drum synth kit as a synth', () => {
    expect(isSynthMeta({ ...DBOX_META, synth_version: 2, adsr: [], knobs: [] })).toBe(false);
    expect(isSynthMeta({ ...GIN_META, synth_version: 2, adsr: [], knobs: [] })).toBe(false);
  });

  it('still needs a string name', () => {
    expect(isDrumMeta({ ...DBOX_META, name: 1 })).toBe(false);
    expect(isDrumMeta({ ...GIN_META, name: undefined })).toBe(false);
    expect(metaKind({ type: 'gin' })).toBe('unknown');
  });

  it('keeps the sample-kit rule for type drum', () => {
    expect(isDrumMeta({ ...DBOX_META, type: 'drum' })).toBe(false);
  });
});
