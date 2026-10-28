import { writeFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { writeForm, type Chunk } from '../../src/core/codec/iff';
import { buildComm } from '../../src/core/codec/comm';
import { buildOp1Chunk } from '../../src/core/codec/op1-chunk';
import { buildSsnd } from '../../src/core/codec/ssnd';
import { SYNTH_META, SAMPLER_META, DRUM_META } from './meta';

const SAMPLE_RATE = 44100;

function sine(seconds: number, hz: number): Int16Array {
  const frames = Math.round(seconds * SAMPLE_RATE);
  const out = new Int16Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = Math.round(Math.sin((2 * Math.PI * hz * i) / SAMPLE_RATE) * 12000);
  }
  return out;
}

function fverChunk(): Chunk {
  const data = new Uint8Array(4);
  new DataView(data.buffer).setUint32(0, 0xa2805140); // AIFC version 1 timestamp
  return { id: 'FVER', data };
}

interface Spec {
  file: string;
  formType: 'AIFF' | 'AIFC';
  meta: Record<string, unknown> | null;
  seconds: number;
  hz: number;
}

const specs: Spec[] = [
  { file: 'synth-cluster.aif', formType: 'AIFF', meta: SYNTH_META, seconds: 0.25, hz: 220 },
  { file: 'synth-sampler-sowt.aif', formType: 'AIFC', meta: SAMPLER_META, seconds: 0.25, hz: 261.63 },
  { file: 'drum-boombap.aif', formType: 'AIFF', meta: DRUM_META, seconds: 0.5, hz: 110 },
  { file: 'no-metadata.aif', formType: 'AIFF', meta: null, seconds: 0.1, hz: 440 },
];

function buildFixture(spec: Spec): Uint8Array {
  const samples = sine(spec.seconds, spec.hz);
  const little = spec.formType === 'AIFC';
  const chunks: Chunk[] = [];
  if (spec.formType === 'AIFC') chunks.push(fverChunk());
  chunks.push(
    buildComm(
      {
        channels: 1,
        frames: samples.length,
        bitsPerSample: 16,
        sampleRate: SAMPLE_RATE,
        compression: little ? 'sowt' : 'NONE',
        // Deliberately not the device's own string: its odd length exercises the Pascal-string pad byte.
        compressionName: little ? 'not compressed' : '',
      },
      spec.formType,
    ),
  );
  if (spec.meta) chunks.push(buildOp1Chunk(spec.meta));
  chunks.push(buildSsnd(samples, little));
  return writeForm({ formType: spec.formType, chunks });
}

/** Pure: returns every generated fixture keyed by file name, without touching the disk. */
export function buildFixtures(): Map<string, Uint8Array> {
  return new Map(specs.map((spec) => [spec.file, buildFixture(spec)]));
}

// CLI: `pnpm --filter @co-op/web fixtures` writes the files next to this script. Importing the
// module (as fixtures.test.ts does) must not write anything, hence the entry-point guard.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  for (const [file, bytes] of buildFixtures()) {
    writeFileSync(fileURLToPath(new URL(file, import.meta.url)), bytes);
    console.log(`${file}: ${bytes.length} bytes`);
  }
}
