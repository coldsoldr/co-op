/**
 * The coop delta (firmware/tools/make_web_delta.py): the coop OP1_vdk.ldr as copies of ranges of the stock
 * OP1_vdk.ldr and literal bytes of coop's own. Little endian: b"COOPDLT1", stock LDR sha256 (32), coop LDR
 * sha256 (32), coop LDR length (u32), op count (u32); then each op, 0 = copy (u32 offset, u32 length) or
 * 1 = literal (u32 length, bytes).
 */
import { PackageError } from './package';

const MAGIC = 'COOPDLT1';

export interface Delta {
  stockSha256: string;
  coopSha256: string;
  apply: (stock: Uint8Array) => Uint8Array;
}

const hex = (bytes: Uint8Array): string => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

export function readDelta(bytes: Uint8Array): Delta {
  const fail = (): never => {
    throw new PackageError('The coop delta is damaged; reload the page and try again.');
  };
  if (bytes.length < 80 || String.fromCharCode(...bytes.subarray(0, 8)) !== MAGIC) fail();
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint32(72, true);
  const count = view.getUint32(76, true);
  type Op = { copy: true; from: number; length: number } | { copy: false; bytes: Uint8Array };
  const ops: Op[] = [];
  let at = 80;
  for (let i = 0; i < count; i++) {
    if (at + 5 > bytes.length) fail();
    const kind = bytes[at];
    if (kind === 0) {
      if (at + 9 > bytes.length) fail();
      ops.push({ copy: true, from: view.getUint32(at + 1, true), length: view.getUint32(at + 5, true) });
      at += 9;
    } else if (kind === 1) {
      const n = view.getUint32(at + 1, true);
      if (at + 5 + n > bytes.length) fail();
      ops.push({ copy: false, bytes: bytes.subarray(at + 5, at + 5 + n) });
      at += 5 + n;
    } else {
      fail();
    }
  }
  if (at !== bytes.length) fail();
  return {
    stockSha256: hex(bytes.subarray(8, 40)),
    coopSha256: hex(bytes.subarray(40, 72)),
    apply(stock) {
      const out = new Uint8Array(length);
      let to = 0;
      for (const op of ops) {
        const piece = op.copy ? stock.subarray(op.from, op.from + op.length) : op.bytes;
        if ((op.copy && op.from + op.length > stock.length) || to + piece.length > length) fail();
        out.set(piece, to);
        to += piece.length;
      }
      if (to !== length) fail();
      return out;
    },
  };
}
