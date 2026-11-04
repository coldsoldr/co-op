#!/usr/bin/env python3
"""Flag parallel (multi-issue) packets in coop's assembly that the device rejects but op1emu runs.

    python3 -I tools/asm_lint.py core/sdram/*.S          # exit 1 when anything is flagged

op1emu raises EXCAUSE 0x22 (illegal instruction combination) only when a slot fails to decode, so a packet the
hardware refuses can pass every emulator gate. Rules, each from a device fault or from what the stock compiler
never emits:
  - a P-register (or SP / FP) store in slot 2 with anything but NOP in slot 3. The p5b test crashed with SEQSTAT
    0x22 at exactly such a packet (spring_fast.S, `A1 = A1 >> 16 || [FP - F_W9] = P0 || R1.H = W[I2]`,
    2026-11-04); the stock code has ~860 parallel P-register stores, all with NOP beside them.
"""
import re, sys

PSTORE = re.compile(r'\]\s*=\s*(P[0-5]|SP|FP)\b')


def lint(path):
    bad = []
    for n, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        code = re.sub(r'/\*.*?\*/', '', line.split('//')[0])
        if '||' not in code:
            continue
        slots = [s.strip().rstrip(';').strip() for s in code.split('||')]
        if len(slots) == 3 and PSTORE.search(slots[1]) and slots[2].upper() != 'NOP':
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
