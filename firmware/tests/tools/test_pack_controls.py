"""Test tools/plugin/pack.py's "controls" declaration (e4 engines, e6 effects: fine, latch) without a toolchain."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'plugin'))
import pack as P  # noqa: E402
import reloc as F  # noqa: E402

GAIN = {'id': 'gain', 'action': 'fn_turn', 'encoder': 'blue', 'word': 5, 'kind': 'continuous', 'default': 16384, 'fine': True}
HOLD = {'id': 'hold', 'action': 'click', 'encoder': 'green', 'kind': 'latch', 'bit': 0}
HEADS = {'id': 'heads', 'action': 'click', 'encoder': 'white', 'word': 6, 'kind': 'steps', 'steps': ['1', '1+2', '1+3'], 'default': 0}


def lines(controls, kind=F.KIND_FX):
    return P.control_lines({'controls': controls}, kind).decode('ascii').rstrip('\0').splitlines()


class Controls(unittest.TestCase):
    def test_fx_tape_echo(self):
        got = lines([GAIN, HOLD, HEADS])
        self.assertEqual(got[0], '4\t0\t5\tc\t0\t16384\t1\t1\tgain\t\t2')
        self.assertEqual(got[1], '1\t1\t0\tl\t0\t0\t0\t1\thold\t')
        self.assertEqual(got[2], '1\t2\t6\ts\t3\t0\t1\t1\theads\t1,1+2,1+3')

    def test_engine_unchanged(self):                       # rings 0.5's model line, as e5 packed it
        model = {'id': 'model', 'action': 'fn_turn', 'encoder': 'blue', 'word': 5, 'kind': 'steps',
                 'steps': ['modal', 'symp'], 'default': 0, 'detents': 2, 'wrap': True}
        self.assertEqual(lines([model], F.KIND_SYNTH), ['4\t0\t5\ts\t2\t0\t1\t2\tmodel\tmodal,symp\t1'])

    def bad(self, controls, kind=F.KIND_FX):
        with self.assertRaises(P.PackError):
            P.control_lines({'controls': controls}, kind)

    def test_refusals(self):
        self.bad([HOLD], F.KIND_SYNTH)                                        # latch: effects only
        self.bad([dict(HOLD, action='fn_turn')])                              # latch on a turn
        self.bad([dict(HOLD, word=5)])                                        # latch with a word
        self.bad([HOLD, dict(HOLD, id='frz', encoder='orange')])              # one bit twice
        self.bad([GAIN, {'id': 'trim', 'action': 'shift_fn_turn', 'encoder': 'blue', 'word': 6, 'kind': 'continuous'}])
        self.bad([dict(GAIN, action='shift_fn_turn')])                        # fine on another action
        self.bad([dict(HEADS, fine=True)])                                    # fine on steps
        self.bad([dict(GAIN, word=4)])                                        # the selector word


if __name__ == '__main__':
    unittest.main()
