"""Transcribe the stock Phase render 0x01A0BC5C..0x01A0BE80 (objdump text of the stock image) into re-assemblable bfin
gas for tests/sim/run_phase_check.sh. The absolute data addresses (the pitch words, the sine table, the global
accumulator) become test arrays, the loop's LSETUP becomes JUMP.L _coop_phase_loop_near, and the loop top / bottom /
back become the labels the near stub and the rewrite jump to. No packet needs splitting: the one that stores a register
its dsp32 ALU op writes (0x01A0BD2C, R0.H = ... || W[P5++] = R0) stores only the low half, which the op leaves alone.

  phase_transcribe.py STOCK_DIS OUT.S          (stdlib only, Python 3.8)
"""
import re, sys

src, dst = sys.argv[1], sys.argv[2]
START, END = 0x1a0bc5c, 0x1a0be80
lines = []
for raw in open(src):
    m = re.match(r'^ ([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$', raw.rstrip('\n'))
    if not m:
        continue
    a = int(m.group(1), 16)
    if a < START or a > END:
        continue
    text = m.group(3)
    text = re.sub(r'/\*.*?\*/', '', text).strip()
    if not text:
        continue
    lines.append((a, text))

targets = set()
for a, t in lines:
    for x in re.findall(r'0x0x([0-9a-f]+)', t):
        targets.add(int(x, 16))

SUBST = {
    ('I0.L', '0x1950'): 'I0.L = _t_dbuf;', ('I0.H', '0xff80'): 'I0.H = _t_dbuf;',
    ('P3.L', '0xe5e8'): 'P3.L = _t_sine;', ('P3.H', '0x101'): 'P3.H = _t_sine;',
    ('P1.L', '0x4b8c'): 'P1.L = _t_acc;', ('P1.H', '0x1de'): 'P1.H = _t_acc;',
}
out = ['    .text', '    .align 4', '    .global _t_phase_render', '_t_phase_render:']
for a, t in lines:
    if a == 0x1a0bce4:
        out.append('    .global _stock_phase_loop_top')
        out.append('_stock_phase_loop_top:')
    if a == 0x1a0bd5a:
        out.append('    .global _stock_phase_loop_bottom')
        out.append('_stock_phase_loop_bottom:')
    if a == 0x1a0bd5c:
        out.append('    .global _stock_phase_loop_back')
        out.append('_stock_phase_loop_back:')
    if a in targets:
        out.append('L_%x:' % a)
    m = re.match(r'^(I0\.L|I0\.H|P3\.L|P3\.H|P1\.L|P1\.H) = (0x[0-9a-f]+);', t)
    if m and (m.group(1), m.group(2)) in SUBST:
        out.append('    ' + SUBST[(m.group(1), m.group(2))])
        continue
    if a == 0x1a0bce0:
        out.append('    JUMP.L _coop_phase_loop_near;')
        continue
    t = re.sub(r'0x0x([0-9a-f]+)', lambda mm: 'L_' + mm.group(1), t)
    out.append('    ' + t)
open(dst, 'w').write('\n'.join(out) + '\n')
n = sum(1 for a, t in lines if 0x1a0bce4 <= a <= 0x1a0be80)
print('instructions in loop range:', n)
