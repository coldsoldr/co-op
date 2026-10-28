import { describe, it, expect } from 'vitest';
import { readForm, writeForm, IffError } from './iff';

function ascii(s: string): Uint8Array {
  return new TextEncoder().encode(s);
}
function u32(n: number): Uint8Array {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, n);
  return b;
}
function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

const body = concat(
  ascii('AIFF'),
  ascii('ABCD'), u32(3), ascii('xyz'), new Uint8Array([7]),
  ascii('EFGH'), u32(2), ascii('ok'),
);
const file = concat(ascii('FORM'), u32(body.length), body);

describe('readForm', () => {
  it('parses chunks and keeps the pad byte of odd-sized chunks', () => {
    const form = readForm(file);
    expect(form.formType).toBe('AIFF');
    expect(form.chunks.map((c) => c.id)).toEqual(['ABCD', 'EFGH']);
    expect(Array.from(form.chunks[0]!.data)).toEqual(Array.from(ascii('xyz')));
    expect(Array.from(form.chunks[0]!.pad!)).toEqual([7]);
    expect(form.chunks[1]!.pad).toBeUndefined();
  });

  it('rejects files without a FORM header', () => {
    expect(() => readForm(ascii('RIFF....WAVE'))).toThrow(IffError);
  });

  it('rejects a chunk that runs past the end of the file', () => {
    const bad = concat(ascii('FORM'), u32(16), ascii('AIFF'), ascii('ABCD'), u32(100), ascii('xy'));
    expect(() => readForm(bad)).toThrow(/runs past end/);
  });
});

describe('writeForm', () => {
  it('round-trips byte for byte', () => {
    expect(Array.from(writeForm(readForm(file)))).toEqual(Array.from(file));
  });

  it('writes the FORM size as total length minus 8 and pads odd chunks', () => {
    const out = writeForm({ formType: 'AIFF', chunks: [{ id: 'ABCD', data: ascii('x') }] });
    expect(out.length).toBe(12 + 8 + 2);
    expect(new DataView(out.buffer).getUint32(4)).toBe(out.length - 8);
    expect(out[out.length - 1]).toBe(0);
  });
});
