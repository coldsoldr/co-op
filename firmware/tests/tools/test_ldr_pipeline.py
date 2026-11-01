"""Tests for the LDR layer and the brick-safety gate, on synthetic fixtures (no real firmware).

Covers:
  - ldr_dump.parse / serialize round-trips a hand-built single-DXE LDR byte for byte;
  - memory_runs merges blocks in load order;
  - verify_bootloader passes a directory with only OP1_vdk.ldr, and rejects a te-boot.ldr, an
    over-size app, and FRC / .KEY file names.

Python 3.8 compatible, stdlib only.
"""
import os
import struct
import sys
import tempfile
import unittest
from functools import reduce

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', '..', 'tools')
sys.path.insert(0, TOOLS)

import ldr_dump as LDR  # noqa: E402
import verify_bootloader as VB  # noqa: E402

SIGNATURE = 0xAD
DMA = 6


def hdr(flags, target, count, arg):
    code = (SIGNATURE << 24) | flags | DMA
    h = bytearray(struct.pack('<IIII', code, target, count, arg))
    h[2] = reduce(lambda a, b: a ^ b, h)  # checksum byte: XOR of all 16 header bytes is 0
    return bytes(h)


def build_ldr(app_payload):
    """A minimal single-DXE app LDR: FIRST (empty), a data block at 0x01000000, a FINAL -> 0xffa00000."""
    blocks = bytearray()
    # Reserve the FIRST header; its arg (bytes to next DXE) is set after the body is known.
    body = bytearray()
    body += hdr(0, 0x01000000, len(app_payload), 0) + app_payload
    body += hdr(0x8000, 0xffa00000, 0, 0)  # FINAL
    first = hdr(0x4000, 0xffa00000, 0, len(body))  # FIRST: arg = bytes from end of its header to next DXE (file end)
    return bytes(first + body)


class TestLdrRoundTrip(unittest.TestCase):
    def test_parse_serialize_identity(self):
        raw = build_ldr(b'\xaa\xbb\xcc\xdd' * 64)
        dxes = LDR.parse(raw)
        self.assertEqual(len(dxes), 1)
        self.assertEqual(LDR.serialize(dxes), raw)

    def test_memory_runs(self):
        raw = build_ldr(bytes(range(256)) * 1)
        dxes = LDR.parse(raw)
        runs = LDR.memory_runs(dxes[0])
        self.assertEqual(runs[0][0], 0x01000000)
        self.assertEqual(bytes(runs[0][1]), bytes(range(256)))

    def test_fill_block_expands(self):
        # A FILL block (flag 0x0100) loads `count` bytes of the 32-bit argument but carries no payload.
        body = bytearray()
        body += hdr(0x0100, 0x02c00000, 16, 0xa5a5a5a5)  # FILL
        body += hdr(0x8000, 0xffa00000, 0, 0)
        raw = bytes(hdr(0x4000, 0xffa00000, 0, len(body)) + body)
        dxes = LDR.parse(raw)
        runs = LDR.memory_runs(dxes[0])
        self.assertEqual(bytes(runs[0][1]), b'\xa5' * 16)
        self.assertEqual(LDR.serialize(dxes), raw)  # FILL carries no payload, still round-trips


class TestBrickSafety(unittest.TestCase):
    def _dir_with(self, files):
        d = tempfile.mkdtemp()
        for name, data in files.items():
            with open(os.path.join(d, name), 'wb') as f:
                f.write(data)
        return d

    def test_app_only_passes(self):
        ldr = build_ldr(b'\x00' * 1024)
        d = self._dir_with({'OP1_vdk.ldr': ldr})
        # verify() returns a list of problems; empty means safe to flash.
        self.assertEqual(VB.verify(d, allow_stock_bootloader=False), [])

    def test_teboot_present_fails(self):
        ldr = build_ldr(b'\x00' * 1024)
        d = self._dir_with({'OP1_vdk.ldr': ldr, 'te-boot.ldr': b'not a known bootloader'})
        self.assertNotEqual(VB.verify(d, allow_stock_bootloader=False), [])

    def test_oversize_app_fails(self):
        big = build_ldr(b'\x00' * (VB.MAX_APP_SIZE + 4))
        d = self._dir_with({'OP1_vdk.ldr': big})
        self.assertNotEqual(VB.verify(d, allow_stock_bootloader=False), [])

    def test_bad_file_names(self):
        self.assertTrue(VB.check_file_name('coop_FRC.op1'))      # FRC forces a bootloader rewrite
        self.assertTrue(VB.check_file_name('something.KEY'))     # .KEY programs the OTP key
        self.assertEqual(VB.check_file_name('op1_246_coop.op1'), [])


if __name__ == '__main__':
    unittest.main()
