import { decodePcm, type Patch } from './patch';
import { isSynthMeta, type SampleKitMeta } from './op1-meta';
import { sha256Hex } from '../util';

/**
 * The fingerprint of the spoken clip the OP-1 stores in a synth patch that has no render of its
 * own (content/audio/speech/op1patch.raw). Only the hash and length are kept here, not the audio.
 */
export const VOICE_CLIP = {
  sha256: '9488ac94a764df8cfaaa703ccc3bcddbf938089c99307374e0d3012c964c5cc7',
  bytes: 57_792,
};

/** Bytes before the sound data in an SSND chunk: its offset and block size. */
const SSND_HEADER = 8;

/** False when a patch's audio tells nothing about its sound: empty, silent or the voice clip. */
export async function hasPreview(patch: Patch, clip = VOICE_CLIP): Promise<boolean> {
  const ssnd = patch.form.chunks.find((c) => c.id === 'SSND');
  if (!ssnd) return false;
  const sound = ssnd.data.subarray(SSND_HEADER);
  if (sound.length === clip.bytes && (await sha256Hex(sound.slice())) === clip.sha256) return false;
  return decodePcm(patch).some((sample) => sample !== 0);
}

/** A kit's start and end words count frames in these units, in a mono kit. */
const KIT_UNITS_PER_FRAME = 4058;
/** The OP-1 has 24 keys, MIDI notes 53 to 76 at octave 0. */
export const KEY_COUNT = 24;
const LOWEST_NOTE = 53;
const MIDDLE_C_HZ = 261.626;

export interface KeyPlan {
  /** Where each key starts in the audio, and where it stops; null plays to the end. */
  startFrame(key: number): number;
  endFrame(key: number): number | null;
  /** Speed relative to the recording. */
  rate(key: number): number;
}

const noteHz = (key: number): number => 440 * 2 ** ((LOWEST_NOTE + key - 69) / 12);

/**
 * How the keys play a patch's audio. A kit's keys are its slices; anything else is repitched from
 * its root, which is the sampler's stored pitch, or middle C for the note the OP-1 renders.
 */
export function keyPlan(patch: Patch): KeyPlan {
  const meta = patch.meta;
  if (meta?.type === 'drum') {
    const kit = meta as SampleKitMeta;
    const frame = (words: number[], key: number): number => Math.floor((words[key] ?? 0) / KIT_UNITS_PER_FRAME);
    return {
      startFrame: (key) => frame(kit.start, key),
      endFrame: (key) => frame(kit.end, key),
      rate: () => 1,
    };
  }
  const root = isSynthMeta(meta) && meta.base_freq && meta.base_freq > 0 ? meta.base_freq : MIDDLE_C_HZ;
  return { startFrame: () => 0, endFrame: () => null, rate: (key) => noteHz(key) / root };
}
