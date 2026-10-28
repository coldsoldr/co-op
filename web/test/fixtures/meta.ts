import type { SynthMeta, DrumMeta } from '../../src/core/codec/op1-meta';

export const SYNTH_META: SynthMeta = {
  adsr: [64, 64, 0, 64, 14336, 64, 4000, 4000],
  fx_active: true,
  fx_params: [64, -14337, 4515, 7232, 0, 0, 0, 0],
  fx_type: 'nitro',
  knobs: [3072, 0, 512, 3, 0, 0, 0, 0],
  lfo_active: false,
  lfo_params: [4608, 32767, 8448, 15360, 0, 0, 0, 0],
  lfo_type: 'value',
  name: 'default',
  octave: 0,
  synth_version: 2,
  type: 'cluster',
};

export const SAMPLER_META: SynthMeta = {
  adsr: [512, 10746, 32767, 10000, 4000, 64, 4000, 4000],
  base_freq: 261.6253662109375,
  fx_active: false,
  fx_params: [4480, 15544, 10788, 12104, 0, 0, 0, 0],
  fx_type: 'delay',
  knobs: [0, 0, 32767, 32767, 12000, 0, 0, 8304],
  lfo_active: false,
  lfo_params: [11840, 18431, 1024, 15144, 0, 0, 0, 0],
  lfo_type: 'random',
  name: '20150108_0251',
  octave: 0,
  synth_version: 1,
  type: 'sampler',
};

const slots = (value: number): number[] => Array.from({ length: 24 }, () => value);

export const DRUM_META: DrumMeta = {
  drum_version: 2,
  dyna_env: [0, 8192, 0, 8192, 0, 0, 0, 0],
  end: [
    97643143, 165163892, 211907777, 282025634, 313446583, 372916297, 413167412, 454132733,
    478541489, 492549640, 582028126, 642634075, 642634075, 642634075, 642634075, 642634075,
    642634075, 642634075, 642634075, 642634075, 642634075, 642634075, 642634075, 2032606256,
  ],
  fx_active: false,
  fx_params: [8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000],
  fx_type: 'delay',
  lfo_active: false,
  lfo_params: [16000, 16000, 16000, 16000, 0, 0, 0, 0],
  lfo_type: 'tremolo',
  name: 'boombap1',
  octave: 0,
  pitch: slots(0),
  playmode: slots(8192),
  reverse: slots(8192),
  start: [
    0, 97647201, 165167950, 211911835, 282029692, 313450641, 372920355, 413171470,
    454136790, 478545547, 492553698, 582032184, 582032184, 582032184, 582032184, 582032184,
    582032184, 582032184, 582032184, 582032184, 582032184, 582032184, 582032184, 642638133,
  ],
  type: 'drum',
  volume: slots(8192),
};
