/* coop-fw channel strip, milestone CS3: the L1 hooks of the audio path (docs/channel-strip.md "CS3", the hook map).
 *
 * Reached from three 12-byte trampolines in loop B's dead body (0xffa00fb4.., tools/l1_stereo_insert.py) and two
 * inline rewrites of the master stage. Every register the stock code keeps live is preserved; comments stay on
 * their own lines (docs/tape-stereo.md section 10: the assembler swallows a line inside a multi-line comment).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text

/* ---- the main lane loop's meter MAX, 0xffa0712c (`R0 = MAX (R0, R1) (V)`), inside LSETUP(0xffa0711a, 0xffa07176)
 * LC1. Entered by JUMP from the trampoline (`P2 = this; JUMP (P2)`; P2 is reloaded by the stock at 0xffa07134 and
 * 0xffa0716e), RETS = 0xffa07130. Live: R3 the lane (0..7), R6 the count (CC = R6 <= 0 already), R7.H the
 * destination offset, P1 + 2 the lane's first sample, P0 the gains, P4 the meters, M0 / I1 the destinations.
 * A split lane (g_strip_lanes.dst[lane] != 0): its R6 samples are added (saturating) into dst + R7.H and the
 * caller gets CC = 1, so the stock MAC skips the lane. Everything but R0 (the MAX), P2 and CC is preserved. */
    .global _coop_strip_lane_main
    .type   _coop_strip_lane_main, STT_FUNC
_coop_strip_lane_main:
    R0 = MAX (R0, R1) (V);
    [--SP] = ASTAT;
    [--SP] = P0;
    P2.L = _g_strip_lanes; P2.H = _g_strip_lanes;
    P0 = R3;
    P2 = P2 + (P0 << 2);
    P0 = [P2];
    CC = P0 == 0;
    IF CC JUMP .Lm_stock;
    [--SP] = R1;
    [--SP] = R2;
    [--SP] = P1;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    R1 = R7 >> 16;
    R2 = R1 + R6;
    [--SP] = R3;
    R3 = [P2 + 32];
    R3 = MAX (R3, R2);
    [P2 + 32] = R3;
    R3 = [SP++];
    R1 = R1 << 1;
    P2 = R1;
    P0 = P0 + P2;
    P1 += 2;
    CC = R6 <= 0;
    IF CC JUMP .Lm_done;
    P2 = R6;
    LSETUP (.Lm_top, .Lm_bot) LC0 = P2;
.Lm_top:
    R1 = W[P1++] (X);
    R2 = W[P0] (X);
    R1.L = R1.L + R2.L (S);
.Lm_bot:
    W[P0++] = R1;
.Lm_done:
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    P1 = [SP++];
    /* CC = 1 in the saved ASTAT (bit 5), so the stock skips this lane's MAC: R2, R1, P0, ASTAT are on the stack,
     * ASTAT at [SP + 12] */
    R2 = [SP + 12];
    BITSET (R2, 5);
    [SP + 12] = R2;
    R2 = [SP++];
    R1 = [SP++];
.Lm_stock:
    P0 = [SP++];
    ASTAT = [SP++];
    RTS;
    .size _coop_strip_lane_main, .-_coop_strip_lane_main

/* ---- the seam lane loop's meter MAX, 0xffa0709c (`R0 = MAX (R2, R0) (V)`), inside LSETUP(0xffa0707a, 0xffa070e0)
 * LC1. Every P register is live here, so the trampoline is `[--SP] = P2; P2 = this; JUMP (P2)` and this pops P2.
 * RETS = 0xffa070a0. Live: R1 the lane, R6 the count (CC = R6 <= 0), R7.H the offset, P4 the lane's first sample
 * (the stock MAC reads W[P4++]), P0 the meters, P1 the gains, P3 the R destination. As the main hook otherwise. */
    .global _coop_strip_lane_seam
    .type   _coop_strip_lane_seam, STT_FUNC
_coop_strip_lane_seam:
    R0 = MAX (R2, R0) (V);
    [--SP] = ASTAT;
    [--SP] = P0;
    P2.L = _g_strip_lanes; P2.H = _g_strip_lanes;
    P0 = R1;
    P2 = P2 + (P0 << 2);
    P0 = [P2];
    CC = P0 == 0;
    IF CC JUMP .Ls_stock;
    [--SP] = R1;
    [--SP] = R2;
    [--SP] = R3;
    [--SP] = P1;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
    R1 = R7 >> 16;
    R2 = R1 + R6;
    R3 = [P2 + 32];
    R3 = MAX (R3, R2);
    [P2 + 32] = R3;
    R1 = R1 << 1;
    P2 = R1;
    P0 = P0 + P2;
    P1 = P4;
    CC = R6 <= 0;
    IF CC JUMP .Ls_done;
    P2 = R6;
    LSETUP (.Ls_top, .Ls_bot) LC0 = P2;
.Ls_top:
    R1 = W[P1++] (X);
    R2 = W[P0] (X);
    R1.L = R1.L + R2.L (S);
.Ls_bot:
    W[P0++] = R1;
.Ls_done:
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    P1 = [SP++];
    /* CC = 1 in the saved ASTAT: R3, R2, R1, P0, ASTAT are on the stack, ASTAT at [SP + 16] */
    R3 = [SP + 16];
    BITSET (R3, 5);
    [SP + 16] = R3;
    R3 = [SP++];
    R2 = [SP++];
    R1 = [SP++];
.Ls_stock:
    P0 = [SP++];
    ASTAT = [SP++];
    P2 = [SP++];
    RTS;
    .size _coop_strip_lane_seam, .-_coop_strip_lane_seam

/* ---- the tick's resampler call, 0xffa07250 (`CALL 0xffa06194`), retargeted at the trampoline
 * (`P2 = this; JUMP (P2)`), RETS = 0xffa07254. R0 = position, R1 = increment, R2 = the gain-ramp step pointer.
 * The stock pass first (it reads the stock pair and writes all 64 of d34 / db4, and uses the 12-byte outgoing area
 * of its caller: [FP + 8]); then, with a split track, coop_strip_tape(pos, inc, step). The caller relies on R0 (the
 * stock's return), R1 (the increment), the callee-saved registers and CC (set before the call, tested at
 * 0xffa0725c), so ASTAT is preserved; M0-M3, B0-B3 and L0-L3 are saved around the C call (L zeroed for it). */
    .global _coop_strip_rs_thunk
    .type   _coop_strip_rs_thunk, STT_FUNC
_coop_strip_rs_thunk:
    [--SP] = RETS;
    [--SP] = ASTAT;
    [--SP] = R0;
    [--SP] = R1;
    [--SP] = R2;
    SP += -12;
    P2.L = 0x6194; P2.H = 0xffa0;
    CALL (P2);
    SP += 12;
    P2.L = _g_strip_split; P2.H = _g_strip_split;
    R3 = [P2];
    CC = R3 == 0;
    IF CC JUMP .Lr_out;
    [--SP] = R0;
    [--SP] = M0; [--SP] = M1; [--SP] = M2; [--SP] = M3;
    [--SP] = B0; [--SP] = B1; [--SP] = B2; [--SP] = B3;
    [--SP] = L0; [--SP] = L1; [--SP] = L2; [--SP] = L3;
    L0 = 0 (X); L1 = 0 (X); L2 = 0 (X); L3 = 0 (X);
    /* 13 words pushed: the step at [SP + 52], the increment at [SP + 56], the position at [SP + 60] */
    R2 = [SP + 52];
    R1 = [SP + 56];
    R0 = [SP + 60];
    SP += -12;
    CALL _coop_strip_tape;
    SP += 12;
    L3 = [SP++]; L2 = [SP++]; L1 = [SP++]; L0 = [SP++];
    B3 = [SP++]; B2 = [SP++]; B1 = [SP++]; B0 = [SP++];
    M3 = [SP++]; M2 = [SP++]; M1 = [SP++]; M0 = [SP++];
    R0 = [SP++];
.Lr_out:
    R2 = [SP++];
    R1 = [SP++];
    SP += 4;
    ASTAT = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_strip_rs_thunk, .-_coop_strip_rs_thunk

/* ---- coop_rs_pair(pos, inc, io): the stock tape resampler (L1 0xffa06194..0xffa06244), instruction for
 * instruction, with its immediates taken from io = { step, inL base, inR base, outL, outR } (the bases 8 samples
 * below the pair, as the stock's 0x01b5ef84 / 0x01b5f434). The gain from W[0xff8029dc] and the 16 x 64 tap table
 * at 0xff802154 are the stock's. 64 outputs; returns the position after them. A C callee: R4-R7 / P3-P5 saved, the
 * caller's 12-byte area used for the table base ([FP + 8]) as the stock does; L0-L3 must be 0. */
    .global _coop_rs_pair
    .type   _coop_rs_pair, STT_FUNC
_coop_rs_pair:
    LINK 0;
    [--SP] = (R7:6, P5:3);
    P5 = R2;
    R3 = [P5];
    I3 = R3;
    P3 = [P5 + 4];
    P4 = [P5 + 8];
    R3 = [P5 + 12];
    I1 = R3;
    R3 = [P5 + 16];
    I2 = R3;
    P1.L = 0x29dc; P1.H = 0xff80;
    P2.L = 0x2154; P2.H = 0xff80;
    P0 = 0x40 (X);
    R2 = W[P1] (X);
    [SP + 0x1c] = P2;
    R7 = 0x3f (X);
    P1 = 0x80 (X);
    LSETUP (.Lp_top, .Lp_bot) LC1 = P0;
.Lp_top:
    R0 = R1 + R0;
    R3 = R0 >> 0x6;
    P0 = R3;
    R6 = R3 << 0x6;
    R6 = R0 - R6;
    R3 = R7 - R6;
    P2 = R3;
    P5 = P4 + (P0 << 1);
    I0 = P5;
    A1 = A0 = 0 || P5 = [SP + 0x1c] || R6.L = W[I0++];
    P0 = P3 + (P0 << 1);
    R3 = W[P0++] (X);
    P2 = P5 + (P2 << 1);
    R2.H = W[P2 ++ P1];
    P5 = 0xf (X);
    A0 += R2.H * R6.L (W32);
    LSETUP (.Lp_in, .Lp_ine) LC0 = P5;
.Lp_in:
    A1 += R2.H * R3.L (W32) || R2.H = W[P2 ++ P1] || R3.L = W[I0++];
.Lp_ine:
    A0 += R2.H * R3.L (W32) || R3 = W[P0++] (X) || NOP;
    A1 += R2.H * R3.L (W32);
    R3 = A0.W;
    R6 = A1.W;
    R2.H = R2.L * R3.H;
    R2.H = R2.L * R6.H || W[I2++] = R2.H || NOP;
    W[I1++] = R2.H;
    R3.L = W[I3];
.Lp_bot:
    R2.L = R2.L + R3.L (S);
    (R7:6, P5:3) = [SP++];
    UNLINK;
    RTS;
    .size _coop_rs_pair, .-_coop_rs_pair

/* ---- the master EQ call, 0xffa0110e..0xffa0111b (`P1 = 0x010004cc; CALL (P1); CALL 0xffa030f0`, 14 bytes), becomes
 * `P1 = coop_master_eq; CALL (P1); NOP; NOP`. The getter (R0 = 0xff8017e0, the EQ's state), then the stock EQ unless
 * the strip mode is on (g_coop_strip_on bit 0, bit 1 clear). The EQ (LINK, R7:4 / P5:3 saved) writes its caller's
 * 12-byte area ([FP + 8 .. 0x10]): reserved here. The code after the site reloads what it uses (P1, P0 from P5). */
    .global _coop_master_eq
    .type   _coop_master_eq, STT_FUNC
_coop_master_eq:
    [--SP] = RETS;
    SP += -12;
    P1.L = 0x04cc; P1.H = 0x0100;
    CALL (P1);
    P1.L = _g_coop_strip_on; P1.H = _g_coop_strip_on;
    R1 = [P1];
    R2 = 3;
    R1 = R1 & R2;
    CC = R1 == 1;
    IF CC JUMP .Leq_skip;
    P1.L = 0x30f0; P1.H = 0xffa0;
    CALL (P1);
.Leq_skip:
    SP += 12;
    RETS = [SP++];
    RTS;
    .size _coop_master_eq, .-_coop_master_eq
