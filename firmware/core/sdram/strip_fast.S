/* coop-fw channel strip CS2: the Blackfin kernels of the per-tape channel strip (docs/channel-strip.md "CS2").
 *
 * Each kernel is bit for bit the strip_bfc_* C of core/include/coop_strip_bf.h (tests/sim/run_strip_check.sh
 * compares them on the bfin simulator over random and edge blocks, with guard words around every array). Frames
 * are packed stereo words (L low half, R high half); every array is 4-byte aligned, 64 frames / samples.
 * Products are the MAC's truncating forms only (no dependence on ASTAT RND_MOD); the compressor's 32 x 32 products
 * are GCC's own __builtin_bfin_mult_fr1x32x32 sequence (coop_mul_fr).
 * Input streams are read through circular buffers (B / L registers) so the last iteration's look-ahead load stays
 * inside its array; L0-L3 are zero again on return. Stores: only the named outputs. Many loops are rotated (a pass
 * finishes item n and starts item n + 1, whose first packet stores item n) so that a store never shares a packet
 * with the instruction that writes its register.
 * Packets: one 32-bit DSP instruction beside I-register loads and / or one D-register store (an I-register store
 * may pair with an I-register load; a P-register store has slot 3 NOP); P-register loads (table lookups) never
 * beside a shift; no slot loads a register another slot of the packet reads or writes.
 * tools/asm_lint.py: clean. Comments stay on their own lines (the assembler eats code inside a multi-line comment).
 * C ABI: arguments R0 / R1 / R2, then the stack; R4-R7 and P3-P5 are saved where used.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text

/* ---------------------------------------------------------------------------------------------------------------
 * int32_t strip_bfa_pack(const int16_t *L, const int16_t *R, uint32_t *w)
 * w[i] = R[i] << 16 | L[i]; returns the peak |x| of both (ABS saturates: |-32768| = 32767). Two frames a pass. */
    .global _strip_bfa_pack
    .type   _strip_bfa_pack, STT_FUNC
    .align 4
_strip_bfa_pack:
    [--SP] = (R7:4);
    I0 = R0;
    B0 = R0;
    I1 = R1;
    B1 = R1;
    R3 = 128;
    L0 = R3;
    L1 = R3;
    I2 = R2;
    R4 = 0;
    R0 = [I0++];
    R1 = [I1++];
    P0 = 32;
    LSETUP (.Lpk_a, .Lpk_b) LC0 = P0;
.Lpk_a: R2 = PACK(R1.L, R0.L);
    R3 = PACK(R1.H, R0.H) || [I2++] = R2 || NOP;
    R5 = ABS R0 (V) || [I2++] = R3 || NOP;
    R6 = ABS R1 (V) || R0 = [I0++] || NOP;
    R4 = MAX(R4, R5) (V) || R1 = [I1++] || NOP;
.Lpk_b: R4 = MAX(R4, R6) (V);
    L0 = 0;
    L1 = 0;
    R0 = R4.L (Z);
    R1 = R4 >> 16;
    R0 = MAX(R0, R1);
    (R7:4) = [SP++];
    RTS;
    .size _strip_bfa_pack, .-_strip_bfa_pack

/* ---------------------------------------------------------------------------------------------------------------
 * int32_t strip_bfa_unpack(const uint32_t *w, int16_t *L, int16_t *R)
 * L[i] = w[i] low, R[i] = w[i] high; returns the peak. Frames i (I0) and i + 1 (I1) a pass, stride 8. */
    .global _strip_bfa_unpack
    .type   _strip_bfa_unpack, STT_FUNC
    .align 4
_strip_bfa_unpack:
    [--SP] = (R7:4);
    I0 = R0;
    B0 = R0;
    B1 = R0;
    R3 = 4;
    R3 = R3 + R0;
    I1 = R3;
    R3 = 256 (X);
    L0 = R3;
    L1 = R3;
    M0 = 8;
    I2 = R1;
    I3 = R2;
    R4 = 0;
    MNOP || R0 = [I0 ++ M0] || R1 = [I1 ++ M0];
    P0 = 32;
    LSETUP (.Lup_a, .Lup_b) LC0 = P0;
.Lup_a: R2 = PACK(R1.L, R0.L);
    R3 = PACK(R1.H, R0.H) || [I2++] = R2 || NOP;
    R5 = ABS R2 (V) || [I3++] = R3 || NOP;
    R6 = ABS R3 (V) || R0 = [I0 ++ M0] || R1 = [I1 ++ M0];
    R4 = MAX(R4, R5) (V);
.Lup_b: R4 = MAX(R4, R6) (V);
    L0 = 0;
    L1 = 0;
    R0 = R4.L (Z);
    R1 = R4 >> 16;
    R0 = MAX(R0, R1);
    (R7:4) = [SP++];
    RTS;
    .size _strip_bfa_unpack, .-_strip_bfa_unpack

/* ---------------------------------------------------------------------------------------------------------------
 * int32_t strip_bfa_peak(const int16_t *L, const int16_t *R): the peak |x| of both channels. */
    .global _strip_bfa_peak
    .type   _strip_bfa_peak, STT_FUNC
    .align 4
_strip_bfa_peak:
    [--SP] = R4;
    I0 = R0;
    B0 = R0;
    I1 = R1;
    B1 = R1;
    R3 = 128;
    L0 = R3;
    L1 = R3;
    R4 = 0;
    R0 = [I0++];
    R1 = [I1++];
    P0 = 32;
    LSETUP (.Lpe_a, .Lpe_b) LC0 = P0;
.Lpe_a: R2 = ABS R0 (V);
    R3 = ABS R1 (V) || R0 = [I0++] || NOP;
    R4 = MAX(R4, R2) (V) || R1 = [I1++] || NOP;
.Lpe_b: R4 = MAX(R4, R3) (V);
    L0 = 0;
    L1 = 0;
    R0 = R4.L (Z);
    R1 = R4 >> 16;
    R0 = MAX(R0, R1);
    R4 = [SP++];
    RTS;
    .size _strip_bfa_peak, .-_strip_bfa_peak

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_comp(uint32_t *w, strip_bf_comp *c): the compressor over the block (strip_bfc_comp), four
 * sub-blocks of 16 frames. c: thr 0, slope5 4, knee 8, inv2k 12, makeup 16, att 20, rel 24, gr 28, cg 32,
 * log2 table 36, exp2 table 40. MULFR is coop_mul_fr on the Blackfin (__builtin_bfin_mult_fr1x32x32's sequence:
 * (a b) >> 31 exactly, saturating only (-1)(-1)). Few branches (a taken one costs more than the arithmetic): the
 * log2 of a zero peak falls out of the normal path (SIGNBITS 0 = 31, table[0] = 0: -32.0), the knee is computed
 * out of line only inside the knee (|2 over| < knee), exp2's cases are a clamped shift and one conditional move.
 * Registers: P5 = c, P4 = the sub-block, R7 = gr; LC1 counts the sub-blocks; I0 / I2 read the block through a
 * circular buffer (B0 = B2 = w, 256 bytes), I1 writes. */
#define MULFR(d, a, b)                                                     \
    A1 = a.L * b.L (FU);                                                   \
    A1 = A1 >> 16;                                                         \
    A1 += a.H * b.L (M), A0 = a.H * b.H;                                   \
    A1 += b.H * a.L (M);                                                   \
    A1 = A1 >>> 15;                                                        \
    d = (A0 += A1);

    .global _strip_bfa_comp
    .type   _strip_bfa_comp, STT_FUNC
    .align 4
_strip_bfa_comp:
    [--SP] = (R7:4, P5:4);
    P4 = R0;
    P5 = R1;
    B0 = R0;
    B2 = R0;
    R3 = 256 (X);
    L0 = R3;
    L2 = R3;
    R7 = [P5 + 28];
    P0 = 4;
    LSETUP (.Lco_sub, .Lco_end) LC1 = P0;
/* the peak of the sub-block */
.Lco_sub: I0 = P4;
    R1 = 0;
    R0 = [I0++];
    P0 = 16;
    LSETUP (.Lco_pa, .Lco_pb) LC0 = P0;
.Lco_pa: R2 = ABS R0 (V);
.Lco_pb: R1 = MAX(R1, R2) (V) || R0 = [I0++] || NOP;
    R0 = R1.L (Z);
    R1 = R1 >> 16;
    R0 = MAX(R0, R1);
/* lvl = strip_log2(pk << 16) -> R6: sb = SIGNBITS, m = x << sb, lg = T[m >> 22 & 255] + lerp (m >> 14 & 255) */
    R0 <<= 16;
    R1.L = SIGNBITS R0;
    R1 = R1.L (X);
    R2 = LSHIFT R0 BY R1.L;
    R3 = R2 >> 22;
    R3 = R3.B (Z);
    R4 = R2 >> 14;
    R4 = R4.B (Z);
    P0 = R3;
    P1 = [P5 + 36];
    P0 = P1 + (P0 << 2);
    R5 = [P0];
    R3 = [P0 + 4];
    R3 = R3 - R5;
    R3 = R3.L * R4.L (IS);
    R3 >>>= 8;
    R5 = R5 + R3;
    R1 = -R1;
    R1 += -1;
    R1 <<= 16;
    R6 = R1 + R5;
/* the target reduction -> R6: over > 0 ? over x slope : 0, or the knee when |2 over| < knee */
    R0 = [P5];
    R0 = R6 - R0;
    R1 = [P5 + 8];
    R3 = [P5 + 4];
    R2 = R0 << 1;
    R2 = ABS R2;
    CC = R2 < R1;
    IF CC JUMP .Lco_knee;
    R2 = 0;
    R0 = MAX(R0, R2);
    R0 <<= 10;
    MULFR(R6, R0, R3)
/* gr += MULFR(target - gr, target > gr ? att : rel) */
.Lco_gr:
    R1 = [P5 + 20];
    R2 = [P5 + 24];
    CC = R7 < R6;
    IF !CC R1 = R2;
    R0 = R6 - R7;
    MULFR(R0, R0, R1)
    R7 = R7 + R0;
/* g1 = strip_bf_exp2(makeup - gr) -> R4: T[g >> 8 & 255] + lerp (g & 255) (Q30), >> clamp(4 - n, 0, 31), n = g >> 16 */
    R0 = [P5 + 16];
    R0 = R0 - R7;
    R1 = R0 >>> 16;
    R2 = R0 >> 8;
    R2 = R2.B (Z);
    R3 = R0.B (Z);
    P0 = R2;
    P1 = [P5 + 40];
    P0 = P1 + (P0 << 2);
    R4 = [P0];
    R5 = [P0 + 4];
    R5 = R5 - R4;
    R5 *= R3;
    R5 >>>= 8;
    R4 = R4 + R5;
    R2 = 4;
    R2 = R2 - R1;
    R3 = 0;
    R2 = MAX(R2, R3);
    R3 = 31;
    R2 = MIN(R2, R3);
    R2 = -R2;
    R4 = ASHIFT R4 BY R2.L;
/* the mantissas m1 = g1 >> 11, m0 = cg >> 11, sh = max(0, 16 - SIGNBITS max), step = (m1 - m0) / 16 */
    R5 = [P5 + 32];
    [P5 + 32] = R4;
    R0 = R4 >>> 11;
    R1 = R5 >>> 11;
    R2 = MAX(R0, R1);
    R3.L = SIGNBITS R2;
    R3 = R3.L (X);
    R6 = 16;
    R3 = R6 - R3;
    R6 = 0;
    R3 = MAX(R3, R6);
    R6 = -R3;
    R0 = ASHIFT R0 BY R6.L;
    R1 = ASHIFT R1 BY R6.L;
    R2 = R0 - R1;
    R6 = 15;
    R6 = R2 + R6;
    CC = R2 < 0;
    IF CC R2 = R6;
    R2 >>>= 4;
    R6 = R2 << 4;
    R6 = R6 - R2;
    R0 = R0 - R6;
    R0 = PACK(R0.L, R0.L);
    R2 = PACK(R2.L, R2.L);
/* apply: 16 frames, x m_i >> 15 (truncating), << sh (S) */
    I1 = P4;
    I2 = P4;
    R1 = [I2++];
    P0 = 16;
    CC = R3 == 0;
    IF CC JUMP .Lco_ns (BP);
    LSETUP (.Lco_aa, .Lco_ab) LC0 = P0;
.Lco_aa: R4.H = R1.H * R0.H, R4.L = R1.L * R0.L (T);
    R4 = ASHIFT R4 BY R3.L (V, S) || R1 = [I2++] || NOP;
.Lco_ab: R0 = R0 +|+ R2 || [I1++] = R4 || NOP;
    JUMP .Lco_nx;
.Lco_ns:
    LSETUP (.Lco_ac, .Lco_ad) LC0 = P0;
.Lco_ac: R4.H = R1.H * R0.H, R4.L = R1.L * R0.L (T);
.Lco_ad: R0 = R0 +|+ R2 || [I1++] = R4 || R1 = [I2++];
.Lco_nx:
    P4 += 32;
.Lco_end: P4 += 32;
    [P5 + 28] = R7;
    L0 = 0;
    L2 = 0;
    (R7:4, P5:4) = [SP++];
    RTS;
/* the knee (out of line): d = over + knee / 2; MULFR(MULFR(MULFR(d << 14, d << 14), inv2k) << 10, slope5) */
.Lco_knee:
    R2 = R1 >>> 1;
    R0 = R0 + R2;
    R0 <<= 14;
    MULFR(R2, R0, R0)
    R4 = [P5 + 12];
    MULFR(R2, R2, R4)
    R2 <<= 10;
    MULFR(R6, R2, R3)
    JUMP .Lco_gr;
    .size _strip_bfa_comp, .-_strip_bfa_comp

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_shape(const uint32_t *x, uint32_t *y, const strip_bf_satk *k)
 * per channel: p = x drive (Q26); q = sat32(p << 3) (Q29, the +-4 clamp); idx = q >> 23, fr = (q >> 8) & 0x7fff;
 * y = sat16(t[idx] + (sat16(t[idx + 1] - t[idx]) fr >> 15)); y = y dcomp >> 15. The loop is rotated: each pass
 * ends with the next frame's multiply and stores the frame it finished.
 * k: tab 0, drive 4, dcomp 8. Registers: P5 table centre, R7.L drive, R6 dcomp pair, R5.L the EXTRACT pattern. */
    .global _strip_bfa_shape
    .type   _strip_bfa_shape, STT_FUNC
    .align 4
_strip_bfa_shape:
    [--SP] = (R7:4, P5:5);
    P2 = R2;
    I0 = R0;
    B0 = R0;
    I1 = R1;
    R3 = 256 (X);
    L0 = R3;
    P5 = [P2];
    R7 = [P2 + 4];
    R6 = [P2 + 8];
    R6 = PACK(R6.L, R6.L);
/* EXTRACT pattern: position 8 in bits 12..8, length 15 in bits 4..0 */
    R5 = 0x080f (Z);
    R0 = [I0++];
    R1 = R0.H * R7.L, R0 = R0.L * R7.L (IS);
    P0 = 64;
    LSETUP (.Lsh_a, .Lsh_b) LC0 = P0;
.Lsh_a: R0 = R0 << 3 (S);
    R1 = R1 << 3 (S);
    R2 = R0 >>> 23;
    R3 = R1 >>> 23;
    P0 = R2;
    P1 = R3;
    P0 = P5 + (P0 << 1);
    P1 = P5 + (P1 << 1);
    R2 = EXTRACT(R0, R5.L) (Z) || R4 = W[P0] (X) || NOP;
    R3 = EXTRACT(R1, R5.L) (Z) || R0 = W[P1] (X) || NOP;
    R2 = PACK(R3.L, R2.L) || R1 = W[P0 + 2] (X) || NOP;
    R4 = PACK(R0.L, R4.L) || R3 = W[P1 + 2] (X) || NOP;
    R1 = PACK(R3.L, R1.L);
    R1 = R1 -|- R4 (S);
    R1.H = R1.H * R2.H, R1.L = R1.L * R2.L (T);
    R1 = R4 +|+ R1 (S) || R0 = [I0++] || NOP;
    R4.H = R1.H * R6.H, R4.L = R1.L * R6.L (T);
.Lsh_b: R1 = R0.H * R7.L, R0 = R0.L * R7.L (IS) || [I1++] = R4 || NOP;
    L0 = 0;
    (R7:4, P5:5) = [SP++];
    RTS;
    .size _strip_bfa_shape, .-_strip_bfa_shape

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_fold(const uint32_t *x, uint32_t *y, const strip_bf_satk *k)
 * per channel: p = x drive; s = (p << 4) + 2^30 (wrapping: the period of 4); m = |s| - 2^30 (ABS saturates);
 * y = (m << 1).H = m >> 15 (the triangle, Q15); y = y dcomp >> 15. Rotated as shape. R5 = 2^30. */
    .global _strip_bfa_fold
    .type   _strip_bfa_fold, STT_FUNC
    .align 4
_strip_bfa_fold:
    [--SP] = (R7:4);
    P2 = R2;
    I0 = R0;
    B0 = R0;
    I1 = R1;
    R3 = 256 (X);
    L0 = R3;
    R7 = [P2 + 4];
    R6 = [P2 + 8];
    R6 = PACK(R6.L, R6.L);
    R5.H = 0x4000;
    R5.L = 0;
    R0 = [I0++];
    R1 = R0.H * R7.L, R0 = R0.L * R7.L (IS);
    P0 = 64;
    LSETUP (.Lfo_a, .Lfo_b) LC0 = P0;
.Lfo_a: R0 = R0 << 4;
    R1 = R1 << 4;
    R0 = R0 + R5 (NS);
    R1 = R1 + R5 (NS);
    R0 = ABS R0;
    R1 = ABS R1;
    R0 = R0 - R5 (NS);
    R1 = R1 - R5 (NS);
    R0 = R0 << 1;
    R1 = R1 << 1;
    R2 = PACK(R1.H, R0.H);
    R4.H = R2.H * R6.H, R4.L = R2.L * R6.L (T) || R0 = [I0++] || NOP;
.Lfo_b: R1 = R0.H * R7.L, R0 = R0.L * R7.L (IS) || [I1++] = R4 || NOP;
    L0 = 0;
    (R7:4) = [SP++];
    RTS;
    .size _strip_bfa_fold, .-_strip_bfa_fold

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_tube(uint32_t *y, strip_bf_satk *k)
 * per channel: z = sat32((v << 16) - dc); dc += z >> 10; y = z >> 16. k: dc[0] 24, dc[1] 28. */
    .global _strip_bfa_tube
    .type   _strip_bfa_tube, STT_FUNC
    .align 4
_strip_bfa_tube:
    [--SP] = (R7:4);
    P2 = R1;
    I0 = R0;
    B0 = R0;
    I1 = R0;
    R3 = 256 (X);
    L0 = R3;
    R5 = [P2 + 24];
    R6 = [P2 + 28];
    R7.H = 0xffff;
    R7.L = 0;
    R0 = [I0++];
    P0 = 64;
    LSETUP (.Ltu_a, .Ltu_b) LC0 = P0;
.Ltu_a: R1 = R0 << 16;
    R2 = R0 & R7;
    R1 = R1 - R5 (S);
    R2 = R2 - R6 (S) || R0 = [I0++] || NOP;
    R4 = PACK(R2.H, R1.H);
    R3 = R1 >>> 10 || [I1++] = R4 || NOP;
    R5 = R5 + R3 (NS);
    R3 = R2 >>> 10;
.Ltu_b: R6 = R6 + R3 (NS);
    L0 = 0;
    [P2 + 24] = R5;
    [P2 + 28] = R6;
    (R7:4) = [SP++];
    RTS;
    .size _strip_bfa_tube, .-_strip_bfa_tube

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_tone(uint32_t *y, strip_bf_satk *k)
 * both channels: d = sat16(y - lp); lp = sat16(lp + (d c >> 15)); d2 = sat16(y - lp); y = sat16(y + (d2 k >> 15)),
 * c = 6291 (Q15). k: tone 12 (packed), lp 20 (packed). */
    .global _strip_bfa_tone
    .type   _strip_bfa_tone, STT_FUNC
    .align 4
_strip_bfa_tone:
    [--SP] = (R7:5);
    P2 = R1;
    I0 = R0;
    B0 = R0;
    I1 = R0;
    R3 = 256 (X);
    L0 = R3;
    R7 = [P2 + 12];
    R5 = [P2 + 20];
    R6 = 6291 (X);
    R6 = PACK(R6.L, R6.L);
/* two frames a pass, y in R0 (even) and R3 (odd); rotated by one packet: the pass ends with the next even
 * frame's first step, which stores the odd frame just finished */
    R0 = [I0++];
    R1 = R0 -|- R5 (S) || R3 = [I0++] || NOP;
    P0 = 32;
    LSETUP (.Lto_a, .Lto_b) LC0 = P0;
.Lto_a: R1.H = R1.H * R6.H, R1.L = R1.L * R6.L (T);
    R5 = R5 +|+ R1 (S);
    R1 = R0 -|- R5 (S);
    R1.H = R1.H * R7.H, R1.L = R1.L * R7.L (T);
    R2 = R0 +|+ R1 (S);
    R1 = R3 -|- R5 (S) || [I1++] = R2 || R0 = [I0++];
    R1.H = R1.H * R6.H, R1.L = R1.L * R6.L (T);
    R5 = R5 +|+ R1 (S);
    R1 = R3 -|- R5 (S);
    R1.H = R1.H * R7.H, R1.L = R1.L * R7.L (T);
    R2 = R3 +|+ R1 (S);
.Lto_b: R1 = R0 -|- R5 (S) || [I1++] = R2 || R3 = [I0++];
    L0 = 0;
    [P2 + 20] = R5;
    (R7:5) = [SP++];
    RTS;
    .size _strip_bfa_tone, .-_strip_bfa_tone

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_mix(uint32_t *x, const uint32_t *y, const strip_bf_satk *k)
 * x = (x (32768 - m) + y m) >> 15 (fractional MACs, truncating extraction). k: mix 16 (low m, high 32768 - m). */
    .global _strip_bfa_mix
    .type   _strip_bfa_mix, STT_FUNC
    .align 4
_strip_bfa_mix:
    P2 = R2;
    I0 = R0;
    B0 = R0;
    I1 = R1;
    B1 = R1;
    I2 = R0;
    R3 = 256 (X);
    L0 = R3;
    L1 = R3;
    R3 = [P2 + 16];
/* rotated by one packet: a pass finishes frame n and starts frame n + 1, whose first packet stores frame n */
    R0 = [I0++];
    A1 = R0.H * R3.H, A0 = R0.L * R3.H || R1 = [I1++] || NOP;
    P0 = 64;
    LSETUP (.Lmx_a, .Lmx_b) LC0 = P0;
.Lmx_a: R2.H = (A1 += R1.H * R3.L), R2.L = (A0 += R1.L * R3.L) (T) || R0 = [I0++] || NOP;
.Lmx_b: A1 = R0.H * R3.H, A0 = R0.L * R3.H || [I2++] = R2 || R1 = [I1++];
    L0 = 0;
    L1 = 0;
    RTS;
    .size _strip_bfa_mix, .-_strip_bfa_mix

/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_bus_n(const strip_bf_job *j, int32_t n): for each job { bus, x, g0, g1 } (16 bytes), n > 0:
 * g0 = g1 = 0: nothing; g0 = g1 = 32768: bus[i] = sat16(bus[i] + x[i]); else g = min(g, 32767), step =
 * (g1 - g0) / 64 toward zero, bus[i] = sat16(bus[i] + (x[i] g_i >> 15)), g_i = g1 - (63 - i) step.
 * Samples i, i + 1 a pass; x and the bus read ahead through circular buffers (B0 / B1, 128 bytes). A constant
 * gain (step 0, the fader still) is the fall-through path, two cycles a pass (rotated by one packet: a pass
 * finishes pair n and starts pair n + 1, whose first packet stores pair n); a ramp is three; skip and unity are
 * out of line. */
    .global _strip_bfa_bus_n
    .type   _strip_bfa_bus_n, STT_FUNC
    .align 4
_strip_bfa_bus_n:
    [--SP] = (R7:5, P5:5);
    P5 = R0;
    P2 = R1;
    R3 = 128;
    L0 = R3;
    L1 = R3;
    R5.H = 0;
    R5.L = 0x8000;
.Lbn_job:
    R2 = [P5 + 8];
    R3 = [P5 + 12];
    R0 = R2 | R3;
    CC = R0 == 0;
    IF CC JUMP .Lbn_next;
    R0 = R2 ^ R5;
    R1 = R3 ^ R5;
    R0 = R0 | R1;
    CC = R0 == 0;
    IF CC JUMP .Lbn_unity;
    R0 = [P5];
    R1 = [P5 + 4];
    I1 = R0;
    B1 = R0;
    I2 = R0;
    I0 = R1;
    B0 = R1;
/* step = (g1' - g0') / 64, g = g1' - 63 step */
    R6 = 32767 (X);
    R2 = MIN(R2, R6);
    R3 = MIN(R3, R6);
    R0 = R3 - R2;
    R1 = 63;
    R1 = R0 + R1;
    CC = R0 < 0;
    IF CC R0 = R1;
    R0 >>>= 6;
    P0 = 32;
    CC = R0 == 0;
    IF !CC JUMP .Lbn_ramp;
/* constant: R7 = (g, g) */
    R7 = PACK(R3.L, R3.L);
    R0 = [I0++];
    R2.H = R0.H * R7.H, R2.L = R0.L * R7.L (T) || R1 = [I1++] || NOP;
    LSETUP (.Lbn_e, .Lbn_f) LC0 = P0;
.Lbn_e: R3 = R2 +|+ R1 (S) || R0 = [I0++] || NOP;
.Lbn_f: R2.H = R0.H * R7.H, R2.L = R0.L * R7.L (T) || [I2++] = R3 || R1 = [I1++];
.Lbn_next:
    P5 += 16;
    P2 += -1;
    CC = P2 == 0;
    IF !CC JUMP .Lbn_job (BP);
    L0 = 0;
    L1 = 0;
    (R7:5, P5:5) = [SP++];
    RTS;
/* a ramp: R7 = (g + step, g), R6 = (2 step, 2 step) */
.Lbn_ramp:
    R1 = R0 << 6;
    R1 = R1 - R0;
    R2 = R3 - R1;
    R6 = R2 + R0;
    R7 = PACK(R6.L, R2.L);
    R6 = R0 << 1;
    R6 = PACK(R6.L, R6.L);
    R0 = [I0++];
    LSETUP (.Lbn_a, .Lbn_b) LC0 = P0;
.Lbn_a: R2.H = R0.H * R7.H, R2.L = R0.L * R7.L (T) || R1 = [I1++] || NOP;
    R2 = R2 +|+ R1 (S) || R0 = [I0++] || NOP;
.Lbn_b: R7 = R7 +|+ R6 || [I2++] = R2 || NOP;
    P5 += 16;
    P2 += -1;
    CC = P2 == 0;
    IF !CC JUMP .Lbn_job (BP);
    L0 = 0;
    L1 = 0;
    (R7:5, P5:5) = [SP++];
    RTS;
.Lbn_unity:
    R0 = [P5];
    R1 = [P5 + 4];
    I1 = R0;
    B1 = R0;
    I2 = R0;
    I0 = R1;
    B0 = R1;
    MNOP || R0 = [I0++] || R1 = [I1++];
    P0 = 32;
    LSETUP (.Lbn_c, .Lbn_d) LC0 = P0;
.Lbn_c: R2 = R0 +|+ R1 (S);
    MNOP || R0 = [I0++] || R1 = [I1++];
.Lbn_d: MNOP || [I2++] = R2 || NOP;
    P5 += 16;
    P2 += -1;
    CC = P2 == 0;
    IF !CC JUMP .Lbn_job (BP);
    L0 = 0;
    L1 = 0;
    (R7:5, P5:5) = [SP++];
    RTS;
    .size _strip_bfa_bus_n, .-_strip_bfa_bus_n


/* ---------------------------------------------------------------------------------------------------------------
 * void strip_bfa_eq(const strip_bf_eqjob *j, int32_t n, uint32_t *e): n > 0 EQ bands, each job { in, out, coef,
 * st } (the C model strip_bfc_eq_band has the arithmetic). Per sample, both channels in A0 (L) / A1 (R):
 *   G0  (integer):    2E1 L1 + 2E2 L2;  >>> 15
 *   G15: (integer) 2E1 H1 + 2E2 H2, (fractional) Y1 L1 + Y2 L2 + x2 Lb2 + x1 Lb1 + x0 Lb0, (integer) r x 1,
 *        (fractional) 16384 x 16384 = 2^29 (the rounding)
 *   r = A.W & 0x7fff (the remainder the next shift drops, fed back);  >>> 15
 *   G30 (fractional): Y1 H1 + Y2 H2 + x2 Hb2 + x1 Hb1 + x0 Hb0 -> v (32-bit)
 *   w = v << 1 (S): Y = w.H (stored in out: the output and the Y history), 2E = w.L ^ 0x8000 (stored in e)
 * 29 cycles a sample. Registers: R4 = C0 (-a1), R5 = C1 (-a2) (H << 16 | L), R7 = r, R2 = 2E1 at a sample's start.
 * The loop is rotated by two packets: a pass ends with the next sample's G0, whose second packet stores this
 * sample's 2E. I0 walks the ring coef[6..14] = C2, K (16384 : 1), C3, C4, mask, C2, C3, C4, xor (circular);
 * I1 in[n..n+2]; I2 out[n..n+1] (Y); I3 e[n]; P0 stores Y at out[n + 2], P1 2E at e[n + 2]; P3 in, P4 out,
 * P5 coef; LC1 counts the bands. The job pointer and e live on the stack ([SP], [SP + 8]).
 * k > 0 (coef[5]): once the state is saved, out[2..65] = sat16(sat16(Y << k) + ((E + 2^(14 - k)) >> (15 - k)))
 * (coef[15] = 2^(14 - k) pair, coef[16] = k - 15, coef[17] = k), out of line. */
    .global _strip_bfa_eq
    .type   _strip_bfa_eq, STT_FUNC
    .align 4
_strip_bfa_eq:
    [--SP] = (R7:4, P5:3);
    SP += -12;
    [SP + 8] = R2;
    R3 = 36;
    L0 = R3;
    M1 = -8;
    M2 = -4;
    LC1 = R1;
    LSETUP (.Leq_job, .Leq_end) LC1;
.Leq_job:
    [SP] = R0;
    P1 = R0;
    P3 = [P1];
    P4 = [P1 + 4];
    P5 = [P1 + 8];
    P1 = [P1 + 12];
    P2 = R2;
/* history: in[0..1] = st[0..1], out[0..1] = st[2..3], e[0..1] = st[4..5] */
    R0 = [P1];
    R1 = [P1 + 4];
    [P3] = R0;
    [P3 + 4] = R1;
    R0 = [P1 + 8];
    R1 = [P1 + 12];
    [P4] = R0;
    [P4 + 4] = R1;
    R0 = [P1 + 16];
    R2 = [P1 + 20];
    [P2] = R0;
    [P2 + 4] = R2;
    R7 = [P1 + 24];
    R4 = [P5];
    R5 = [P5 + 4];
/* streams */
    I1 = P3;
    I2 = P4;
    I3 = P2;
    R0 = P5;
    R1 = 24;
    R0 = R0 + R1;
    I0 = R0;
    B0 = R0;
/* stores at [2]: Y -> P0 (out), 2E -> P1 (e) */
    P1 = P2;
    P1 += 8;
    P0 = P4;
    P0 += 8;
    R0 = 64;
    LC0 = R0;
/* prologue: the first two packets of sample 0 (R2 = 2E[1] already) */
    A1 = R2.H * R4.L, A0 = R2.L * R4.L (IS) || R3 = [I3++] || R1 = [I2++];
    A1 += R3.H * R5.L, A0 += R3.L * R5.L (IS);
    LSETUP (.Leq_a, .Leq_b) LC0;
.Leq_a: A0 = A0 >>> 15 || R0 = [I1++] || R6 = [I0++];
    A1 = A1 >>> 15;
    A1 += R2.H * R4.H, A0 += R2.L * R4.H (IS);
    A1 += R3.H * R5.H, A0 += R3.L * R5.H (IS) || R2 = [I2 ++ M2] || NOP;
    A1 += R1.H * R5.L, A0 += R1.L * R5.L || R3 = [I0++] || NOP;
    A1 += R2.H * R4.L, A0 += R2.L * R4.L || R1 = [I1++] || NOP;
    A1 += R0.H * R6.L, A0 += R0.L * R6.L || R2 = [I0++] || NOP;
    A1 += R1.H * R2.L, A0 += R1.L * R2.L || R0 = [I1 ++ M1] || R6 = [I0++];
    A1 += R0.H * R6.L, A0 += R0.L * R6.L || R2 = [I0++] || NOP;
    A1 += R7.H * R3.L, A0 += R7.L * R3.L (IS);
    A1 += R3.H * R3.H, A0 += R3.H * R3.H;
    R0 = A0.W;
    R1 = A1.W;
    R0 = PACK(R1.L, R0.L) || R3 = [I2++] || R6 = [I1++];
    R7 = R0 & R2;
    A0 = A0 >>> 15 || R2 = [I2] || R0 = [I0++];
    A1 = A1 >>> 15 || R1 = [I1++] || NOP;
    A1 += R2.H * R4.H, A0 += R2.L * R4.H;
    A1 += R3.H * R5.H, A0 += R3.L * R5.H || R2 = [I0++] || NOP;
    A1 += R6.H * R0.H, A0 += R6.L * R0.H || R3 = [I1 ++ M2] || NOP;
    A1 += R1.H * R2.H, A0 += R1.L * R2.H || R0 = [I0++] || NOP;
    R1 = (A1 += R3.H * R0.H), R0 = (A0 += R3.L * R0.H);
    R0 = R0 << 1 (S) || R2 = [I0++] || NOP;
    R1 = R1 << 1 (S);
    R6 = PACK(R1.H, R0.H);
    R3 = PACK(R1.L, R0.L) || [P0++] = R6 || NOP;
    R2 = R3 ^ R2;
/* the next sample's G0; its second packet stores this sample's 2E */
    A1 = R2.H * R4.L, A0 = R2.L * R4.L (IS) || R3 = [I3++] || R1 = [I2++];
.Leq_b: A1 += R3.H * R5.L, A0 += R3.L * R5.L (IS) || [P1++] = R2 || NOP;
/* state out: st[0..1] = in[64..65], st[2..3] = out[64..65], st[4..5] = e[64..65], st[6] = r */
    P1 = [SP];
    P1 = [P1 + 12];
    R0 = [P3 + 0x100];
    R1 = [P3 + 0x104];
    [P1] = R0;
    [P1 + 4] = R1;
    R0 = [P4 + 0x100];
    R1 = [P4 + 0x104];
    [P1 + 8] = R0;
    [P1 + 12] = R1;
    R0 = [P2 + 0x100];
    R1 = [P2 + 0x104];
    [P1 + 16] = R0;
    [P1 + 20] = R1;
    [P1 + 24] = R7;
    R0 = [P5 + 20];
    CC = R0 == 0;
    IF !CC JUMP .Leq_fix;
.Leq_next:
    R0 = [SP];
    R0 += 16;
.Leq_end: R2 = [SP + 8];
    L0 = 0;
    SP += 12;
    (R7:4, P5:3) = [SP++];
    RTS;
/* k > 0: out = sat16(sat16(Y << k) + ((2E >>> 1) + 2^(14 - k)) >>> (15 - k)), in place */
.Leq_fix:
    R5 = [P5 + 60];
    R6 = [P5 + 64];
    R7 = [P5 + 68];
    R0 = P4;
    R0 += 8;
    I2 = R0;
    I1 = R0;
    B2 = R0;
    R0 = P2;
    R0 += 8;
    I3 = R0;
    B3 = R0;
    R0 = 256 (X);
    L2 = R0;
    L3 = R0;
    MNOP || R0 = [I2++] || R1 = [I3++];
    R3 = R1 >>> 1 (V);
    R3 = R3 +|+ R5 || R1 = [I3++] || NOP;
    R3 = ASHIFT R3 BY R6.L (V);
    R2 = ASHIFT R0 BY R7.L (V, S);
    P0 = 64;
/* rotated: a pass finishes sample n, stores it and starts sample n + 1 */
    LSETUP (.Leq_c, .Leq_d) LC0 = P0;
.Leq_c: R2 = R2 +|+ R3 (S) || R0 = [I2++] || NOP;
    R3 = R1 >>> 1 (V) || [I1++] = R2 || NOP;
    R3 = R3 +|+ R5 || R1 = [I3++] || NOP;
    R3 = ASHIFT R3 BY R6.L (V);
.Leq_d: R2 = ASHIFT R0 BY R7.L (V, S);
    L2 = 0;
    L3 = 0;
    JUMP .Leq_next;
    .size _strip_bfa_eq, .-_strip_bfa_eq
