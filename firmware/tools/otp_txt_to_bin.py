#!/usr/bin/env python3
"""Convert a community OP-1 OTP text dump (e.g. Tolsi/op1dumps TE-OP-1-BF524-OTP-dump.txt) into the
flat 8 KiB binary op1emu reads as `otp.bin`.

op1emu stores OTP as array<u32, 0x200 * 4> and reads otp.bin straight into it (src/cpu/otp.cpp
LoadFromFile), so otp.bin is 0x200 pages x 16 bytes = 8192 bytes, each page four little-endian u32.

The text dump has one line per written page:  `[!]0xPP  <hex64 lo> [<hex64 hi>]`, where each 64-bit
field is a big-endian hex number and an unwritten page is `00`. Each field is stored little-endian
(its 8 bytes reversed), which is how it sits in the chip and matches the key arrangement
tools/l1_decrypt.py verified. `--no-reverse` writes the field bytes as-is if a future dump needs it.

No key is embedded here; this operates on a dump the user supplies locally. The output otp.bin is
gitignored. op1emu is the final check: if it boots the firmware to the main screen, the bytes are
right; if not, try --no-reverse.
"""
import argparse
import re

PAGES = 0x200
PAGE_BYTES = 16


def field_bytes(hexstr, reverse):
    hexstr = hexstr.strip()
    if len(hexstr) % 2:
        hexstr = '0' + hexstr
    b = bytes.fromhex(hexstr)
    b = b[::-1] if reverse else b
    return b.rjust(8, b'\x00')[:8] if reverse else b.ljust(8, b'\x00')[:8]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('txt', help='the OTP text dump')
    ap.add_argument('out', help='otp.bin to write (8192 bytes)')
    ap.add_argument('--no-reverse', action='store_true', help='store each 64-bit field as-is (not little-endian)')
    args = ap.parse_args()

    mem = bytearray(PAGES * PAGE_BYTES)
    line_re = re.compile(r'^!?\s*0x([0-9a-fA-F]+)\s+(.*)$')
    written = 0
    for raw in open(args.txt):
        m = line_re.match(raw.strip())
        if not m:
            continue
        page = int(m.group(1), 16)
        if page >= PAGES:
            continue
        fields = m.group(2).split()
        if len(fields) == 1 and fields[0] in ('0', '00'):
            continue  # unwritten page: leave zero
        off = page * PAGE_BYTES
        lo = field_bytes(fields[0], not args.no_reverse) if len(fields) >= 1 else b'\x00' * 8
        hi = field_bytes(fields[1], not args.no_reverse) if len(fields) >= 2 else b'\x00' * 8
        mem[off:off + 8] = lo
        mem[off + 8:off + 16] = hi
        written += 1

    open(args.out, 'wb').write(mem)
    print('{}: {} bytes, {} pages written'.format(args.out, len(mem), written))
    key = mem[0xD0 * PAGE_BYTES:0xD0 * PAGE_BYTES + 16]
    print('page 0xD0 (key) bytes: {}'.format(key.hex()))


if __name__ == '__main__':
    main()
