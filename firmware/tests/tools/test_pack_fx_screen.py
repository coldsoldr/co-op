"""Test e9's effect screens in tools/plugin/pack.py: coop_fx_t.flags (COOP_FX_F_SCREEN) and coop_fx_t.screen agree with
plugin.json "screen", the SVG limit, and the packed ports when they are built (make -C plugins). No toolchain needed;
the file checks skip when build/plugins has no port with a screen."""
import json
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'plugin'))
import pack as P  # noqa: E402
import reloc as F  # noqa: E402

PORTS = ('bitcrusher', 'drive', 'ensemble', 'freq_shift', 'tape_drive', 'tape_echo', 'wavefolder', 'cs_hall', 'cs_plate')
SCREEN = {'svg': 'x.svg', 'symbol': 'SCREEN_x'}
ENTRY, SCR = 0x40, 0x80


def text(*parts):
    with open(os.path.join(ROOT, *parts)) as f:
        return f.read()


def image(flags, magic=P.COOP_FX_MAGIC, size=0x100):
    img = bytearray(size)
    struct.pack_into('<III', img, ENTRY, magic, 1, flags)
    return img


def reloc_to(target, at=ENTRY + P.FX_SCREEN_FIELD):
    return [(at, F.R_ABS32, F.SYM_LOCAL, target)]


class Check(unittest.TestCase):
    def ok(self, img, screen, relocs, entry_size=24, have_svg=True):
        P.fx_screen_check(img, len(img), ENTRY, entry_size, screen, SCR if screen else F.NONE, relocs, have_svg)

    def test_no_screen(self):
        self.ok(image(0), None, [])                                         # every effect before e9

    def test_screen(self):
        self.ok(image(P.COOP_FX_F_SCREEN), SCREEN, reloc_to(SCR))

    def test_refusals(self):
        with self.assertRaises(P.PackError):                                # the flag without plugin.json "screen"
            self.ok(image(P.COOP_FX_F_SCREEN), None, [])
        with self.assertRaises(P.PackError):                                # "screen" without the flag
            self.ok(image(0), SCREEN, reloc_to(SCR))
        with self.assertRaises(P.PackError):                                # an unknown flag
            self.ok(image(0x2), None, [])
        with self.assertRaises(P.PackError):                                # the field missing (a 5-member coop_fx_t)
            self.ok(image(P.COOP_FX_F_SCREEN), SCREEN, reloc_to(SCR), entry_size=20)
        with self.assertRaises(P.PackError):                                # the field naming another object
            self.ok(image(P.COOP_FX_F_SCREEN), SCREEN, reloc_to(SCR + 8))
        with self.assertRaises(P.PackError):                                # no relocation there (a null pointer)
            self.ok(image(P.COOP_FX_F_SCREEN), SCREEN, [])
        with self.assertRaises(P.PackError):                                # no SVG
            self.ok(image(P.COOP_FX_F_SCREEN), SCREEN, reloc_to(SCR), have_svg=False)
        with self.assertRaises(P.PackError):                                # a screen on a kennel-layout entry
            self.ok(image(P.COOP_FX_F_SCREEN, magic=0), SCREEN, reloc_to(SCR))

    def test_kennel_layout_left_alone(self):
        self.ok(image(0x1234, magic=0), None, [])                           # no COFX magic: kennel's own layout

    def test_constants_agree_with_the_sdk(self):
        sdk = text('sdk', 'coop_plugin.h')
        self.assertIn('#define COOP_FX_F_SCREEN    0x%xu' % P.COOP_FX_F_SCREEN, sdk)
        self.assertIn('#define COOP_FX_MAGIC   0x%08Xu' % P.COOP_FX_MAGIC, sdk)
        body = sdk[sdk.index('typedef struct coop_fx_t {'):sdk.index('} coop_fx_t;')]
        members = re.findall(r'^\s+(?:const )?[\w ]+?\**\s*\(?\*?(\w+)\)?(?:\(|;)', body, re.M)
        self.assertEqual(members, ['magic', 'api', 'flags', 'init', 'render', 'screen'])   # offset 20 = the sixth word


class Packed(unittest.TestCase):
    def setUp(self):
        self.files = {}
        for n in PORTS:
            path = os.path.join(ROOT, 'build', 'plugins', n + '.coop')
            if os.path.exists(path) and 'screen' in json.loads(text('plugins', n, 'plugin.json')):
                with open(path, 'rb') as f:
                    self.files[n] = f.read()
        if not self.files:
            self.skipTest('no packed port with a screen in build/plugins (make -C plugins)')

    def test_screen_flag_svg(self):
        for name, data in self.files.items():
            h = F.parse(data)
            img = h['image']
            self.assertEqual(struct.unpack_from('<I', img, h['entry_offset'])[0], P.COOP_FX_MAGIC, name)
            self.assertEqual(struct.unpack_from('<I', img, h['entry_offset'] + 8)[0], P.COOP_FX_F_SCREEN, name)
            self.assertNotEqual(h['screen_offset'], F.NONE, name)
            target = [r for r in h['relocs'] if r[0] == h['entry_offset'] + P.FX_SCREEN_FIELD]
            self.assertEqual(len(target), 1, name)
            self.assertTrue(0 < h['svg_size'] <= P.SVG_FX_MAX + 1, name)
            self.assertTrue(data.startswith(b'<?xml') or data.startswith(b'<svg'), name)
            # a core before e9 reads none of this: the header flags carry no new bit (it would refuse the file)
            self.assertEqual(h['flags'] & ~(F.FLAG_PAGE2 | F.FLAG_SCRATCH), 0, name)


if __name__ == '__main__':
    unittest.main()
