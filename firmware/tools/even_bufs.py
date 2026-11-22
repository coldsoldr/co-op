#!/usr/bin/env python3
"""Fail the link when a data object of coop's image starts at an odd address.

    python3 -I tools/even_bufs.py build/core/coop_combined.elf      # exit 1 when an object is odd

TE's NAND driver asserts `te_yflash2.c:600 !((unsigned int)data & 1)`: a buffer at an odd address handed to a yaffs
read or write halts the device, at every boot when the read is a boot read (op1-sdk, docs/lessons.md 2026-11-15).
A uint8_t / char array has alignment 1 on the Blackfin and -fdata-sections gives it a section of its own, so whether
it lands odd changes with unrelated edits (docs/brick-safety.md "Even buffers for the stock file layer").

The rule checked is wider than the file buffers, so a new buffer cannot slip past: every STT_OBJECT symbol of two
bytes or more, defined in a section, starts even. One-byte objects are left out. The fix for a hit is
`__attribute__((aligned(4)))` on the object. Stack buffers, string literals and pointer offsets are not symbols:
coop_y_read / coop_y_write (core/include/coop_yio.h) bounce those at run time.
"""
import struct
import sys


def objects(data):
    """(name, address, size) of every defined STT_OBJECT symbol of an ELF32 little-endian file."""
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise ValueError('not an ELF32 little-endian file')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2E)
    secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for s in secs:
        if s[1] != 2:                                   # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value, size, info, _other, shndx = struct.unpack_from('<IIIBBH', data, off)
            if info & 0xF != 1 or shndx == 0 or shndx >= 0xFF00:   # STT_OBJECT, defined, not ABS / COMMON
                continue
            n0 = strtab[4] + name
            nm = data[n0:data.index(b'\0', n0)].decode('ascii', 'replace')
            out.append((nm, value, size))
    return out


def odd(objs, min_size=2):
    return sorted((o for o in objs if o[2] >= min_size and o[1] & 1), key=lambda o: o[1])


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    with open(argv[1], 'rb') as f:
        objs = objects(f.read())
    bad = odd(objs)
    for nm, value, size in bad:
        print('even_bufs: %s at 0x%08x (%d bytes) is odd: add __attribute__((aligned(4)))' % (nm, value, size))
    if bad:
        print('even_bufs: %d odd object(s) in %s (docs/brick-safety.md "Even buffers")' % (len(bad), argv[1]))
        return 1
    print('even_bufs: %d objects, none odd' % len(objs))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
