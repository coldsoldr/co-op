import { readForm, writeForm, type IffForm, type Chunk } from './iff';
import { parseComm, type CommInfo } from './comm';
import { isOp1Chunk, parseOp1Chunk, buildOp1Chunk } from './op1-chunk';
import { metaKind, type Op1Meta, type PatchKind } from './op1-meta';
import { decodeSsnd } from './ssnd';

export interface Patch {
  kind: PatchKind;
  /** The name stored in the op-1 metadata, or '' when there is none. */
  name: string;
  meta: Op1Meta | null;
  comm: CommInfo;
  form: IffForm;
  /**
   * The current bytes of this patch: the bytes it was read from (aliasing the caller's buffer,
   * not copied) or, after withMeta, the re-serialised file. Always equal to writePatch(patch).
   */
  raw: Uint8Array;
  /** Filler bytes that followed the JSON in the original APPL chunk. */
  op1Trailing: Uint8Array | null;
  /**
   * True when re-serialising the metadata reproduces the original APPL chunk byte for byte.
   * False means an edit will canonicalise the chunk. True when there is no APPL chunk.
   */
  op1Canonical: boolean;
}

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

export class PatchError extends Error {
  override name = 'PatchError';
}

export function readPatch(bytes: Uint8Array): Patch {
  let form: IffForm;
  try {
    form = readForm(bytes);
  } catch (error) {
    throw new PatchError(`not an AIFF file: ${(error as Error).message}`);
  }
  if (form.formType !== 'AIFF' && form.formType !== 'AIFC') {
    throw new PatchError(`unsupported FORM type ${form.formType}`);
  }
  const commChunk = form.chunks.find((c) => c.id === 'COMM');
  if (!commChunk) throw new PatchError('missing COMM chunk');
  let comm: CommInfo;
  try {
    comm = parseComm(commChunk, form.formType);
  } catch (error) {
    throw new PatchError(`bad COMM chunk: ${(error as Error).message}`);
  }

  let meta: Op1Meta | null = null;
  let op1Trailing: Uint8Array | null = null;
  let op1Canonical = true;
  const op1Chunks = form.chunks.filter(isOp1Chunk);
  if (op1Chunks.length > 1) throw new PatchError('more than one op-1 APPL chunk');
  const applChunk = op1Chunks[0];
  if (applChunk) {
    try {
      const content = parseOp1Chunk(applChunk);
      meta = content.meta as Op1Meta;
      op1Trailing = content.trailing;
      op1Canonical = bytesEqual(buildOp1Chunk(content.meta, content.trailing).data, applChunk.data);
    } catch (error) {
      throw new PatchError(`bad op-1 metadata: ${(error as Error).message}`);
    }
  }
  const kind = metaKind(meta);
  const name = typeof meta?.name === 'string' ? meta.name : '';
  return { kind, name, meta, comm, form, raw: bytes, op1Trailing, op1Canonical };
}

/**
 * Returns a copy of the patch carrying new metadata. The audio chunks are untouched; `raw` is
 * re-serialised so it stays equal to writePatch(result).
 */
export function withMeta(patch: Patch, meta: Op1Meta): Patch {
  const next: Patch = { ...patch, meta, kind: metaKind(meta), name: meta.name, op1Canonical: true };
  return { ...next, raw: writePatch(next) };
}

/**
 * Serialises the patch, rebuilding only the op-1 APPL chunk. Every other chunk is copied verbatim.
 *
 * Placement is decided from the whole chunk list: an existing op-1 APPL chunk is rebuilt in place
 * (wherever it was, even after SSND); a file that had none gets the chunk inserted just before
 * SSND, or appended when there is no SSND either. When `meta` is null the op-1 APPL chunk is
 * dropped, so `{ ...patch, meta: null }` strips the metadata from a file.
 */
export function writePatch(patch: Patch): Uint8Array {
  const rebuilt = patch.meta ? buildOp1Chunk(patch.meta, patch.op1Trailing ?? undefined) : null;
  const hadOp1 = patch.form.chunks.some(isOp1Chunk);
  const chunks: Chunk[] = [];
  let placed = false;
  for (const chunk of patch.form.chunks) {
    if (isOp1Chunk(chunk)) {
      if (!placed && rebuilt) chunks.push(rebuilt);
      placed = true;
      continue;
    }
    if (rebuilt && !hadOp1 && !placed && chunk.id === 'SSND') {
      chunks.push(rebuilt);
      placed = true;
    }
    chunks.push(chunk);
  }
  if (rebuilt && !placed) chunks.push(rebuilt);
  return writeForm({ formType: patch.form.formType, chunks });
}

/** Decodes the sound data to mono floats for preview. */
export function decodePcm(patch: Patch): Float32Array {
  const ssnd = patch.form.chunks.find((c) => c.id === 'SSND');
  if (!ssnd) throw new PatchError('missing SSND chunk');
  return decodeSsnd(ssnd, patch.comm);
}
