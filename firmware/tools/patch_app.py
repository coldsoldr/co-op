#!/usr/bin/env python3
"""Patch bytes in OP1_vdk.ldr at runtime (load) addresses.

Each patch gives the memory address, the bytes expected there in the stock image,
and the replacement bytes (same length). Patches must land entirely inside one plain
data block: encrypted CALLBACK blocks, FILL and IGNORE blocks are refused. The LDR is
re-serialized with ldr_dump.serialize(), so headers and checksums stay valid.

Usage:
    python tools/patch_app.py IN.ldr OUT.ldr ADDR:OLDHEX:NEWHEX [...]

Example (play mode label 'poly' -> 'cow'):
    python tools/patch_app.py OP1_vdk.ldr OP1_vdk.ldr 0x18f2f8c:706f6c7900000000:636f770000000000
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import LdrError, parse, serialize  # noqa: E402

REFUSED_FLAGS = 0x0400 | 0x0100 | 0x1000 | 0x4000  # CALLBACK, FILL, IGNORE, FIRST


def apply_patch(dxes, addr, old, new):
    if len(old) != len(new):
        raise LdrError('{:#x}: old and new must be the same length'.format(addr))
    for dxe in dxes:
        for block in dxe:
            start = block['target']
            if not (start <= addr and addr + len(old) <= start + block['count']):
                continue
            if block['flags'] & REFUSED_FLAGS:
                raise LdrError('{:#x} is inside a CALLBACK/FILL/IGNORE block; refusing'.format(addr))
            pos = addr - start
            current = block['payload'][pos:pos + len(old)]
            if current != old:
                raise LdrError('{:#x}: expected {} but found {}'.format(addr, old.hex(), current.hex()))
            payload = bytearray(block['payload'])
            payload[pos:pos + len(new)] = new
            block['payload'] = bytes(payload)
            return block['offset']
    raise LdrError('{:#x}: no single plain block covers this range'.format(addr))


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        data = f.read()
    try:
        dxes = parse(data)
        for spec in sys.argv[3:]:
            addr, old, new = spec.split(':')
            addr, old, new = int(addr, 16), bytes.fromhex(old), bytes.fromhex(new)
            offset = apply_patch(dxes, addr, old, new)
            print('patched {:#x}: {} -> {} (block at file offset {:#x})'.format(addr, old.hex(), new.hex(), offset))
        out = serialize(dxes)
        parse(out)
    except (LdrError, ValueError) as e:
        print('ERROR: {}'.format(e))
        sys.exit(1)

    changed = [i for i, (a, b) in enumerate(zip(data, out)) if a != b] if len(out) == len(data) else None
    with open(dst, 'wb') as f:
        f.write(out)
    if changed is None:
        print('wrote {} ({} bytes, size changed from {})'.format(dst, len(out), len(data)))
    else:
        print('wrote {} ({} bytes, {} byte(s) differ from input)'.format(dst, len(out), len(changed)))


if __name__ == '__main__':
    main()
