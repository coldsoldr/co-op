"""tools/nophysics.py on a synthetic LDR (no firmware): stock checks, blanks across blocks and a FILL block, refusal.

Python 3.8 compatible, stdlib only.
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from functools import reduce

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import ldr_dump as LDR  # noqa: E402
import nophysics as NP  # noqa: E402


def hdr(flags, target, count, arg):
    h = bytearray(struct.pack('<IIII', (0xAD << 24) | flags | 6, target, count, arg))
    h[2] = reduce(lambda a, b: a ^ b, h)
    return bytes(h)


BASE = 0x01900000
CODE_A = bytes(range(256)) * 2                       # 0x01900000..0x01900200
FILL_ARG = 0x92009200                                # 32 bytes of a repeated instruction, as in Box2D
CODE_B = bytes((i * 7) & 0xff for i in range(256))  # 0x01900220..0x01900320


def build():
    body = hdr(0, BASE, len(CODE_A), 0) + CODE_A
    body += hdr(0x0100, BASE + 0x200, 0x20, FILL_ARG)
    body += hdr(0, BASE + 0x220, len(CODE_B), 0) + CODE_B
    body += hdr(0x8000, 0xffa00000, 0, 0)
    return hdr(0x4000, 0xffa00000, 0, len(body)) + body


def memory(raw):
    return {s: bytes(r) for s, r in LDR.memory_runs(LDR.parse(raw)[0])}[BASE]


class TestNophysics(unittest.TestCase):
    def setUp(self):
        self.raw = build()
        mem = memory(self.raw)
        lo, hi = BASE + 0x100, BASE + 0x300           # across block A, the FILL block and block B
        self.blank = (lo, hi)
        feat = {'firmware': {'R246': NP.R246_OP1_SHA256}, 'hooks': {
            'patches': [{'addr': hex(BASE + 0x10), 'expect': mem[0x10:0x14].hex(), 'bytes': '8cfe9101', 'what': 'ptr'}],
            'blank': [{'addr': hex(lo), 'end': hex(hi), 'what': 'code',
                       'expect': 'sha256:' + hashlib.sha256(mem[0x100:0x300]).hexdigest()}]}}
        fd, self.feature = tempfile.mkstemp(suffix='.json')
        with os.fdopen(fd, 'w') as f:
            json.dump(feat, f)

    def tearDown(self):
        os.unlink(self.feature)

    def test_apply(self):
        out, lines = NP.apply(self.raw, self.feature)
        self.assertEqual(len(out), len(self.raw))
        mem = memory(out)
        self.assertEqual(mem[0x10:0x14], bytes.fromhex('8cfe9101'))
        self.assertEqual(mem[0x100:0x300], NP.RTS * 0x100)
        self.assertEqual(mem[:0x10], memory(self.raw)[:0x10])
        self.assertEqual(mem[0x300:], memory(self.raw)[0x300:])
        fill = [b for b in LDR.parse(out)[0] if b['flags'] & 0x0100][0]
        self.assertEqual(fill['argument'], 0x00100010)
        self.assertTrue(all(ok for _, ok, _ in NP.check(out, self.feature, applied=True)))

    def test_refuses_changed_bytes(self):
        raw = bytearray(self.raw)
        i = raw.index(CODE_B[:16]) + 40                # one byte inside the blank, in block B
        raw[i] ^= 1
        with self.assertRaises(LDR.LdrError):
            NP.apply(bytes(raw), self.feature)

    def test_refuses_twice(self):
        out, _ = NP.apply(self.raw, self.feature)
        with self.assertRaises(LDR.LdrError):
            NP.apply(out, self.feature)

    def test_real_feature_loads(self):
        patches, blanks = NP.load_feature()
        self.assertEqual((len(patches), len(blanks)), (40, 17))
        self.assertEqual(sum(hi - lo for lo, hi, _, _ in blanks), 144888)
        redirected = [a for a, _, new, _ in patches if new == struct.pack('<I', NP.RTS_TARGET)]
        self.assertEqual(len(redirected), 36)
        self.assertEqual(min(lo for lo, _, _, _ in blanks), NP.RTS_TARGET)


if __name__ == '__main__':
    unittest.main()
