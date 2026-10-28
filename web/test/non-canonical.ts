import { readForm, writeForm } from '../src/core/codec/iff';
import { isOp1Chunk, parseOp1Chunk } from '../src/core/codec/op1-chunk';
import { readPatch } from '../src/core/codec/patch';

/**
 * The same patch with its op-1 metadata pretty-printed, so readPatch reports op1Canonical false and
 * writePatch would NOT reproduce these bytes. Tests use it to prove an operation kept raw bytes
 * rather than re-serialising.
 */
export function nonCanonical(bytes: Uint8Array): Uint8Array {
  const form = readForm(bytes);
  const encoder = new TextEncoder();
  const chunks = form.chunks.map((chunk) => {
    if (!isOp1Chunk(chunk)) return chunk;
    const json = encoder.encode(JSON.stringify(parseOp1Chunk(chunk).meta, null, 2));
    const data = new Uint8Array(4 + json.length + 1);
    data.set(encoder.encode('op-1'), 0);
    data.set(json, 4);
    data[4 + json.length] = 0x0a;
    return { id: chunk.id, data };
  });
  const out = writeForm({ formType: form.formType, chunks });
  if (readPatch(out).op1Canonical) throw new Error('nonCanonical: result is still canonical');
  return out;
}
