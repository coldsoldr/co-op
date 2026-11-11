/* m1 probe_code: three 4 KB blocks of straight-line code (2047 NOPs and an RTS each: 128 cache lines, no loads, no
 * branches, as the p6f icache probe in coop_stats.cc), 32-byte aligned.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
    .text

    .macro BLOCK name
    .global _\name
    .type   _\name, STT_FUNC
    .align 32
_\name:
    .rept 2047
    NOP;
    .endr
    RTS;
    .size _\name, .-_\name
    .endm

    BLOCK probe_code_a
    BLOCK probe_code_b
    BLOCK probe_code_c
