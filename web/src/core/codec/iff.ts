const latin1 = new TextDecoder('latin1');

export interface Chunk {
  /** Four ASCII characters. */
  id: string;
  /** Exactly `size` bytes; the pad byte is not included. */
  data: Uint8Array;
  /** The pad byte that followed an odd-sized chunk, kept so writes are byte-identical. */
  pad?: Uint8Array;
}

export interface IffForm {
  /** "AIFF" or "AIFC" for the files we care about. */
  formType: string;
  chunks: Chunk[];
}

export class IffError extends Error {
  override name = 'IffError';
}

function readId(bytes: Uint8Array, offset: number): string {
  return latin1.decode(bytes.subarray(offset, offset + 4));
}

/** Writes a four-character latin1 id, the inverse of the latin1 decode used when reading. */
export function writeId(out: Uint8Array, offset: number, id: string): void {
  if (id.length !== 4) throw new IffError(`id "${id}" must be exactly 4 characters`);
  for (let i = 0; i < 4; i++) {
    const code = id.charCodeAt(i);
    if (code > 0xff) throw new IffError(`id "${id}" contains characters outside latin1`);
    out[offset + i] = code;
  }
}

export function readForm(bytes: Uint8Array): IffForm {
  if (bytes.length < 12 || readId(bytes, 0) !== 'FORM') {
    throw new IffError('not an IFF FORM file');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const formType = readId(bytes, 8);
  const chunks: Chunk[] = [];
  let offset = 12;
  while (offset + 8 <= bytes.length) {
    const id = readId(bytes, offset);
    const size = view.getUint32(offset + 4);
    const start = offset + 8;
    const end = start + size;
    if (end > bytes.length) throw new IffError(`chunk ${id} runs past end of file`);
    const chunk: Chunk = { id, data: bytes.slice(start, end) };
    offset = end;
    if (size % 2 === 1) {
      if (offset >= bytes.length) {
        throw new IffError(`chunk ${id} is odd-sized but has no pad byte`);
      }
      chunk.pad = bytes.slice(offset, offset + 1);
      offset += 1;
    }
    chunks.push(chunk);
  }
  if (offset !== bytes.length) throw new IffError('trailing bytes after last chunk');
  return { formType, chunks };
}

export function writeForm(form: IffForm): Uint8Array {
  let total = 12;
  for (const c of form.chunks) total += 8 + c.data.length + (c.data.length % 2);
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  writeId(out, 0, 'FORM');
  view.setUint32(4, total - 8);
  writeId(out, 8, form.formType);
  let offset = 12;
  for (const c of form.chunks) {
    writeId(out, offset, c.id);
    view.setUint32(offset + 4, c.data.length);
    out.set(c.data, offset + 8);
    offset += 8 + c.data.length;
    if (c.data.length % 2 === 1) {
      out[offset] = c.pad?.[0] ?? 0;
      offset += 1;
    }
  }
  return out;
}
