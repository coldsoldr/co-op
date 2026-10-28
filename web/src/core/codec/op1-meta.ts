export type PatchKind = 'synth' | 'drum' | 'unknown';

/** Fields common to every op-1 JSON blob. Unknown fields are kept and written back untouched. */
interface BaseMeta {
  name: string;
  octave: number;
  type: string;
  fx_active: boolean;
  fx_params: number[];
  fx_type: string;
  lfo_active: boolean;
  lfo_params: number[];
  lfo_type: string;
  [extra: string]: unknown;
}

export interface SynthMeta extends BaseMeta {
  synth_version: number;
  adsr: number[];
  knobs: number[];
  /** Present on sampler patches. */
  base_freq?: number;
}

export interface DrumMeta extends BaseMeta {
  type: 'drum';
  drum_version: number;
  dyna_env: number[];
  start: number[];
  end: number[];
  pitch: number[];
  playmode: number[];
  reverse: number[];
  volume: number[];
}

export type Op1Meta = SynthMeta | DrumMeta;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function isSynthMeta(value: unknown): value is SynthMeta {
  return (
    isRecord(value) &&
    typeof value['synth_version'] === 'number' &&
    typeof value['type'] === 'string' &&
    value['type'] !== 'drum' &&
    Array.isArray(value['adsr']) &&
    Array.isArray(value['knobs'])
  );
}

export function isDrumMeta(value: unknown): value is DrumMeta {
  return (
    isRecord(value) &&
    value['type'] === 'drum' &&
    typeof value['drum_version'] === 'number' &&
    Array.isArray(value['start']) &&
    Array.isArray(value['end'])
  );
}

export function metaKind(value: unknown): PatchKind {
  if (isDrumMeta(value)) return 'drum';
  if (isSynthMeta(value)) return 'synth';
  return 'unknown';
}
