"""Tests for the pure parts of tools/emu_gate.py (the op1emu gate): the stats parser, the --expect lines, the
scenario rewrite, the PPM to PNG conversion, the scratch-file guard and the audio compare. No emulator is started.
Python 3.8 compatible, stdlib only.
"""
import array
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))

import emu_gate as G  # noqa: E402

STATS = """coop_stats 3 build e7
cb blocks 220629 max 1994099 last 220539 cycles 1349092258
cost avg/max/n/p0/p1 pf_mode 0 pf_live 0 stage=12089/20992/774 fx1_plugin=3068/3570/900/12/7
plugins seen 11 loaded 9 failed 2 disabled 0
plugfile tape_echo.coop failed needs 718480: over the plugin memory budget
plugfile cs_hall.coop failed needs 514880: over the plugin memory budget
fxctl fx 1 turns 11 fine 1 p0=36116i2:11197/1/16703/0
slow_worst cb 1994099 block 197883 marks 13 lost 0: 01@0 02@4391
"""


class Stats(unittest.TestCase):
    def setUp(self):
        self.s = G.parse_stats(STATS)

    def test_pairs_and_build(self):
        self.assertEqual(self.s['plugins'][0]['loaded'], 9)
        self.assertEqual(self.s['coop_stats'][0]['_'], ['3', 'build', 'e7'])
        self.assertEqual(self.s['cb'][0]['cycles'], 1349092258)

    def test_cost_items(self):
        c = self.s['cost'][0]
        self.assertEqual(c['stage'], {'avg': 12089, 'max': 20992, 'n': 774})
        self.assertEqual(c['fx1_plugin']['p1'], 7)
        self.assertEqual(c['pf_mode'], 0)

    def test_repeated_tags_and_free_text(self):
        self.assertEqual(len(self.s['plugfile']), 2)
        self.assertEqual(self.s['plugfile'][1]['_'][0], 'cs_hall.coop')
        self.assertEqual(self.s['fxctl'][0]['p0'], '36116i2:11197/1/16703/0')
        self.assertEqual(self.s['slow_worst'][0]['marks'], 13)

    def test_expect(self):
        v = dict(self.s, faults=0, audio={'nonzero': 5})
        self.assertEqual(G.check_expect('plugins.loaded==9', v), (True, 'plugins.loaded = 9'))
        self.assertFalse(G.check_expect('plugins.failed == 0', v)[0])
        self.assertTrue(G.check_expect('cost.fx1_plugin.max<60000', v)[0])
        self.assertTrue(G.check_expect('audio.nonzero>0', v)[0])
        self.assertTrue(G.check_expect('faults==0', v)[0])
        self.assertFalse(G.check_expect('plugins.lodaed==9', v)[0])      # a typo never passes
        with self.assertRaises(G.GateError):
            G.check_expect('plugins loaded 9', v)


class Scenario(unittest.TestCase):
    def test_screenshots_redirected(self):
        text, shots = G.rewrite_screenshots('at 100\ntap synth\nwait 5\nscreenshot /root/op1emu-work/fx2.ppm\nquit\n',
                                            '/out', 'sc')
        self.assertEqual(shots, [os.path.join('/out', 'sc_fx2.ppm')])
        self.assertIn('screenshot ' + shots[0], text)
        self.assertNotIn('/root/', text)

    def test_last_at(self):
        self.assertEqual(G.last_at_ms('# c\nat 30000\ntap synth 300\nat 34000\nrelease c4\nwait 45000\nquit\n'), 34000)
        self.assertIsNone(G.last_at_ms('wait 100\nquit\n'))

    def test_ppm_to_png(self):
        d = tempfile.mkdtemp()
        ppm, png = os.path.join(d, 'a.ppm'), os.path.join(d, 'a.png')
        pix = bytes(range(24))                                # 4 x 2 RGB
        with open(ppm, 'wb') as f:
            f.write(b'P6\n4 2\n255\n' + pix)
        self.assertEqual(G.ppm_to_png(ppm, png), (4, 2))
        with open(png, 'rb') as f:
            data = f.read()
        self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>II', data[16:24]), (4, 2))
        n = struct.unpack('>I', data[33:37])[0]
        raw = zlib.decompress(data[41:41 + n])
        self.assertEqual(raw, b'\0' + pix[:12] + b'\0' + pix[12:])


class Scratch(unittest.TestCase):
    def test_only_gate_names(self):
        self.assertEqual(G.scratch_path('/t', 'gate_1_2_x.bin'), os.path.join('/t', 'gate_1_2_x.bin'))
        for bad in ('pristine.bin', 'gate_1_pristine.bin', 'gate_stock.bin', 'otp.bin', 'nand.bin'):
            with self.assertRaises(G.GateError):
                G.scratch_path('/t', bad)

    def test_bases_are_never_removed(self):
        d = tempfile.mkdtemp()
        for name in ('pristine.bin', 'plug_pristine.bin', 'run_stock.bin', 'otp.bin', 'gate_9_1_det.bin'):
            with open(os.path.join(d, name), 'wb') as f:
                f.write(b'x')
        removed = [n for n in sorted(os.listdir(d)) if G.remove_scratch(os.path.join(d, n))]
        self.assertEqual(removed, ['gate_9_1_det.bin'])
        self.assertEqual(len(os.listdir(d)), 4)


def tone(frames, start, amp, shift=0):
    a = array.array('h', [0]) * (2 * frames)
    for i in range(start + shift, frames):
        v = amp if (i - start - shift) // 50 % 2 == 0 else -amp
        a[2 * i], a[2 * i + 1] = v, v // 2
    return a


class Compare(unittest.TestCase):
    RATE = 1000

    def cmp(self, a, b, tol=16):
        sa = G.wav_summary(a, 2, self.RATE, skip_s=1.0)
        sb = G.wav_summary(b, 2, self.RATE, skip_s=1.0)
        return G.wav_compare(a, b, 2, self.RATE, sa, sb, skip_s=1.0, tol=tol)

    def test_identical(self):
        self.assertEqual(self.cmp(tone(4000, 2000, 3000), tone(4000, 2000, 3000))['verdict'], 'identical')

    def test_block_shift_aligns(self):
        r = self.cmp(tone(4000, 2000, 3000), tone(4000, 2000, 3000, shift=64))
        self.assertEqual(r['verdict'], 'aligned')
        self.assertEqual(r['aligned_max_diff'], 0)
        self.assertEqual(r['onset_b'] - r['onset_a'], 64)

    def test_level_change_differs(self):
        r = self.cmp(tone(4000, 2000, 3000), tone(4000, 2000, 2000))
        self.assertEqual(r['verdict'], 'differs')
        self.assertGreater(r['rms_rel_diff'], 0.3)

    def test_summary(self):
        s = G.wav_summary(tone(4000, 2000, 3000), 2, self.RATE, skip_s=1.0)
        self.assertEqual((s['frames'], s['onset_frame'], s['peak']), (4000, 2000, [3000, 1500]))
        self.assertAlmostEqual(s['width'], 1.0 / 3, places=3)


if __name__ == '__main__':
    unittest.main()
