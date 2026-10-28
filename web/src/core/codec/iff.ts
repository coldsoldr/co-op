const encoder = new TextEncoder();
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

export class IffError extends Error {}

function readId(bytes: Uint8Array, offset: number): string {
  return latin1.decode(bytes.subarray(offset, offset + 4));
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
    if (size % 2 === 1 && offset < bytes.length) {
      chunk.pad = bytes.slice(offset, offset + 1);
      offset += 1;
    }
    chunks.push(chunk);
  }
  return { formType, chunks };
}

export function writeForm(form: IffForm): Uint8Array {
  let total = 12;
  for (const c of form.chunks) total += 8 + c.data.length + (c.data.length % 2);
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  out.set(encoder.encode('FORM'), 0);
  view.setUint32(4, total - 8);
  out.set(encoder.encode(form.formType), 8);
  let offset = 12;
  for (const c of form.chunks) {
    out.set(encoder.encode(c.id), offset);
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
