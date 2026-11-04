/* coop-fw: the stock DSynth oscillator loop, rewritten (performance step 3, 2026-11-04).
 *
 * DSynth's render (0x0197E1D8) runs a 128-sample loop at 0x0197E736..0x0197E84C: a wavetable read indexed from the
 * previous sample and the phase, two phase-modulation inputs, a one-pole filter on the 32-bit state s1 (the stock
 * 32 x 32 products by R7), a quantised feedback into s2, a second product and the output scaled by a gain. The
 * compiled loop recomputes a loop-invariant gain G = sat(C40 + sat(K3 << 2) * C3c2) and H1 = C1c * G every sample,
 * reloads a dozen constants from the frame (four of them with long-form halfword loads that cannot issue in
 * parallel) and passes three values through the stack. This is the same arithmetic, every operation with the stock
 * instruction's options (fract16 multiplies rounded, (S) / (NS) adds as the stock has them, the mixed-mode MAC
 * sequences operand for operand), with G and H1 hoisted and the constants and the two loop-carried words in a
 * 32-byte block on the stack (SP lowered for the duration, so nothing below SP is written).
 * Entry: JUMP.L from 0x0197E732 (the LSETUP; the only way into the loop). At entry R0 = phase, R1 = s1, R6 = s2,
 * R2 = the previous table sample, R4 = v1, R7 = the filter coefficient, P0 = the 32-bit modulation input, I3 = the
 * 16-bit one, P4 = the table, I2 = the output, P1 = 128, [FP-0x18] = the quantised state. Exit: JUMP.L 0x0197E850 with
 * R0, R1, R6, R2 (low half = the last table sample), R3 (the quantised state), R4 (v1) and R5 as the stock loop leaves
 * them and the three stack words it wrote ([FP-0x14], [FP-0x18], [FP-0x28]); I0 / I1 untouched.
 * Frame constants (stock offsets; SP-relative ones read 32 higher here): K1 W[SP+0x6A], C20 [FP-0x20], K2 W[SP+0x66],
 * C44 [FP-0x44], C3c [SP+0x3C], K3 W[SP+0x72], C3c2 [FP-0x3C], C40 [FP-0x40], C1c [FP-0x1C], C24 [FP-0x24],
 * K4 W[SP+0x62].
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#define B_C20   0        /* 32 */
#define B_K12   4        /* .H K1, .L K2 */
#define B_C3C   8        /* 32 */
#define B_H44   12       /* .H H1, .L C44 */
#define B_C24   16       /* 32 */
#define B_GK4   20       /* .H G, .L K4 */
#define B_V1    24       /* loop-carried */
#define B_ST    28       /* loop-carried: s1 with its low half cleared */

    .section .near, "ax"
    .global _coop_dsynth_osc_loop
    .type   _coop_dsynth_osc_loop, STT_FUNC
_coop_dsynth_osc_loop:
    P2.L = _g_coop_fast_dsynth_osc; P2.H = _g_coop_fast_dsynth_osc;   /* 0: the stock loop. P2 and CC are free here */
    R5 = [P2];                                                         /* (R5 is set inside the stock loop first) */
    CC = R5 == 0;
    IF CC JUMP .Losc_stock;
    SP += -32;
    [SP + B_V1] = R4;                              /* v1 at entry (R4 = [FP-0x28], 0x0197E6BE) */
    R3 = [FP + -0x18];
    [SP + B_ST] = R3;
    R5 = [FP + -0x20];
    [SP + B_C20] = R5;
    R3 = W[SP + 0x8a] (Z);                         /* K1 (0x6a + 32) */
    R4 = W[SP + 0x86] (Z);                         /* K2 (0x66 + 32) */
    R3 = PACK (R3.L, R4.L);
    [SP + B_K12] = R3;
    R5 = [SP + 0x5c];                              /* C3c (0x3c + 32) */
    [SP + B_C3C] = R5;
    R5 = W[SP + 0x92] (Z);                         /* K3 (0x72 + 32) */
    R5.H = R5.L << 2 (S);
    R4 = [FP + -0x3c];                             /* C3c2 */
    R4.H = R5.H * R4.L;
    R5 = [FP + -0x40];                             /* C40 */
    R4.L = R5.L + R4.H (S);                        /* G */
    R3 = [FP + -0x1c];                             /* C1c */
    R3.L = R3.L * R4.L;                            /* H1 = C1c * G */
    R5 = [FP + -0x44];                             /* C44 */
    R5 = PACK (R3.L, R5.L);
    [SP + B_H44] = R5;
    R5 = [FP + -0x24];
    [SP + B_C24] = R5;
    R5 = W[SP + 0x82] (Z);                         /* K4 (0x62 + 32) */
    R5 = PACK (R4.L, R5.L);
    [SP + B_GK4] = R5;
    LSETUP (.Losc_a, .Losc_b) LC0 = P1;            /* P1 = 128 */
.Losc_a:
    R3 = [SP + B_K12];
    R3.L = R3.H * R2.L || R4 = [SP + B_V1];        /* K1 * previous table sample; v1 */
    R3.L = R3.L + R0.H (NS) || R5 = [SP + B_C20];
    R3.L = R3.L >>> 3;
    A1 = R4.L * R5.L (FU);
    R2 = 0x1000 (X);
    A1 = A1 >> 16;
    R3.H = R3.L + R2.L (NS);
    A1 += R4.H * R5.L (M), A0 = R4.H * R5.H || R5 = [SP + B_K12];
    R2 = R3 >> 16;                                 /* the table index */
    A1 += R5.L * R4.L (M) || R5 = [SP + B_C3C] || R4.L = W[I3++];
    P1 = R2;
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    R2 = R5 + R2 (S) || R5 = [SP + B_H44];         /* v1' = C3c + v1 * C20 */
    P1 = P4 + (P1 << 1);
    [SP + B_V1] = R2;
    R4.L = R4.L * R5.L || R2 = [P0++];             /* in16 * C44 */
    R3 = R4 << 16;
    R3 = R3 + R2 (S) || R2 = W[P1] (X);            /* the phase increment; the table sample */
    R4.H = R5.H + R2.L (S);                        /* H1 + t */
    R4.L = R5.H + R4.H (S);                        /* H1 + (H1 + t) */
    R4 = R4 << 16;                                 /* x */
    R5 = -R1 (S);
    A1 = R4.L * R7.L (FU);                         /* x * c */
    A1 = A1 >> 16;
    A1 += R4.H * R7.L (M), A0 = R4.H * R7.H;
    A1 += R7.H * R4.L (M);
    A1 = A1 >>> 15;
    R4 = (A0 += A1);
    A1 = R5.L * R7.L (FU);                         /* -s1 * c */
    A1 = A1 >> 16;
    A1 += R5.H * R7.L (M), A0 = R5.H * R7.H;
    A1 += R7.H * R5.L (M);
    A1 = A1 >>> 15;
    R1 = R1 + R4 (S);
    R5 = (A0 += A1);
    R1 = R1 + R5 (S);                              /* s1' */
    R4 = R1;
    R4.L = 0;                                      /* the quantised state */
    R6 = R6 + R4 (S) || R5 = [SP + B_ST];
    R6 = R6 - R5 (S) || [SP + B_ST] = R4;
    R5 = [SP + B_C24];
    R4 = [SP + B_GK4];
    A1 = R5.L * R6.L (FU);                         /* s2 * C24 (K4 as the high half, as the stock) */
    A1 = A1 >> 16;
    A1 += R4.L * R6.L (M), A0 = R4.L * R6.H;
    A1 += R6.H * R5.L (M);
    A1 = A1 >>> 15;
    R6 = (A0 += A1);
    R5.H = R6.H * R4.H;                            /* out = s2 * G */
.Losc_b:
    R0 = R0 + R3 (NS) || W[I2++] = R5.H;
    R5 = R3;                                       /* the stock loop leaves the increment in R5 */
    [FP + -0x28] = R3;
    R4 = [SP + B_V1];
    [FP + -0x14] = R4;
    R3 = [SP + B_ST];
    [FP + -0x18] = R3;
    SP += 32;
    JUMP.L _stock_dsynth_osc_back;
.Losc_stock:                                       /* the replaced LSETUP by hand (an LSETUP cannot reach the stock
                                                      loop from here), then the stock loop at its own address */
    P2.L = _stock_dsynth_osc_entry; P2.H = _stock_dsynth_osc_entry;
    LT0 = P2;
    P2.L = _stock_dsynth_osc_bottom; P2.H = _stock_dsynth_osc_bottom;
    LB0 = P2;
    LC0 = P1;
    JUMP.L _stock_dsynth_osc_entry;
    .size _coop_dsynth_osc_loop, .-_coop_dsynth_osc_loop
    .text
