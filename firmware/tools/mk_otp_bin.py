#!/usr/bin/env python3
"""Turn a coop_l1.bin dump (tools/dump_l1) into the pieces op1emu and the key check need.

coop_l1.bin layout (little-endian):
  u32 magic "C1DP", u32 version, u32 region_count
  region_count x { u32 addr, u32 len }
  u8 key[16]                                 OTP page 0xD0 as the te-boot context block holds it
  u32 otp_status, u32 otp_bytes              (version >= 2)
  u8 otp[otp_bytes]                          (present when a DUMP_OTP build captured it)
  raw region bytes, in table order

Writes, into an output directory:
  region_<addr>.bin    each captured L1 region (region_ffa00000.bin is the instruction SRAM)
  otp.bin              the 8 KiB OTP, if the dump has it (op1emu reads this from its working dir)
  key.hex              the 16-byte context-block key, as hex (for l1_decrypt.py check-key)

Nothing here is firmware or a secret of ours; it only unpacks the owner's own device dump. Keep the
outputs out of git (the repo .gitignore already blocks *.bin and symbols/otp*).
"""
import argparse
import os
import struct
import sys

OUT_MAGIC = 0x50443143  # "C1DP"


def parse_dump(data):
    if len(data) < 12 or struct.unpack_from('<I', data, 0)[0] != OUT_MAGIC:
        raise SystemExit('not a coop_l1.bin dump (bad magic)')
    magic, version, nregions = struct.unpack_from('<III', data, 0)
    off = 12
    table = []
    for _ in range(nregions):
        addr, length = struct.unpack_from('<II', data, off)
        table.append((addr, length))
        off += 8
    key = data[off:off + 16]
    off += 16
    otp = b''
    if version >= 2:
        otp_status, otp_bytes = struct.unpack_from('<II', data, off)
        off += 8
        otp = data[off:off + otp_bytes]
        off += otp_bytes
    else:
        otp_status = 0
    regions = {}
    for addr, length in table:
        regions[addr] = data[off:off + length]
        off += length
    return {'version': version, 'key': key, 'otp_status': otp_status, 'otp': otp, 'regions': regions}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', help='coop_l1.bin from the device')
    ap.add_argument('out', help='output directory')
    args = ap.parse_args()

    d = parse_dump(open(args.dump, 'rb').read())
    os.makedirs(args.out, exist_ok=True)

    for addr, blob in sorted(d['regions'].items()):
        path = os.path.join(args.out, 'region_{:08x}.bin'.format(addr))
        open(path, 'wb').write(blob)
        print('{}: {} bytes'.format(path, len(blob)))

    keyhex = d['key'].hex()
    open(os.path.join(args.out, 'key.hex'), 'w').write(keyhex + '\n')
    print('key.hex: {}'.format(keyhex))

    if d['otp']:
        status = {1: 'ok', 2: 'a read reported an error'}.get(d['otp_status'], 'unknown')
        path = os.path.join(args.out, 'otp.bin')
        open(path, 'wb').write(d['otp'])
        print('{}: {} bytes (OTP read status: {})'.format(path, len(d['otp']), status))
        if d['otp_status'] != 1:
            print('WARNING: the OTP read was not clean; op1emu may reject this otp.bin')
    else:
        print('no OTP in this dump (flash the DUMP_OTP build to capture it for op1emu)')

    print('\nNext: verify the key, then (with otp.bin + a NAND image) boot the owner image in op1emu.')
    print('  python tools/l1_decrypt.py check-key <stock OP1_vdk.ldr> '
          'third_party/kennel/synth_sdk/l1_patches.json ' + keyhex)


if __name__ == '__main__':
    main()
