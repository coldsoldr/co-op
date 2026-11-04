/* coop-fw: the S3 instrument stage in assembly (performance step 2, 2026-11-04).
 *
 * The same arithmetic as the C path in stereo_bus.cc coop_instr_stage (coop_hb_decimate2, two coop_dc_block, the mid /
 * side split, pair_clear), bit for bit (tests/sim/kern_check.cc compares them):
 *   pass 1, 64 outputs: even[w], odd[w] = pack(sat16 L, sat16 R) of the two input samples; the centre tap
 *     odd[w + 16] * 0.5 starts both accumulators; 32 dual MACs (W32) over the even ring from the oldest slot; out =
 *     (acc >> 16) * ramp (fract16, rounded); ramp = sat16(step + ramp). The rings stay circular in I0 / I1 (B / L)
 *     for the whole block instead of being set up per output; the coefficient table circular in I2 (L2 = 64).
 *   then the 128-sample voice pair is cleared;
 *   pass 2, 64 samples per side: the stock DC blocker y = sat(sat(y - xprev) + x) - c * yq (c * yq the stock 32 x 32
 *     sequence), x = in << 16, yq = y with its low half cleared; the result back into pL / pR (as the C path writes
 *     it); mid = (l + r) >> 1; side = sat16(l - mid), sat16(r - mid).
 * C ABI: R0 = struct coop_stage_io * (offsets below), returns the final ramp (sign-extended). Clobbers what the ABI
 * lets a callee clobber (R0-R3, P0-P2, I/B/M, LC/LT/LB, A0/A1); L0-L3 are zero on return.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#define IO_VL     0      /* int32_t[128] */
#define IO_VR     4
#define IO_HB     8      /* coop_hb2_state: even[32] at +0, odd[32] at +128, w at +256 */
#define IO_COEFS  12     /* int16_t[32] */
#define IO_RAMP   16     /* lo = ramp, hi = step (the stock word at 0xff80107c) */
#define IO_DCL    20     /* coop_dc_state {xprev, yq, y} */
#define IO_DCR    24
#define IO_C      28     /* DC coefficient */
#define IO_MID    32     /* int16_t[64] each */
#define IO_SL     36
#define IO_SR     40
#define IO_PL     44
#define IO_PR     48

    .text
    .global _coop_stage_asm
    .type   _coop_stage_asm, STT_FUNC
_coop_stage_asm:
    [--SP] = (R7:4, P5:3);
    P5 = R0;

    /* ---- pass 1: the half-band decimator, both sides ---- */
    P0 = [P5 + IO_VL];
    P1 = [P5 + IO_VR];
    P2 = [P5 + IO_HB];
    R0 = [P2 + 256];                               /* w */
    R2 = R0 << 2;
    R1 = P2;
    B0 = R1;
    R3 = R1 + R2;
    I0 = R3;                                       /* even[w] */
    L0 = 128 (X);
    R3 = 128 (X);
    R1 = R1 + R3;
    B1 = R1;
    B3 = R1;
    R3 = R1 + R2;
    I1 = R3;                                       /* odd[w] */
    L1 = 128 (X);
    L3 = 128 (X);
    M3 = 60 (X);                                   /* odd[w + 1 + 15] from odd[w + 1] */
    R1 = [P5 + IO_COEFS];
    B2 = R1;
    I2 = R1;
    L2 = 64 (X);
    R4 = [P5 + IO_RAMP];                           /* .L ramp, .H step */
    R7 = 0x4000 (X);                               /* 0.5: the centre tap */
    P3 = [P5 + IO_PL];
    P4 = [P5 + IO_PR];
    P2 = 64;
    LSETUP (.Lst_p1a, .Lst_p1b) LC1 = P2;
.Lst_p1a: R0 = [P0++];
    R1 = [P1++];
    R0 = R0 << 16 (S);
    R1 = R1 << 16 (S);
    R0 = PACK (R1.H, R0.H);
    [I0++] = R0;                                   /* even[w], I0 -> the oldest slot */
    R0 = [P0++];
    R1 = [P1++];
    R0 = R0 << 16 (S);
    R1 = R1 << 16 (S);
    R0 = PACK (R1.H, R0.H);
    [I1++] = R0;                                   /* odd[w] */
    I3 = I1;
    I3 += M3;
    R2 = [I3];
    A0 = R2.L * R7.L, A1 = R2.H * R7.L;            /* centre * 0.5 = sample << 15 */
    P2 = 31;
    MNOP || R3 = [I0++] || R5.L = W[I2++];
    LSETUP (.Lst_mac, .Lst_mac) LC0 = P2;
.Lst_mac: A0 += R3.L * R5.L, A1 += R3.H * R5.L (W32) || R3 = [I0++] || R5.L = W[I2++];
    A0 += R3.L * R5.L, A1 += R3.H * R5.L (W32);
    R0 = A0.W;
    R1 = A1.W;
    R0.L = R0.H * R4.L;
    R1.L = R1.H * R4.L;
    W[P3++] = R0;
    W[P4++] = R1;
.Lst_p1b: R4.L = R4.H + R4.L (S);                        /* ramp = sat16(step + ramp) */

    /* the voice pair cleared for the next block */
    P0 = [P5 + IO_VL];
    P1 = [P5 + IO_VR];
    R2 = 0;
    P2 = 128;
    LSETUP (.Lst_ca, .Lst_cb) LC0 = P2;
.Lst_ca: [P0++] = R2;
.Lst_cb: [P1++] = R2;
    L0 = 0;
    L1 = 0;
    L2 = 0;
    L3 = 0;

    /* ---- pass 2: DC blockers, back into pL / pR, then mid / side ---- */
    [--SP] = R4;                                   /* the ramp, returned */
    P0 = [P5 + IO_DCL];
    P1 = [P5 + IO_DCR];
    R7 = [P5 + IO_C];
    R5 = [P0 + 0];                                 /* xprev L */
    R4 = [P0 + 8];                                 /* y L */
    R0 = [P1 + 0];
    M2 = R0;                                       /* xprev R */
    R6 = [P1 + 8];                                 /* y R */
    P2 = [P5 + IO_PL];
    P3 = [P5 + IO_PR];
    R0 = [P5 + IO_MID];
    I1 = R0;
    R0 = [P5 + IO_SL];
    I2 = R0;
    R0 = [P5 + IO_SR];
    I3 = R0;
    P4 = 64;
    LSETUP (.Lst_p2a, .Lst_p2b) LC0 = P4;
.Lst_p2a: R0 = W[P2++] (X);
    R0 <<= 16;                                     /* x */
    R1 = R4 - R5 (S);
    R1 = R1 + R0 (S);
    R5 = R0;
    R2 = R4;
    R2.L = 0;                                      /* yq */
    A1 = R7.L * R2.L (FU);
    A1 = A1 >> 16;
    A1 += R7.H * R2.L (M), A0 = R7.H * R2.H;
    A1 += R2.H * R7.L (M);
    A1 = A1 >>> 15;
    R3 = (A0 += A1);
    R4 = R1 - R3 (S);                              /* y L */
    R0 = W[P3++] (X);
    R0 <<= 16;
    R3 = M2;
    R1 = R6 - R3 (S);
    R1 = R1 + R0 (S);
    M2 = R0;
    R2 = R6;
    R2.L = 0;
    A1 = R7.L * R2.L (FU);
    A1 = A1 >> 16;
    A1 += R7.H * R2.L (M), A0 = R7.H * R2.H;
    A1 += R2.H * R7.L (M);
    A1 = A1 >>> 15;
    R3 = (A0 += A1);
    R6 = R1 - R3 (S);                              /* y R */
    R0 = R4 >>> 16;                                /* l */
    R1 = R6 >>> 16;                                /* r */
    W[P2 + -2] = R0;
    W[P3 + -2] = R1;
    R2 = R0 + R1;
    R2 >>>= 1;                                     /* mid */
    W[I1++] = R2.L;
    R3.L = R0.L - R2.L (S);
    W[I2++] = R3.L;
    R3.L = R1.L - R2.L (S);
.Lst_p2b: W[I3++] = R3.L;
    [P0 + 0] = R5;
    [P0 + 8] = R4;
    R4.L = 0;
    [P0 + 4] = R4;                                 /* yq = y, low half cleared */
    R0 = M2;
    [P1 + 0] = R0;
    [P1 + 8] = R6;
    R6.L = 0;
    [P1 + 4] = R6;
    R0 = [SP++];
    R0 = R0.L (X);
    (R7:4, P5:3) = [SP++];
    RTS;
    .size _coop_stage_asm, .-_coop_stage_asm
