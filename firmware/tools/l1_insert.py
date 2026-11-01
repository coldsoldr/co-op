#!/usr/bin/env python3
"""Insert an L1 hook into a (plaintext) OP-1 LDR: place a small trampoline in free L1 and retarget an
L1 CALL site to it, so an L1 call reaches a coop function in SDRAM (L1 cannot reach SDRAM by pcrel24).

  l1_insert.py IN.ldr OUT.ldr --call-site 0xffa017ae --call-stock 0xffa04280 --fn 0x02c00020 \
               [--tramp-at 0xffa0bfb4]

- Adds a 16-byte trampoline at --tramp-at (free L1; kennel's .l1tramp range 0xffa0bfb4..0xffa0c000):
  [--SP]=RETS; P0=<fn>; CALL(P0); RETS=[SP++]; RTS;  (preserves RETS across the indirect call).
- Retargets the CALL at --call-site (currently calling --call-stock) to the trampoline, after checking
  the stock bytes. Both --call-site and --tramp-at are in L1, so the CALL is pcrel24-reachable.

Run on the DECRYPTED LDR (op1emu's input), after inject_app added the SDRAM payload and decrypt-ldr
ran. For the device, run encrypt-ldr afterwards. No key or firmware bytes are stored here.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr_dump
from inject_app import call24, plain_block, DMA_CODE  # noqa: F401

# trampoline template (assembled from core/patches/l1_tramp.S). Each `P0.x = imm` is a 32-bit
# instruction: opcode halfword then the imm halfword, so the imm fields are at byte offsets 4 (P0.L)
# and 8 (P0.H), not 2/6.
#   67 01        [--SP]=RETS
#   08 e1 LL LL  P0.L = imm   (imm at offset 4)
#   48 e1 HH HH  P0.H = imm   (imm at offset 8)
#   60 00        CALL (P0)
#   27 01        RETS=[SP++]
#   10 00        RTS
TRAMP = bytearray.fromhex('670108e1000048e1000060002701' + '1000')  # 16 bytes
P0_LO_OFF, P0_HI_OFF = 4, 8


def build_tramp(fn):
    t = bytearray(TRAMP)
    struct.pack_into('<H', t, P0_LO_OFF, fn & 0xffff)
    struct.pack_into('<H', t, P0_HI_OFF, (fn >> 16) & 0xffff)
    return bytes(t)


def patch_call(dxe, site, stock_target, new_target):
    """Replace the 4-byte CALL at `site` (targeting stock_target) with a CALL to new_target."""
    old = call24(site, stock_target)
    new = call24(site, new_target)
    for b in dxe:
        loaded = ldr_dump.loaded_bytes(b)
        if loaded is None:
            continue
        start, end = b['target'], b['target'] + len(loaded)
        if start <= site < end and site + 4 <= end:
            off = site - start
            if b['flags'] & 0x0100:
                raise SystemExit('call site {:#x} is in a FILL block, cannot patch'.format(site))
            payload = bytearray(b['payload'])
            if bytes(payload[off:off + 4]) != old:
                raise SystemExit('call site {:#x}: stock bytes {} != expected {}'.format(
                    site, bytes(payload[off:off + 4]).hex(), old.hex()))
            payload[off:off + 4] = new
            b['payload'] = bytes(payload)
            return
    raise SystemExit('call site {:#x} not found in a loaded block'.format(site))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ldr_in')
    ap.add_argument('ldr_out')
    ap.add_argument('--call-site', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--call-stock', type=lambda s: int(s, 0), required=True, help='the CALL target to replace')
    ap.add_argument('--fn', type=lambda s: int(s, 0), required=True, help='coop SDRAM function to call')
    ap.add_argument('--tramp-at', type=lambda s: int(s, 0), default=0xffa0bfb4)
    args = ap.parse_args()

    tramp = build_tramp(args.fn)
    if not (0xffa0bfb4 <= args.tramp_at and args.tramp_at + len(tramp) <= 0xffa0c000):
        raise SystemExit('trampoline must fit in free L1 0xffa0bfb4..0xffa0c000')

    dxes = ldr_dump.parse(open(args.ldr_in, 'rb').read())
    if len(dxes) != 1:
        raise SystemExit('expected a single-DXE LDR')
    dxe = dxes[0]

    # add the trampoline as a plain block, inserted before FINAL
    blk = plain_block(args.tramp_at, tramp)
    final = [i for i, b in enumerate(dxe) if b['flags'] & 0x8000]
    if len(final) != 1:
        raise SystemExit('expected exactly one FINAL block')
    dxe.insert(final[0], blk)

    # retarget the call site to the trampoline
    patch_call(dxe, args.call_site, args.call_stock, args.tramp_at)

    open(args.ldr_out, 'wb').write(ldr_dump.serialize(dxes))
    print('{}: trampoline at {:#x} -> {:#x}; CALL {:#x} retargeted {:#x} -> {:#x}'.format(
        args.ldr_out, args.tramp_at, args.fn, args.call_site, args.call_stock, args.tramp_at))


if __name__ == '__main__':
    main()
