"""Test e8's position-buffer declaration: plugin.json "scratch" in tools/plugin/pack.py, the scratch flag and extension
in tools/plugin/reloc.py (the reference of core/sdram/plugin_load.cc), and the packed ports when they are built
(make -C plugins). No toolchain needed; the file checks skip when build/plugins has no scratch file."""
import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'plugin'))
import pack as P  # noqa: E402
import reloc as F  # noqa: E402

PORTS = ('bitcrusher', 'drive', 'ensemble', 'freq_shift', 'tape_drive', 'tape_echo', 'wavefolder', 'cs_hall', 'cs_plate')
E7_FLAGS = F.FLAG_USES_SAMPLE | F.FLAG_PAGE2 | F.FLAG_STEREO | F.FLAG_INPUT | F.FLAG_BIGBUF | F.FLAG_COARSE_MASK


def built(name):
    path = os.path.join(ROOT, 'build', 'plugins', name + '.coop')
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        return f.read()


def text(*parts):
    with open(os.path.join(ROOT, *parts)) as f:
        return f.read()


def meta_of(name):
    return json.loads(text('plugins', name, 'plugin.json'))


def with_flags(data, flags):
    """the file with another header flags word and its CRC redone"""
    d = bytearray(data)
    hoff = F.header_offset(bytes(d))
    struct.pack_into('<I', d, hoff + 36, flags)
    struct.pack_into('<I', d, hoff + 12, F.crc32(bytes(d), hoff + 12))
    return bytes(d)


class Declaration(unittest.TestCase):
    def test_values(self):
        self.assertIsNone(P.scratch_bytes({'kind': 'fx'}))
        self.assertEqual(P.scratch_bytes({'kind': 'fx', 'scratch': 134784}), 134784)
        self.assertEqual(P.scratch_bytes({'kind': 'fx', 'scratch': F.SCRATCH_MAX}), F.SCRATCH_MAX)

    def test_refusals(self):
        for bad in (0, -1, F.SCRATCH_MAX + 1, 1.5, '4096', True, None):
            with self.assertRaises(P.PackError):
                P.scratch_bytes({'kind': 'fx', 'scratch': bad})
        with self.assertRaises(P.PackError):                                   # effects only
            P.scratch_bytes({'kind': 'synth', 'scratch': 4096})

    def test_constants_agree_with_the_sdk_and_the_loader(self):
        sdk = text('sdk', 'coop_plugin.h')
        self.assertIn('#define COOP_FX_SCRATCH_MAX  %du' % F.SCRATCH_MAX, sdk)
        self.assertIn('#define COOP_FX_SLOTS        5', sdk)
        load = text('core', 'sdram', 'plugin_load.h')
        self.assertIn('#define KN_FLAG_SCRATCH     0x%xu' % F.FLAG_SCRATCH, load)
        self.assertIn('#define KN_EXT_SCRATCH      %d ' % F.EXT_SCRATCH, load)
        self.assertEqual(F.FLAG_SCRATCH & E7_FLAGS, 0)                            # a new bit: e7 cores refuse it

    def test_ports_declare_it(self):
        for name in PORTS:
            meta = meta_of(name)
            n = P.scratch_bytes(meta)
            self.assertTrue(n and n % 32 == 0, name)


class Extension(unittest.TestCase):
    def test_parse_ext(self):
        ext = struct.pack('<HHI', F.EXT_SCRATCH, 4, 4096) + struct.pack('<HH', 0, 0)
        data = bytes(16) + ext
        self.assertEqual(F.parse_ext(data, 16, 16)[F.EXT_SCRATCH], struct.pack('<I', 4096))
        with self.assertRaises(F.PluginError):                                 # wrong length
            F.parse_ext(bytes(16) + struct.pack('<HHH', F.EXT_SCRATCH, 2, 1) + bytes(2) + bytes(4), 16, 16)
        with self.assertRaises(F.PluginError):                                 # twice
            F.parse_ext(bytes(16) + ext[:8] + ext, 16, 16)


class Packed(unittest.TestCase):
    def setUp(self):
        self.files = {n: built(n) for n in PORTS if built(n)}
        if not self.files:
            self.skipTest('no packed ports in build/plugins (make -C plugins)')

    def test_flag_and_size(self):
        for name, data in self.files.items():
            h = F.parse(data)
            meta = meta_of(name)
            self.assertTrue(h['flags'] & F.FLAG_SCRATCH, name)
            self.assertEqual(h['scratch'], meta['scratch'], name)
            self.assertTrue(h['flags'] & ~E7_FLAGS, name + ': an e7 loader must refuse the file (unknown flags)')
            self.assertLessEqual(h['bss_size'], 64, name + ': the state belongs in the position buffer')

    def test_flag_without_size_and_size_without_flag(self):
        name, data = next(iter(self.files.items()))
        h = F.parse(data)
        with self.assertRaises(F.PluginError):                                 # the extension without the flag
            F.parse(with_flags(data, h['flags'] & ~F.FLAG_SCRATCH))
        old = built('stereo_delay')
        if old:
            h0 = F.parse(old)
            self.assertIsNone(h0['scratch'])
            with self.assertRaises(F.PluginError):                             # the flag without the extension
                F.parse(with_flags(old, h0['flags'] | F.FLAG_SCRATCH))


if __name__ == '__main__':
    unittest.main()
