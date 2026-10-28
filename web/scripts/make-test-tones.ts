/**
 * Generates the patches of the test-tones starter pack (packs/test-tones). Run once from web/
 * with `tsx scripts/make-test-tones.ts`; the output is committed, and a test checks that it still
 * matches what this script produces.
 *
 * Everything here is synthetic: the audio is computed sine waves and the drum slice points are
 * evenly spaced. The synth and effect parameter values are the defaults used by the teoperator
 * project (MIT licence), the same values as the codec's test fixtures.
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { buildComm } from '../src/core/codec/comm';
import { writeForm, type Chunk } from '../src/core/codec/iff';
import { buildOp1Chunk } from '../src/core/codec/op1-chunk';
import type { DrumMeta, SynthMeta } from '../src/core/codec/op1-meta';
import { buildSsnd } from '../src/core/codec/ssnd';

const SAMPLE_RATE = 44100;
/** Drum slices on the OP-1. */
const SLOTS = 24;
/**
 * Drum start/end positions are stored in units of 1/4058 of a frame at 44.1 kHz, so the 12-second
 * drum sampler spans about the whole 31-bit range.
 */
const POSITION_UNITS_PER_FRAME = 4058;

function sine(frames: number, hz: number, amplitude = 12000): Int16Array {
  const out = new Int16Array(frames);
  for (let i = 0; i < frames; i++) out[i] = Math.round(Math.sin((2 * Math.PI * hz * i) / SAMPLE_RATE) * amplitude);
  return out;
}

/** A short fade at both ends so slices do not click. */
function fade(samples: Int16Array, frames = 220): Int16Array {
  for (let i = 0; i < frames && i < samples.length; i++) {
    const gain = i / frames;
    samples[i] = Math.round((samples[i] as number) * gain);
    const j = samples.length - 1 - i;
    samples[j] = Math.round((samples[j] as number) * gain);
  }
  return samples;
}

const slots = (value: number): number[] => Array.from({ length: SLOTS }, () => value);

const CLUSTER: SynthMeta = {
  adsr: [64, 64, 0, 64, 14336, 64, 4000, 4000],
  fx_active: true,
  fx_params: [64, -14337, 4515, 7232, 0, 0, 0, 0],
  fx_type: 'nitro',
  knobs: [3072, 0, 512, 3, 0, 0, 0, 0],
  lfo_active: false,
  lfo_params: [4608, 32767, 8448, 15360, 0, 0, 0, 0],
  lfo_type: 'value',
  name: 'tone-cluster',
  octave: 0,
  synth_version: 2,
  type: 'cluster',
};

const SAMPLER: SynthMeta = {
  adsr: [512, 10746, 32767, 10000, 4000, 64, 4000, 4000],
  base_freq: 261.6253662109375,
  fx_active: false,
  fx_params: [4480, 15544, 10788, 12104, 0, 0, 0, 0],
  fx_type: 'delay',
  knobs: [0, 0, 32767, 32767, 12000, 0, 0, 8304],
  lfo_active: false,
  lfo_params: [11840, 18431, 1024, 15144, 0, 0, 0, 0],
  lfo_type: 'random',
  name: 'tone-sampler',
  octave: 0,
  synth_version: 1,
  type: 'sampler',
};

/** Frames per drum slice: 0.1 s. */
const SLICE_FRAMES = SAMPLE_RATE / 10;

function drumMeta(): DrumMeta {
  const start = Array.from({ length: SLOTS }, (_, i) => i * SLICE_FRAMES * POSITION_UNITS_PER_FRAME);
  const end = Array.from({ length: SLOTS }, (_, i) => ((i + 1) * SLICE_FRAMES - 1) * POSITION_UNITS_PER_FRAME);
  return {
    drum_version: 2,
    dyna_env: [0, 8192, 0, 8192, 0, 0, 0, 0],
    end,
    fx_active: false,
    fx_params: [8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000],
    fx_type: 'delay',
    lfo_active: false,
    lfo_params: [16000, 16000, 16000, 16000, 0, 0, 0, 0],
    lfo_type: 'tremolo',
    name: 'tone-drum',
    octave: 0,
    pitch: slots(0),
    playmode: slots(8192),
    reverse: slots(8192),
    start,
    type: 'drum',
    volume: slots(8192),
  };
}

/** One slice per slot, each a sine a semitone above the last, starting at A2 (110 Hz). */
function drumAudio(): Int16Array {
  const out = new Int16Array(SLOTS * SLICE_FRAMES);
  for (let i = 0; i < SLOTS; i++) out.set(fade(sine(SLICE_FRAMES, 110 * 2 ** (i / 12))), i * SLICE_FRAMES);
  return out;
}

function patch(meta: Record<string, unknown>, samples: Int16Array, formType: 'AIFF' | 'AIFC'): Uint8Array {
  const little = formType === 'AIFC';
  const chunks: Chunk[] = [];
  if (little) {
    const fver = new Uint8Array(4);
    new DataView(fver.buffer).setUint32(0, 0xa2805140); // AIFC version 1
    chunks.push({ id: 'FVER', data: fver });
  }
  chunks.push(
    buildComm(
      {
        channels: 1,
        frames: samples.length,
        bitsPerSample: 16,
        sampleRate: SAMPLE_RATE,
        compression: little ? 'sowt' : 'NONE',
        compressionName: little ? 'not compressed' : '',
      },
      formType,
    ),
  );
  chunks.push(buildOp1Chunk(meta));
  chunks.push(buildSsnd(samples, little));
  return writeForm({ formType, chunks });
}

/** The pack's patch files, by path inside the pack folder. Pure and deterministic. */
export function makeTestTones(): Record<string, Uint8Array> {
  return {
    'synth/tone-cluster.aif': patch(CLUSTER, fade(sine(SAMPLE_RATE / 2, 220)), 'AIFF'),
    // Little-endian AIFC, as the OP-1 itself writes sampler patches.
    'synth/tone-sampler.aif': patch(SAMPLER, fade(sine(SAMPLE_RATE / 2, 261.63)), 'AIFC'),
    'drum/tone-drum.aif': patch(drumMeta() as unknown as Record<string, unknown>, drumAudio(), 'AIFF'),
  };
}

export const TEST_TONES_DIR = fileURLToPath(new URL('../../packs/test-tones', import.meta.url));

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  for (const [path, bytes] of Object.entries(makeTestTones())) {
    const full = join(TEST_TONES_DIR, path);
    mkdirSync(dirname(full), { recursive: true });
    writeFileSync(full, bytes);
    console.log(`wrote ${full} (${bytes.length} bytes)`);
  }
}
