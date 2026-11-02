"""Encodings and the stock-byte guard of tools/l1_stereo_insert.py (no firmware bytes: a synthetic LDR)."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import l1_stereo_insert as si   # noqa: E402
import ldr_dump                 # noqa: E402
from inject_app import plain_block, call24   # noqa: E402


class Encodings(unittest.TestCase):
    # reference bytes from bfin-elf-as (binutils 2.43) for the same source with thunk 0x02c01234
    def test_tramp_synth(self):
        self.assertEqual(si.tramp_synth(0x02c01234).hex(), '42ad09e1341249e1c0025100')

    def test_tramp_drum(self):
        self.assertEqual(si.tramp_drum(0x02c01234).hex(), '48ae0ae134124ae1c0025200')

    def test_tramp_mix(self):
        self.assertEqual(si.tramp_mix(0x02c01234).hex(), '670108e1341248e1c002600027011000')

    def test_jump_s(self):
        # JUMP.S from 0xffa00fb2 to 0xffa00fdc: +0x2a bytes = imm 0x15
        self.assertEqual(si.jump_s(0xffa00fb2, 0xffa00fdc), bytes.fromhex('1520'))
        # backwards by 4 bytes (as bfin-as emitted for `JUMP.S .-4`): 0x2ffe
        self.assertEqual(si.jump_s(0x1000, 0x0ffc), bytes.fromhex('fe2f'))

    def test_tramp_noteon(self):
        self.assertEqual(si.tramp_noteon(0x02c01234).hex(), 'c8ad0ae134124ae1c0025200')

    def test_layout_fits_and_is_contiguous(self):
        tr = si.layout(si.TRAMP_BASE, {'synth': 1, 'drum': 2, 'mix': 3, 'noteon': 4})
        self.assertEqual([a for _, a, _ in tr], [0xffa0bfb4, 0xffa0bfc0, 0xffa0bfcc, 0xffa0bfdc])
        self.assertEqual(sum(len(b) for _, _, b in tr), 52)

    def test_site_patches_keep_length_and_kept_instructions(self):
        tr = si.layout(si.TRAMP_BASE, {'synth': 0x02c01538, 'drum': 0x02c0154a, 'mix': 0x02c01564, 'noteon': 0x02c01580})
        p = dict((a, (s, n)) for a, s, n in si.site_patches(tr))
        s, n = p[si.SYNTH_SITE]
        self.assertEqual(len(s), len(n))
        self.assertEqual(n[:4], bytes.fromhex('03aa1850'))                 # R3 = W[P0+0x10] (X); R0 = R0 + R3
        self.assertEqual(n[4:], call24(si.SYNTH_CALL_AT, 0xffa0bfb4))
        s, n = p[si.DRUM_SITE]
        self.assertEqual(n[:6], bytes.fromhex('49e510000850'))
        self.assertEqual(n[6:], call24(si.DRUM_CALL_AT, 0xffa0bfc0))
        s, n = p[si.MIX_SITE]
        self.assertEqual(n, call24(si.MIX_SITE, 0xffa0bfcc) + bytes.fromhex('1520'))
        s, n = p[si.NOTEON_SITE]
        self.assertEqual(n[:4], bytes.fromhex('0aab1050'))                 # R2 = W[P1+0x18] (X); R0 = R0 + R2
        self.assertEqual(n[4:], call24(si.NOTEON_CALL_AT, 0xffa0bfdc))


class TapeStereo(unittest.TestCase):
    THUNKS = {"synth": 1, "drum": 2, "mix": 3, "noteon": 4, "tape_rec": 0x02c01768, "tape_gains": 0x02c017be}

    def test_tramp_plain(self):
        # P2.L = 0x1234; P2.H = 0x2c0; JUMP (P2); NOP  (bfin-elf-as)
        self.assertEqual(si.tramp_plain(0x02c01234).hex(), "0ae134124ae1c00252000000")

    def test_layout_with_tape_fills_the_free_range_exactly(self):
        tr = si.layout(si.TRAMP_BASE, self.THUNKS, tape=True)
        self.assertEqual([n for n, _, _ in tr], ["synth", "drum", "mix", "noteon", "tape_rec", "tape_gains"])
        self.assertEqual(tr[-1][1] + len(tr[-1][2]), si.TRAMP_END)
        self.assertEqual(si.layout(si.TRAMP_BASE, self.THUNKS)[-1][0], "noteon")   # default: no tape

    def test_tape_patches(self):
        tr = si.layout(si.TRAMP_BASE, self.THUNKS, tape=True)
        t = dict((n, a) for n, a, _ in tr)
        p = dict((a, (s, n)) for a, s, n in si.tape_patches(tr))
        for a, (s, n) in p.items():
            self.assertEqual(len(s), len(n), hex(a))
        for site in si.TAPE_REC_SITES:                                       # CALL 0xffa06334 -> CALL tramp_tape_rec
            self.assertEqual(p[site], (call24(site, 0xffa06334), call24(site, t["tape_rec"])))
        s, n = p[si.TAPE_GAINS_SITE]                                         # 8 bytes: CALL + two NOPs
        self.assertEqual(s, bytes.fromhex("09e1f01749e18c01"))
        self.assertEqual(n, call24(si.TAPE_GAINS_SITE, t["tape_gains"]) + bytes.fromhex("00000000"))
        self.assertEqual(p[0xffa0705a], (bytes.fromhex("2468"), bytes.fromhex("4468")))   # P4 = 4 -> 8
        self.assertEqual(p[0xffa07106], (bytes.fromhex("2268"), bytes.fromhex("4268")))   # P2 = 4 -> 8
        self.assertEqual(p[0xffa07138], (bytes.fromhex("00a9"), bytes.fromhex("00aa")))   # W[P0+8] -> W[P0+0x10]
        self.assertEqual(p[0xffa070a6], (bytes.fromhex("0aa9"), bytes.fromhex("0aaa")))
        self.assertEqual(p[0x01a54172], (bytes.fromhex("904a"), bytes.fromhex("984a")))   # BITSET bit 18 -> 19
        self.assertEqual(len(p), 2 + 1 + 5)


def synthetic_ldr(l1_bytes_at):
    """A one-DXE LDR with plain blocks at the given {address: bytes} plus a FINAL block."""
    first = plain_block(0xffa00000, b'')           # an empty FIRST block; serialize() sets its argument
    first['flags'] |= 0x4000
    dxe = [first]
    for addr, data in sorted(l1_bytes_at.items()):
        dxe.append(plain_block(addr, data))
    final = plain_block(0xffa00000, b'\0' * 16)
    final['flags'] |= 0x8000
    dxe.append(final)
    return ldr_dump.serialize([dxe])


class Guard(unittest.TestCase):
    thunks = {'synth': 0x02c01538, 'drum': 0x02c0154a, 'mix': 0x02c01564, 'noteon': 0x02c01580,
              'core_noteon': 0x02c015a0}

    def blocks(self, synth=si.SYNTH_STOCK, drum=si.DRUM_STOCK, mix=si.MIX_STOCK):
        pad = b'\xff\xff'
        b = {si.SYNTH_SITE - 2: pad + synth + pad,
             si.DRUM_SITE - 2: pad + drum + pad,
             si.MIX_SITE - 2: pad + mix + b'\0' * 0x30,
             si.NOTEON_SITE - 2: pad + si.NOTEON_STOCK + pad}
        for site in si.CORE_NOTEON_CALLERS:
            b[site - 2] = pad + call24(site, si.CORE_NOTEON) + pad
        return b

    def test_core_noteon_callers_go_through_the_region_c_trampoline(self):
        out, lines = si.insert(synthetic_ldr(self.blocks()), self.thunks)
        dxe = ldr_dump.parse(out)[0]
        got = {b['target']: b['payload'] for b in dxe if b['payload']}
        for site in si.CORE_NOTEON_CALLERS:
            self.assertEqual(got[site - 2][2:6], call24(site, si.SDRAM_TRAMP_AT))
        tramp = got[si.SDRAM_TRAMP_AT]
        self.assertEqual(len(tramp), 16)
        self.assertEqual(tramp[4:6], (0x02c015a0 & 0xffff).to_bytes(2, 'little'))
        self.assertEqual(tramp[8:10], (0x02c015a0 >> 16).to_bytes(2, 'little'))
        self.assertTrue(any('sdram tramp' in l for l in lines))

    def test_patches_a_matching_image(self):
        out, lines = si.insert(synthetic_ldr(self.blocks()), self.thunks)
        dxe = ldr_dump.parse(out)[0]
        got = {b['target']: b['payload'] for b in dxe if b['payload']}
        self.assertEqual(got[si.SYNTH_SITE - 2][2:10], bytes.fromhex('03aa1850') + call24(si.SYNTH_CALL_AT, 0xffa0bfb4))
        self.assertEqual(got[si.MIX_SITE - 2][2:8], call24(si.MIX_SITE, 0xffa0bfcc) + bytes.fromhex('1520'))
        self.assertIn(0xffa0bfb4, got)
        self.assertEqual(len(got[0xffa0bfb4]), 52)
        self.assertTrue(any('tramp_mix' in l for l in lines))

    def test_refuses_wrong_stock_bytes(self):
        bad = bytes.fromhex('42ad03aa18506300')   # last halfword differs
        with self.assertRaises(SystemExit):
            si.insert(synthetic_ldr(self.blocks(synth=bad)), self.thunks)

    def test_refuses_double_patch(self):
        out, _ = si.insert(synthetic_ldr(self.blocks()), self.thunks)
        with self.assertRaises(SystemExit):
            si.insert(out, self.thunks)


if __name__ == '__main__':
    unittest.main()
