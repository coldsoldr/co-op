import type { Chunk } from './iff';
import type { CommInfo } from './comm';

export class SsndError extends Error {}

/** Builds an SSND chunk from interleaved 16-bit samples with a zero offset and block size. */
export function buildSsnd(samples: Int16Array, littleEndian: boolean): Chunk {
  const data = new Uint8Array(8 + samples.length * 2);
  const view = new DataView(data.buffer);
  for (let i = 0; i < samples.length; i++) {
    view.setInt16(8 + i * 2, samples[i]!, littleEndian);
  }
  return { id: 'SSND', data };
}

/** Decodes 16-bit PCM to mono floats in [-1, 1), averaging channels. */
export function decodeSsnd(chunk: Chunk, comm: CommInfo): Float32Array {
  if (comm.bitsPerSample !== 16) {
    throw new SsndError(`unsupported bit depth ${comm.bitsPerSample}`);
  }
  const compression = comm.compression;
  if (compression !== 'NONE' && compression !== 'twos' && compression !== 'sowt') {
    throw new SsndError(`unsupported compression ${compression}`);
  }
  const littleEndian = compression === 'sowt';
  const view = new DataView(chunk.data.buffer, chunk.data.byteOffset, chunk.data.byteLength);
  const start = 8 + view.getUint32(0);
  const channels = Math.max(1, comm.channels);
  const frames = Math.floor((chunk.data.length - start) / (2 * channels));
  const out = new Float32Array(frames);
  for (let frame = 0; frame < frames; frame++) {
    let sum = 0;
    for (let ch = 0; ch < channels; ch++) {
      sum += view.getInt16(start + (frame * channels + ch) * 2, littleEndian);
    }
    out[frame] = sum / channels / 32768;
  }
  return out;
}
