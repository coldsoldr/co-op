import type { Chunk } from './iff';

export const OP1_SIGNATURE = 'op-1';

const encoder = new TextEncoder();
const latin1 = new TextDecoder('latin1');
const utf8 = new TextDecoder('utf-8', { fatal: true });

/** The byte the device writes after the newline when the chunk needs padding to an even length. */
const DEFAULT_PAD_BYTE = 0x20;

export class Op1ChunkError extends Error {
  override name = 'Op1ChunkError';
}

export interface Op1ChunkContent {
  meta: Record<string, unknown>;
  /** Bytes after the JSON's closing brace: the OP-1 filler (0x0A, then one 0x20 pad byte if needed). */
  trailing: Uint8Array;
}

export function isOp1Chunk(chunk: Chunk): boolean {
  return (
    chunk.id === 'APPL' &&
    chunk.data.length >= 4 &&
    latin1.decode(chunk.data.subarray(0, 4)) === OP1_SIGNATURE
  );
}

export function parseOp1Chunk(chunk: Chunk): Op1ChunkContent {
  if (!isOp1Chunk(chunk)) throw new Op1ChunkError('not an op-1 APPL chunk');
  const body = chunk.data.subarray(4);
  const end = body.lastIndexOf(0x7d); // '}'
  if (end < 0) throw new Op1ChunkError('op-1 chunk has no JSON object');
  let meta: unknown;
  try {
    meta = JSON.parse(utf8.decode(body.subarray(0, end + 1)));
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new Op1ChunkError(`op-1 chunk JSON is invalid: ${message}`);
  }
  if (typeof meta !== 'object' || meta === null || Array.isArray(meta)) {
    throw new Op1ChunkError('op-1 chunk JSON is not an object');
  }
  return { meta: meta as Record<string, unknown>, trailing: body.slice(end + 1) };
}

function sortKeys(meta: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(Object.keys(meta).sort().map((k) => [k, meta[k]]));
}

/**
 * Serialises metadata the way the OP-1 does: compact JSON with alphabetical keys,
 * a newline, then one pad byte (0x20 on the device) if the chunk length would be odd.
 * The filler is rebuilt on every call; `trailing` only supplies the pad byte seen in the
 * source file, so repeated edits never accumulate padding.
 */
export function buildOp1Chunk(meta: Record<string, unknown>, trailing?: Uint8Array): Chunk {
  const json = encoder.encode(JSON.stringify(sortKeys(meta)));
  const padByte = trailing?.[1] ?? DEFAULT_PAD_BYTE;
  const filler = [0x0a];
  if ((4 + json.length + filler.length) % 2 === 1) filler.push(padByte);
  const data = new Uint8Array(4 + json.length + filler.length);
  data.set(encoder.encode(OP1_SIGNATURE), 0);
  data.set(json, 4);
  data.set(filler, 4 + json.length);
  return { id: 'APPL', data };
}
