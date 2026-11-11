#!/usr/bin/env python3
"""Make the delta co-op's Build firmware tab turns a stock op1_246.op1 into a coop package with.

The site never holds Teenage Engineering bytes or the L1 key. It holds this delta: the coop
OP1_vdk.ldr written as copies of ranges of the stock OP1_vdk.ldr plus literal bytes that are
coop's own (its code, its data, the boot picture, and the L1 blocks coop changed, already
encrypted here). The browser reads the owner's stock package, checks its OP1_vdk.ldr is R246's,
applies the delta, checks the result is exactly the tested coop OP1_vdk.ldr, and packs it with
the stock content and no te-boot.ldr.

No literal run holds 16 or more bytes in a row that occur anywhere in the stock OP1_vdk.ldr: any
such run is a copy (the matcher tries every offset), so the delta carries no stock code.

Usage:
    python3 -I tools/make_web_delta.py STOCK.op1 COOP.op1 OUT.delta

STOCK.op1 is TE's op1_246.op1; COOP.op1 a package from tools/build_device_package.sh that has
passed its gates. Prints the facts web/src/core/firmware/release.ts needs.

Format (little endian): b"COOPDLT1", stock LDR sha256 (32), coop LDR sha256 (32), coop LDR
length (u32), op count (u32), then ops: 0 = copy (u32 stock offset, u32 length), 1 = literal
(u32 length, bytes).
"""

import binascii
import hashlib
import io
import json
import lzma
import struct
import sys
import tarfile

MAGIC = b'COOPDLT1'
STOCK_LDR_SHA256 = 'f79d299f6f9019c998013a4176223a6a1bc999a2d8c54451346861f6ea455c74'
K = 16  # shortest copy; every stock run this long in the target is copied, never carried


def ldr_of(path):
    data = open(path, 'rb').read()
    if struct.unpack('<L', data[:4])[0] != binascii.crc32(data[4:]) & 0xFFFFFFFF:
        sys.exit('{}: outer CRC-32 mismatch'.format(path))
    raw = lzma.decompress(data[4:], format=lzma.FORMAT_ALONE)
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        names = tar.getnames()
        if 'OP1_vdk.ldr' not in names:
            sys.exit('{}: no OP1_vdk.ldr'.format(path))
        return tar.extractfile('OP1_vdk.ldr').read(), names


def stock_index(src):
    index = {}
    for i in range(len(src) - K + 1):
        index.setdefault(src[i:i + K], i)
    return index


def make_ops(src, dst, index):
    ops, lit, i = [], bytearray(), 0
    while i < len(dst):
        j = index.get(dst[i:i + K])
        if j is None:
            lit.append(dst[i])
            i += 1
            continue
        n = K
        while i + n < len(dst) and j + n < len(src) and dst[i + n] == src[j + n]:
            n += 1
        if lit:
            ops.append((1, bytes(lit)))
            lit = bytearray()
        ops.append((0, j, n))
        i += n
    if lit:
        ops.append((1, bytes(lit)))
    return ops


def apply_ops(src, ops):
    out = bytearray()
    for op in ops:
        out += src[op[1]:op[1] + op[2]] if op[0] == 0 else op[1]
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stock_path, coop_path, out_path = sys.argv[1:]
    src, _ = ldr_of(stock_path)
    dst, names = ldr_of(coop_path)
    if hashlib.sha256(src).hexdigest() != STOCK_LDR_SHA256:
        sys.exit('{}: OP1_vdk.ldr is not R246 stock'.format(stock_path))
    if 'te-boot.ldr' in names:
        sys.exit('{}: has te-boot.ldr; coop packages never do'.format(coop_path))

    index = stock_index(src)
    ops = make_ops(src, dst, index)
    if apply_ops(src, ops) != dst:
        sys.exit('the delta does not rebuild the coop OP1_vdk.ldr')
    for op in ops:
        if op[0] == 1:
            for k in range(len(op[1]) - K + 1):
                if op[1][k:k + K] in index:
                    sys.exit('a literal holds stock bytes; not writing the delta')

    body = bytearray(MAGIC)
    body += hashlib.sha256(src).digest() + hashlib.sha256(dst).digest()
    body += struct.pack('<II', len(dst), len(ops))
    for op in ops:
        body += struct.pack('<BII', 0, op[1], op[2]) if op[0] == 0 else struct.pack('<BI', 1, len(op[1])) + op[1]
    with open(out_path, 'wb') as f:
        f.write(body)

    literal = sum(len(op[1]) for op in ops if op[0] == 1)
    print(json.dumps({
        'stockLdrSha256': STOCK_LDR_SHA256,
        'coopLdrSha256': hashlib.sha256(dst).hexdigest(),
        'coopLdrBytes': len(dst),
        'deltaBytes': len(body),
        'deltaSha256': hashlib.sha256(bytes(body)).hexdigest(),
        'ops': len(ops),
        'literalBytes': literal,
        'referencePackageSha256': hashlib.sha256(open(coop_path, 'rb').read()).hexdigest(),
    }, indent=2))


if __name__ == '__main__':
    main()
