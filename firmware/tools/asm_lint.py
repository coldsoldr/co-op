#!/usr/bin/env python3
"""Flag parallel (multi-issue) packets in coop's assembly that the device rejects but op1emu runs.

    python3 -I tools/asm_lint.py core/sdram/*.S          # exit 1 when anything is flagged

op1emu raises EXCAUSE 0x22 (illegal instruction combination) only when a slot fails to decode, so a packet the
hardware refuses can pass every emulator gate. Rules, from device faults and from what the stock compiler emits
(33,108 parallel packets in the stock SDRAM code and L1):
  - A P-register (or SP / FP) store in slot 2 beside a shift in slot 1 (`A1 = A1 >> 16`, `R0 = R1 >>> 4`, ...).
    The device crashed with SEQSTAT 0x22 at `A1 = A1 >> 16 || [FP - 0x64] = P0 || ...` in spring_fast.S, in p5b
    (slot 3 a load) and again in p5c (slot 3 NOP), 2026-11-04. Stock pairs its ~860 parallel P-register stores only
    with ALU and multiply instructions (GCC's own `A1 = R0.L * R7.L (FU) || [P0] = P2 || NOP` in coop_master_apply
    runs every block on the device), and its shifts only with D-register stores.
  - A P-register store in slot 2 with anything but NOP in slot 3 (stock: ~860 with NOP, one doubtful exception).
"""
import re, sys

PSTORE = re.compile(r'\]\s*=\s*(P[0-5]|SP|FP)\b')
SHIFT = re.compile(r'(<<|>>|\bROT\b|\bASHIFT\b|\bLSHIFT\b)')


def lint(path):
    bad = []
    for n, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        code = re.sub(r'/\*.*?\*/', '', line.split('//')[0])
        if '||' not in code:
            continue
        slots = [s.strip().rstrip(';').strip() for s in code.split('||')]
        if len(slots) != 3 or not PSTORE.search(slots[1]):
            continue
        if SHIFT.search(slots[0]) and '*' not in slots[0]:
            bad.append((n, 'P-register store beside a shift ("%s")' % slots[0], line.strip()))
        elif slots[2].upper() != 'NOP':
            bad.append((n, 'P-register store in slot 2 beside "%s"' % slots[2], line.strip()))
    return bad


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: asm_lint.py FILE.S [...]')
    total = 0
    for p in sys.argv[1:]:
        for n, why, text in lint(p):
            print('%s:%d: %s\n    %s' % (p, n, why, text))
            total += 1
    print('%d packet(s) flagged' % total)
    sys.exit(1 if total else 0)


if __name__ == '__main__':
    main()
