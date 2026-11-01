"""Test tools/mk_otp_bin.py against a synthetic version-2 coop_l1.bin (no real firmware)."""
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import mk_otp_bin as M  # noqa: E402


def build_dump(version, regions, key, otp_status=0, otp=b''):
    out = bytearray()
    out += struct.pack('<III', M.OUT_MAGIC, version, len(regions))
    for addr, blob in regions:
        out += struct.pack('<II', addr, len(blob))
    out += key
    if version >= 2:
        out += struct.pack('<II', otp_status, len(otp))
        out += otp
    for _addr, blob in regions:
        out += blob
    return bytes(out)


class TestMkOtp(unittest.TestCase):
    def test_v2_with_otp(self):
        regions = [(0xffa00000, bytes(range(256)) * 4), (0xff800000, b'\xab' * 128)]
        key = bytes(range(16))
        otp = b'\x5a' * (0x200 * 16)
        dump = build_dump(2, regions, key, otp_status=1, otp=otp)
        d = M.parse_dump(dump)
        self.assertEqual(d['version'], 2)
        self.assertEqual(d['key'], key)
        self.assertEqual(d['otp'], otp)
        self.assertEqual(d['regions'][0xffa00000], regions[0][1])
        self.assertEqual(d['regions'][0xff800000], regions[1][1])

    def test_v1_no_otp(self):
        regions = [(0xffa00000, b'\x01\x02\x03\x04')]
        d = M.parse_dump(build_dump(1, regions, bytes(16)))
        self.assertEqual(d['otp'], b'')
        self.assertEqual(d['regions'][0xffa00000], b'\x01\x02\x03\x04')

    def test_writes_files(self):
        regions = [(0xffa00000, b'\x11' * 64)]
        otp = b'\x00' * (0x200 * 16)
        dump = build_dump(2, regions, bytes(range(16)), otp_status=1, otp=otp)
        with tempfile.TemporaryDirectory() as td:
            dpath = os.path.join(td, 'coop_l1.bin')
            open(dpath, 'wb').write(dump)
            outdir = os.path.join(td, 'out')
            sys.argv = ['mk_otp_bin.py', dpath, outdir]
            M.main()
            self.assertTrue(os.path.isfile(os.path.join(outdir, 'otp.bin')))
            self.assertEqual(os.path.getsize(os.path.join(outdir, 'otp.bin')), 0x200 * 16)
            self.assertTrue(os.path.isfile(os.path.join(outdir, 'region_ffa00000.bin')))
            self.assertEqual(open(os.path.join(outdir, 'key.hex')).read().strip(),
                             bytes(range(16)).hex())

    def test_bad_magic(self):
        with self.assertRaises(SystemExit):
            M.parse_dump(b'xxxx' + b'\x00' * 32)


if __name__ == '__main__':
    unittest.main()
