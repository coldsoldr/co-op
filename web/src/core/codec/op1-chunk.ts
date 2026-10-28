import type { Chunk } from './iff';

export const OP1_SIGNATURE = 'op-1';

const encoder = new TextEncoder();
const latin1 = new TextDecoder('latin1');
const utf8 = new TextDecoder('utf-8');

export interface Op1ChunkContent {
  meta: Record<string, unknown>;
  /** Bytes after the JSON's closing brace: the OP-1 filler (0x0A then 0x1E bytes). */
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
  if (!isOp1Chunk(chunk)) throw new Error('not an op-1 APPL chunk');
  const body = chunk.data.subarray(4);
  const end = body.lastIndexOf(0x7d); // '}'
  if (end < 0) throw new Error('op-1 chunk has no JSON object');
  const meta: unknown = JSON.parse(utf8.decode(body.subarray(0, end + 1)));
  if (typeof meta !== 'object' || meta === null || Array.isArray(meta)) {
    throw new Error('op-1 chunk JSON is not an object');
  }
  return { meta: meta as Record<string, unknown>, trailing: body.slice(end + 1) };
}

function sortKeys(meta: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(Object.keys(meta).sort().map((k) => [k, meta[k]]));
}

/**
 * Serialises metadata the way the OP-1 does: compact JSON with alphabetical keys,
 * a newline, then 0x1E bytes until the chunk length is even.
 */
export function buildOp1Chunk(meta: Record<string, unknown>, trailing?: Uint8Array): Chunk {
  const json = encoder.encode(JSON.stringify(sortKeys(meta)));
  const filler = trailing && trailing.length > 0 ? Array.from(trailing) : [0x0a];
  if ((4 + json.length + filler.length) % 2 === 1) filler.push(0x1e);
  const data = new Uint8Array(4 + json.length + filler.length);
  data.set(encoder.encode(OP1_SIGNATURE), 0);
  data.set(json, 4);
  data.set(filler, 4 + json.length);
  return { id: 'APPL', data };
}
