#!/usr/bin/env python3
"""Write tests/sim/run_rings_wrap.txt: rings 0.5's fn + blue (model) and fn + green (polyphony) wrap around (coop e5,
plugin.json "wrap": true). The runner times a held chord after each step; with wrap a whole turn of a control (6
model steps, 4 polyphony steps) lands where it started, so the section after it costs what the one before did; with
clamping it stops on the last step (verb, all), which costs differently. Needs the engine list of the m1 runs
(gen_run_m1.py ROWS: stock + saw, rings and the three probes). Python 3.8, stdlib only.

    python3 -I tests/sim/gen_run_wrap.py
    COOP_M1= PLUGINS="build/plugins/saw.coop build/plugins/rings.coop build/plugins/probe_*.coop" ONLY_B=1 \\
      bash tests/sim/run_emu.sh tests/sim/run_rings_wrap.txt tests/sim/idle_200s.txt wrap
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_run_m1 as m1  # noqa: E402

m1.HOLD = 5000


class W(m1.S):
    def __init__(self):
        super().__init__(0)
        self.lines[0] = '# rings wrap (coop e5): written by tests/sim/gen_run_wrap.py, do not edit.'
        self.lines[1] = '# docs/device-tests.md "e5" says what it shows.'
        self.lines[2] = '#@name rings_wrap'

    def fn_green(self, steps):                      # fn + green: `steps` Gray transitions (+cw / -ccw)
        self.do('press help', dt=1200)
        self.do('turn enc2 %s %d' % ('cw' if steps > 0 else 'ccw', abs(steps)), dt=300)
        self.do('release help', dt=300 + 20 * abs(steps) + 200)


def scenario():
    s = W()
    s.do('tap synth 200', dt=1500)
    s.select('rings')
    s.chord('m_start')
    s.fn_blue(4); s.chord('m_plus1')                 # one model step (2 detents, 4 transitions)
    s.fn_blue(24); s.chord('m_plus7')                # six more: wrap = m_plus1's model, clamp = verb
    s.fn_blue(-4); s.chord('m_plus6')                # one back: wrap = m_start's model
    s.fn_green(2); s.chord('p_plus1')                # one polyphony step (1 detent, 2 transitions)
    s.fn_green(8); s.chord('p_plus5')                # four more: wrap = p_plus1's, clamp = all
    s.at(2000)
    s.lines += ['wait %d' % (s.t + 6000), 'quit']
    return '\n'.join(s.lines) + '\n'


if __name__ == '__main__':
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'run_rings_wrap.txt')
    open(p, 'w', newline='\n').write(scenario())
    print(p)
