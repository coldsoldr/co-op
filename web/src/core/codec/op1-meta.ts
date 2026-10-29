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

/**
 * Drum engine names in `type`: the stock sample kit ('drum'), the stock DBox drum synth ('dbox')
 * and hundekraft's Gin kit ('gin', which its firmware treats as 'dbox').
 */
export const DRUM_TYPES = ['drum', 'dbox', 'gin'] as const;
export type DrumType = (typeof DRUM_TYPES)[number];

/** A stock sample kit: every per-slot array is present. */
export interface SampleKitMeta extends BaseMeta {
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

/** A DBox or Gin drum synth kit. Only `name` and `type` are checked; the rest is as saved. */
export interface DrumSynthMeta extends BaseMeta {
  type: 'dbox' | 'gin';
  /** Gin kits: 24 slots of 8 numbers. */
  dbox_data?: number[][];
  /** Gin kits: the kit's own state as text. */
  gin_state?: string;
}

export type DrumMeta = SampleKitMeta | DrumSynthMeta;

export type Op1Meta = SynthMeta | DrumMeta;

function isDrumType(type: unknown): type is DrumType {
  return (DRUM_TYPES as readonly unknown[]).includes(type);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function isSynthMeta(value: unknown): value is SynthMeta {
  return (
    isRecord(value) &&
    typeof value['name'] === 'string' &&
    typeof value['synth_version'] === 'number' &&
    typeof value['type'] === 'string' &&
    !isDrumType(value['type']) &&
    Array.isArray(value['adsr']) &&
    Array.isArray(value['knobs'])
  );
}

/**
 * A sample kit needs drum_version, start and end. DBox and Gin kits need only a string name: no
 * real DBox file has been seen yet, so their field set is not pinned down.
 */
export function isDrumMeta(value: unknown): value is DrumMeta {
  if (!isRecord(value) || typeof value['name'] !== 'string' || !isDrumType(value['type'])) return false;
  if (value['type'] !== 'drum') return true;
  return typeof value['drum_version'] === 'number' && Array.isArray(value['start']) && Array.isArray(value['end']);
}

export function metaKind(value: unknown): PatchKind {
  if (isDrumMeta(value)) return 'drum';
  if (isSynthMeta(value)) return 'synth';
  return 'unknown';
}
