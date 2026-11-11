"""Tests for tools/boot_logo.py (the e10 boot picture).

Synthetic checks run everywhere; the apply() round trip needs the owner's stock OP1_vdk.ldr and is skipped without it.
Python 3.8 compatible, stdlib only.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import boot_logo as BL  # noqa: E402
from ldr_dump import LdrError, memory_runs, parse  # noqa: E402


class Drawing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = BL.draw_screen()

    def test_art_clear_of_the_version_line(self):
        self.assertLess(BL.art_bottom(self.img), BL.VERSION_LINES * 8)

    def test_centred(self):
        xs = [x for row in self.img for x, px in enumerate(row) if any(px)]
        self.assertLess(abs((min(xs) + max(xs)) / 2.0 - BL.CENTRE_X), 3)

    def test_picture_mapping(self):
        pic = BL.picture_bytes(self.img)
        self.assertEqual(len(pic), BL.PIC_SIZE)
        # the leftmost dot's centre: landscape (x, 84) -> row 359 - x, column 84
        dot = BL.WEB_DOT * BL.K
        width = 4 * dot + 3 * BL.WEB_DOT_GAP * BL.K + BL.WEB_TEXT_GAP * BL.K + BL.WORDMARK_ADVANCE * BL.WEB_FONT * BL.K / 1000
        x = int(BL.CENTRE_X - width / 2 + dot / 2)
        r, c = BL.X_OF_ROW0 - x, int(BL.CENTRE_Y)
        v = pic[2 * (r * BL.PIC_COLS + c)] | pic[2 * (r * BL.PIC_COLS + c) + 1] << 8
        self.assertEqual(v, ((0x69 >> 3) << 11) | ((0x8E >> 2) << 5) | (0xFF >> 3))
        # columns past the panel's 200 are black
        self.assertFalse(any(pic[2 * (r * BL.PIC_COLS + 200):2 * (r * BL.PIC_COLS + 240)]))


class Version(unittest.TestCase):
    def test_text(self):
        self.assertEqual(BL.version_text('e10'), 'coop e10 - 246')

    def test_string_fits_and_keeps_the_line(self):
        s = BL.console_string('coop e10 - 246')
        self.assertEqual(len(s), BL.VERSION_SIZE)
        self.assertTrue(s.startswith(b'\n' * 17 + b' '))
        self.assertIn(b'coop e10 - 246\0', s)

    def test_long_tag_still_fits_or_fails(self):
        s = BL.console_string('coop e10-very-long-tag - 246')
        self.assertEqual(len(s), BL.VERSION_SIZE)
        with self.assertRaises(LdrError):
            BL.console_string('x' * 60)

    def test_ascii_only(self):
        with self.assertRaises(LdrError):
            BL.console_string('coop e10 · 246')

    def test_tag_from_header(self):
        self.assertRegex(BL.build_tag(), r'^[a-z0-9]+$')


@unittest.skipUnless(os.path.isfile(BL.STOCK_LDR), 'stock OP1_vdk.ldr not present')
class Apply(unittest.TestCase):
    def test_round_trip_and_refuses_twice(self):
        stock = open(BL.STOCK_LDR, 'rb').read()
        out, text, n = BL.apply(stock, 'e10')
        self.assertEqual(text, 'coop e10 - 246')
        old, new = parse(stock)[0], parse(out)[0]
        # every stock block keeps its place; only blocks before FINAL are added
        self.assertEqual(len(new), len(old) + n)
        for a, b in zip(old[:-2], new[:-2]):
            self.assertEqual((a['target'], a['count'], a['flags'] & 0x0400), (b['target'], b['count'], b['flags'] & 0x0400))
        runs_old = {s: bytes(r) for s, r in memory_runs(old)}
        runs_new = {s: bytes(r) for s, r in memory_runs(new)}
        self.assertEqual(set(runs_old), set(runs_new))
        for start in runs_old:
            if start not in (0x01000000, BL.PIC_ADDR):
                self.assertEqual(runs_old[start], runs_new[start])
        with self.assertRaises(LdrError):
            BL.apply(out, 'e10')


if __name__ == '__main__':
    unittest.main()
