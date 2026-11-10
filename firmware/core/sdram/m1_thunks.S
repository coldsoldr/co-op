/* coop-fw m1 (docs/request-rings-collateral.md): timing thunks around seven stock calls of the audio callback, so the
 * gap profile (coop_cost.h, g_coop_gap_prof) has the stock stages' edges, with the performance counters at each.
 *
 * Wired only in a test image (tools/l1_stereo_insert.py, COOP_M1=1); a release image never reaches this code. Each
 * site in L1 (a 4-byte CALL pcrel24 of the stock) is retargeted to a 12-byte L1 trampoline in the dead bytes of the
 * replaced mix loop A (`[--SP] = P0; P0 = thunk; JUMP (P0)`), which reaches the thunk with every register as the
 * stock call had it (the thunk pops P0 first). The USB push is reached by CALL (P1) from an imm pair, which is
 * repointed at its thunk directly.
 *
 * A thunk marks its entry, calls the stock function with the stock's registers AND the stock's stack pointer (the
 * caller's return address is kept in a static word, not on the stack, so a callee that reads arguments above SP sees
 * them where it expects), marks the exit and returns with the callee's R0 / R1. The mark (coop_m1_mark) touches only
 * R0-R3, P0-P1 and ASTAT, saved around it; it records (id, CYCLES) in g_mk and the two counters in g_mk_pf, exactly
 * as coop_mark_at_pf does, and nothing while the gap profile is off. The callees run in the audio interrupt and do
 * not nest themselves (0xffa00e50's two calls come one after the other, each through its own thunk).
 *
 * Mark ids: 0x60 / 0x61 the DSP 0xffa017ec, 0x62 / 0x63 the synth voice loop 0xffa009a0, 0x64 / 0x65 the drum
 * voice loop 0xffa00684, 0x66 / 0x67 and 0x68 / 0x69 the master stage 0xffa00e50 from its two call sites, 0x6a /
 * 0x6b the USB push l1v_push_lr 0x019e1b3c, 0x6c / 0x6d the callback's last call 0xffa036c4.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text

/* R0 = id. Clobbers R1-R3, P0-P1, ASTAT (the callers save them). */
    .global _coop_m1_mark
    .type   _coop_m1_mark, STT_FUNC
_coop_m1_mark:
    P0.L = _g_coop_gap_prof; P0.H = _g_coop_gap_prof;
    R1 = [P0];
    CC = R1 == 0;
    IF CC JUMP .Lm_out;
    R3 = CYCLES;
    P0.L = _g_mk_n; P0.H = _g_mk_n;
    R1 = [P0];
    R2.L = 128; R2.H = 0;
    CC = R1 < R2 (IU);
    IF !CC JUMP .Lm_lost;
    R2 = R1;
    R2 += 1;
    [P0] = R2;
    P1 = R1;
    P1 = P1 << 2;
    P1 = P1 + P1;                    /* n * 8: one {u32, u32} pair */
    P0.L = _g_mk; P0.H = _g_mk;
    P0 = P0 + P1;
    [P0] = R0;
    [P0 + 4] = R3;
    P0.L = _g_mk_pf; P0.H = _g_mk_pf;
    P0 = P0 + P1;
    R2 = 0;
    R3 = 0;
    P1.L = _g_coop_pf_live; P1.H = _g_coop_pf_live;
    R1 = [P1];
    CC = R1 == 0;
    IF CC JUMP .Lm_pf;
    P1.L = 0x8100; P1.H = 0xFFE0;    /* PFCNTR0 / PFCNTR1 */
    R2 = [P1];
    R3 = [P1 + 4];
.Lm_pf:
    [P0] = R2;
    [P0 + 4] = R3;
.Lm_out:
    RTS;
.Lm_lost:
    P0.L = _g_mk_lost; P0.H = _g_mk_lost;
    R1 = [P0];
    R1 += 1;
    [P0] = R1;
    RTS;
    .size _coop_m1_mark, .-_coop_m1_mark

    .macro M1_SAVE
    [--SP] = ASTAT;
    [--SP] = R0;
    [--SP] = R1;
    [--SP] = R2;
    [--SP] = R3;
    [--SP] = P0;
    [--SP] = P1;
    [--SP] = RETS;
    .endm
    .macro M1_REST
    RETS = [SP++];
    P1 = [SP++];
    P0 = [SP++];
    R3 = [SP++];
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    ASTAT = [SP++];
    .endm

/* name, the stock target (hi, lo halves), entry id, exit id, 1 = entered from an L1 trampoline (P0 on the stack) */
    .macro M1_SITE name, thi, tlo, idin, idout, tramp
    .global _coop_m1_\name
    .type   _coop_m1_\name, STT_FUNC
_coop_m1_\name:
    .if \tramp
    P0 = [SP++];                     /* the trampoline's push: every register is now the stock call's */
    .endif
    M1_SAVE
    R0 = [SP];                       /* the caller's return address (saved RETS) ... */
    P0.L = .Lrets_\name; P0.H = .Lrets_\name;
    [P0] = R0;                       /* ... kept off the stack */
    R0.L = \idin; R0.H = 0;
    CALL _coop_m1_mark;
    M1_REST
    P1.L = \tlo; P1.H = \thi;        /* P1: no argument of a compiled callee; the stock call clobbers it too */
    CALL (P1);                       /* SP as at the stock call */
    M1_SAVE
    R0.L = \idout; R0.H = 0;
    CALL _coop_m1_mark;
    P0.L = .Lrets_\name; P0.H = .Lrets_\name;
    R0 = [P0];
    [SP] = R0;                       /* M1_REST pops it into RETS */
    M1_REST
    RTS;
    .size _coop_m1_\name, .-_coop_m1_\name
    .section .bss
    .align 4
.Lrets_\name:
    .space 4
    .text
    .endm

    M1_SITE dsp,      0xffa0, 0x17ec, 0x60, 0x61, 1
    M1_SITE voices,   0xffa0, 0x09a0, 0x62, 0x63, 1
    M1_SITE drums,    0xffa0, 0x0684, 0x64, 0x65, 1
    M1_SITE master_a, 0xffa0, 0x0e50, 0x66, 0x67, 1
    M1_SITE master_b, 0xffa0, 0x0e50, 0x68, 0x69, 1
    M1_SITE push,     0x019e, 0x1b3c, 0x6a, 0x6b, 0
    M1_SITE tail,     0xffa0, 0x36c4, 0x6c, 0x6d, 1
