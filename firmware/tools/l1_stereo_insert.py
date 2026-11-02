#!/usr/bin/env python3
"""Wire the coop stereo bus (Phase 2 milestone C1) into a plaintext OP-1 LDR: three L1 trampolines in
free L1 and three small rewrites of the stock L1 code, each checked against the stock bytes first.

  l1_stereo_insert.py IN.ldr OUT.ldr --elf build/core/coop_combined.elf [--tramp-at 0xffa0bfb4]

What it changes (docs/stereo-bus.md "C1 takeover mechanism"; all addresses [C] from the decrypted
R246 L1 and the op1emu write tracer):

  synth render call 0xffa00d22..0xffa00d2a
      stock:  P2 = [P0+0x14]; R3 = W[P0+0x10] (X); R0 = R0 + R3; CALL (P2)
      coop:   R3 = W[P0+0x10] (X); R0 = R0 + R3; CALL tramp_synth
      tramp_synth (12 B):  P2 = [P0+0x14]; P1 = coop_synth_render_thunk; JUMP (P1)
  drum render call 0xffa008fc..0xffa00906
      stock:  P0 = [P1+0x24]; R1 = W[P1+0x20] (X); R0 = R0 + R1; CALL (P0)
      coop:   R1 = W[P1+0x20] (X); R0 = R0 + R1; CALL tramp_drum
      tramp_drum (12 B):   P0 = [P1+0x24]; P2 = coop_drum_render_thunk; JUMP (P2)
  mix loop B 0xffa00fae..0xffa00fb4
      stock:  LSETUP(0xffa00fb2, 0xffa00fda) LC0 = P0; <loop body ...>
      coop:   CALL tramp_mix; JUMP.S 0xffa00fdc          (the stock body stays as dead bytes)
      tramp_mix (16 B):    [--SP] = RETS; P0 = coop_mix_thunk; CALL (P0); RETS = [SP++]; RTS
  synth note-on dispatch 0xffa00e2e..0xffa00e36 (C2: the voice loop calls *(vtable+0x1c) for a voice
  with a pending note; R7 = voice index)
      stock:  P0 = [P1+0x1c]; R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL (P0)
      coop:   R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL tramp_noteon
      tramp_noteon (12 B): P0 = [P1+0x1c]; P2 = coop_synth_noteon_thunk; JUMP (P2)

The SDRAM thunk addresses come from the ELF's symbol table (inject_app.read_elf_symbols), so this runs
after inject_app added the payload and l1_decrypt decrypt-ldr produced the plaintext image. For the
device, encrypt-ldr afterwards. Stdlib only; no firmware bytes are stored here.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ldr_dump                                            # noqa: E402
from inject_app import call24, plain_block, read_elf_symbols, symbol_address   # noqa: E402
from l1_insert import build_tramp   # noqa: E402  ([--SP]=RETS; P0=fn; CALL (P0); RETS=[SP++]; RTS)

TRAMP_BASE = 0xffa0bfb4            # kennel's free .l1tramp range, up to 0xffa0c000
TRAMP_END = 0xffa0c000

# --- stock bytes at the sites (little-endian halfwords as objdump shows them) ---
SYNTH_SITE = 0xffa00d22
SYNTH_STOCK = bytes.fromhex('42ad' '03aa' '1850' '6200')          # P2=[P0+0x14]; R3=W[P0+0x10](X); R0=R0+R3; CALL (P2)
SYNTH_CALL_AT = 0xffa00d26                                        # the CALL lands after the two kept insns
DRUM_SITE = 0xffa008fc
DRUM_STOCK = bytes.fromhex('48ae' '49e51000' '0850' '6000')       # P0=[P1+0x24]; R1=W[P1+0x20](X); R0=R0+R1; CALL (P0)
DRUM_CALL_AT = 0xffa00902
MIX_SITE = 0xffa00fae
MIX_STOCK = bytes.fromhex('a2e01600' '03c8')                      # LSETUP(0xffa00fb2,0xffa00fda) LC0=P0; first body halfword
MIX_RESUME = 0xffa00fdc                                           # first instruction after loop B
# synth note-on dispatch inside the voice loop (reached from 0xffa00aa6 for a voice with a pending note):
#   P0 = [P1+0x1c]; R2 = W[P1+0x18] (X); R0 = R0 + R2; CALL (P0)   ->  R2 = ...; R0 = ...; CALL tramp_noteon
NOTEON_SITE = 0xffa00e2e
NOTEON_STOCK = bytes.fromhex('c8ad' '0aab' '1050' '6000')
NOTEON_CALL_AT = 0xffa00e32
# Core::NoteOn (SDRAM 0x01967854, R0 = voice) is the path a played key takes; its three VoiceCtl callers
# are CALL24s. Region A is beyond pcrel24 reach (+-16 MB) from 0x01a8xxxx, so they call a 16-byte
# trampoline in free region C (0x01e00000, kennel's core region; nothing in the stock LDR loads
# 0x01e00000..0x01ffffff) which reaches coop_core_noteon_thunk in region A.
CORE_NOTEON = 0x01967854
CORE_NOTEON_CALLERS = (0x01a8442c, 0x01a848e4, 0x01a8564e)
SDRAM_TRAMP_AT = 0x01e00000


def imm_pair_bytes(preg, value):
    """`Px.L = lo; Px.H = hi` for P0..P5 (LDIMMhalf: e108 + reg for .L, e148 + reg for .H)."""
    lo = 0xe108 | preg
    hi = 0xe148 | preg
    return struct.pack('<HHHH', lo, value & 0xffff, hi, (value >> 16) & 0xffff)


def jump_s(address, target):
    """JUMP.S pcrel12 (0x2000 | imm12, imm = (target - address) / 2)."""
    off = (target - address) >> 1
    if not -(1 << 11) <= off < (1 << 11) or (target - address) & 1:
        raise SystemExit('JUMP.S {:#x} -> {:#x} out of range'.format(address, target))
    return struct.pack('<H', 0x2000 | (off & 0xfff))


def tramp_synth(thunk):
    # P2 = [P0 + 0x14]  = 42 ad ;  P1 = thunk ;  JUMP (P1) = 51 00
    return bytes.fromhex('42ad') + imm_pair_bytes(1, thunk) + bytes.fromhex('5100')


def tramp_drum(thunk):
    # P0 = [P1 + 0x24]  = 48 ae ;  P2 = thunk ;  JUMP (P2) = 52 00
    return bytes.fromhex('48ae') + imm_pair_bytes(2, thunk) + bytes.fromhex('5200')


def tramp_mix(thunk):
    # [--SP] = RETS = 67 01 ; P0 = thunk ; CALL (P0) = 60 00 ; RETS = [SP++] = 27 01 ; RTS = 10 00
    return bytes.fromhex('6701') + imm_pair_bytes(0, thunk) + bytes.fromhex('6000' '2701' '1000')


def tramp_noteon(thunk):
    # P0 = [P1 + 0x1c]  = c8 ad ;  P2 = thunk ;  JUMP (P2) = 52 00
    return bytes.fromhex('c8ad') + imm_pair_bytes(2, thunk) + bytes.fromhex('5200')


def layout(tramp_at, thunks):
    """(name, address, bytes) for the trampolines packed from tramp_at."""
    parts = [('synth', tramp_synth(thunks['synth'])), ('drum', tramp_drum(thunks['drum'])),
             ('mix', tramp_mix(thunks['mix'])), ('noteon', tramp_noteon(thunks['noteon']))]
    out, a = [], tramp_at
    for name, b in parts:
        out.append((name, a, b))
        a += len(b)
    if a > TRAMP_END:
        raise SystemExit('trampolines do not fit in free L1 {:#x}..{:#x}'.format(tramp_at, TRAMP_END))
    return out


def site_patches(tramps):
    """[(address, stock_bytes, new_bytes)] for the three L1 sites."""
    t = {name: a for name, a, _ in tramps}
    synth_new = SYNTH_STOCK[2:6] + call24(SYNTH_CALL_AT, t['synth'])
    drum_new = DRUM_STOCK[2:8] + call24(DRUM_CALL_AT, t['drum'])
    mix_new = call24(MIX_SITE, t['mix']) + jump_s(MIX_SITE + 4, MIX_RESUME)
    noteon_new = NOTEON_STOCK[2:6] + call24(NOTEON_CALL_AT, t['noteon'])
    assert len(synth_new) == len(SYNTH_STOCK) and len(drum_new) == len(DRUM_STOCK) and len(mix_new) == len(MIX_STOCK)
    assert len(noteon_new) == len(NOTEON_STOCK)
    return [(SYNTH_SITE, SYNTH_STOCK, synth_new), (DRUM_SITE, DRUM_STOCK, drum_new), (MIX_SITE, MIX_STOCK, mix_new),
            (NOTEON_SITE, NOTEON_STOCK, noteon_new)]


def patch_at(dxe, address, stock, new):
    """Replace len(stock) bytes at `address` in the loaded block that holds them, after checking them."""
    if len(stock) != len(new):
        raise SystemExit('{:#x}: patch length mismatch'.format(address))
    for b in dxe:
        loaded = ldr_dump.loaded_bytes(b)
        if loaded is None:
            continue
        start, end = b['target'], b['target'] + len(loaded)
        if start <= address and address + len(stock) <= end:
            if b['flags'] & 0x0100:
                raise SystemExit('{:#x} is in a FILL block, cannot patch'.format(address))
            payload = bytearray(b['payload'])
            off = address - start
            got = bytes(payload[off:off + len(stock)])
            if got != stock:
                raise SystemExit('{:#x}: stock bytes {} != expected {} (wrong image, or already patched?)'.format(
                    address, got.hex(), stock.hex()))
            payload[off:off + len(new)] = new
            b['payload'] = bytes(payload)
            return
    raise SystemExit('{:#x} not found in a loaded block'.format(address))


def core_noteon_patches(sdram_tramp_at=SDRAM_TRAMP_AT):
    """[(address, stock, new)] retargeting the Core::NoteOn callers at the region-C trampoline."""
    return [(site, call24(site, CORE_NOTEON), call24(site, sdram_tramp_at)) for site in CORE_NOTEON_CALLERS]


def thunk_addresses(elf):
    syms = read_elf_symbols(elf)
    return {'synth': symbol_address(syms, 'coop_synth_render_thunk')[0],
            'drum': symbol_address(syms, 'coop_drum_render_thunk')[0],
            'mix': symbol_address(syms, 'coop_mix_thunk')[0],
            'noteon': symbol_address(syms, 'coop_synth_noteon_thunk')[0],
            'core_noteon': symbol_address(syms, 'coop_core_noteon_thunk')[0]}


def insert(ldr_bytes, thunks, tramp_at=TRAMP_BASE):
    """The whole operation on LDR bytes -> (new bytes, report lines)."""
    dxes = ldr_dump.parse(ldr_bytes)
    if len(dxes) != 1:
        raise SystemExit('expected a single-DXE LDR')
    dxe = dxes[0]
    tramps = layout(tramp_at, thunks)
    lines = []
    for address, stock, new in site_patches(tramps) + core_noteon_patches():
        patch_at(dxe, address, stock, new)
        lines.append('site {:#x}: {} -> {}'.format(address, stock.hex(), new.hex()))
    blob = b''.join(b for _, _, b in tramps)
    final = [i for i, b in enumerate(dxe) if b['flags'] & 0x8000]
    if len(final) != 1:
        raise SystemExit('expected exactly one FINAL block')
    dxe.insert(final[0], plain_block(tramp_at, blob))
    dxe.insert(final[0], plain_block(SDRAM_TRAMP_AT, build_tramp(thunks['core_noteon'])))
    for name, a, b in tramps:
        lines.append('tramp_{} at {:#x} ({} B) -> {:#x}'.format(name, a, len(b), thunks[name]))
    lines.append('sdram tramp at {:#x} (16 B) -> {:#x} for Core::NoteOn callers {}'.format(
        SDRAM_TRAMP_AT, thunks['core_noteon'], ', '.join('{:#x}'.format(s) for s in CORE_NOTEON_CALLERS)))
    return ldr_dump.serialize(dxes), lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ldr_in')
    ap.add_argument('ldr_out')
    ap.add_argument('--elf', required=True, help='the coop combined ELF (for the thunk addresses)')
    ap.add_argument('--tramp-at', type=lambda s: int(s, 0), default=TRAMP_BASE)
    args = ap.parse_args()
    thunks = thunk_addresses(args.elf)
    out, lines = insert(open(args.ldr_in, 'rb').read(), thunks, args.tramp_at)
    open(args.ldr_out, 'wb').write(out)
    for line in lines:
        print(line)
    print('{}: stereo bus wired'.format(args.ldr_out))


if __name__ == '__main__':
    main()
