"""Transcribe the stock Voltage render 0x01A49BF4..0x01A49EF6 (objdump text of the stock image) into re-assemblable
bfin gas for tests/sim/run_voltage_check.sh. The absolute data addresses become test arrays, the loop's LSETUP becomes
JUMP.L _coop_voltage_loop_near, and the loop top / bottom / back become the labels the near stub and the rewrite jump
to. One packet is split: bfin-elf-run commits a dsp32 ALU result (add, subtract, ABS) before a store in the same
packet reads the register (multiplies and shifts it defers), the hardware and op1emu read every register first.

  voltage_transcribe.py STOCK_DIS OUT.S          (stdlib only, Python 3.8)
"""
import re, sys

src, dst = sys.argv[1], sys.argv[2]
START, END = 0x1a49bf4, 0x1a49ef6
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
    ('P5.L', '0xa5ec'): 'P5.L = _t_tab;', ('P5.H', '0x106'): 'P5.H = _t_tab;',
    ('I1.L', '0x1950'): 'I1.L = _t_dbuf;', ('I1.H', '0xff80'): 'I1.H = _t_dbuf;',
    ('P3.L', '0xe5e8'): 'P3.L = _t_sine;', ('P3.H', '0x101'): 'P3.H = _t_sine;',
    ('I0.L', '0x6154'): 'I0.L = _t_scope;', ('I0.H', '0x1df'): 'I0.H = _t_scope;',
}
out = ['    .text', '    .align 4', '    .global _t_voltage_render', '_t_voltage_render:']
for a, t in lines:
    if a == 0x1a49cba:
        out.append('    .global _stock_voltage_loop_top')
        out.append('_stock_voltage_loop_top:')
    if a == 0x1a49ed0:
        out.append('    .global _stock_voltage_loop_bottom')
        out.append('_stock_voltage_loop_bottom:')
    if a == 0x1a49ed2:
        out.append('    .global _stock_voltage_loop_back')
        out.append('_stock_voltage_loop_back:')
    if a in targets:
        out.append('L_%x:' % a)
    m = re.match(r'^(P5\.L|P5\.H|I1\.L|I1\.H|P3\.L|P3\.H|I0\.L|I0\.H) = (0x[0-9a-f]+);', t)
    if m and (m.group(1), m.group(2)) in SUBST:
        out.append('    ' + SUBST[(m.group(1), m.group(2))])
        continue
    if a == 0x1a49cb6:
        out.append('    JUMP.L _coop_voltage_loop_near;')
        continue
    if a == 0x1a49e02:   # bfin-elf-run commits a dsp32 ALU result before a parallel store reads it; the hardware does not
        out.append('    W[P4 + 0x1c] = R0;')
        out.append('    R0 = ABS R3 (V);')
        continue
    if a == 0x1a49ee6:   # the copy loop's LSETUP
        out.append('    LSETUP(L_1a49eea, L_1a49eec) LC0 = P0;')
        continue
    t = re.sub(r'0x0x([0-9a-f]+)', lambda mm: 'L_' + mm.group(1), t)
    out.append('    ' + t)
open(dst, 'w').write('\n'.join(out) + '\n')
n = sum(1 for a, t in lines if 0x1a49cba <= a <= 0x1a49ed0)
print('instructions in loop range:', n)
