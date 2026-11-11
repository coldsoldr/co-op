/*
 * Blackfin kernels for DRIVE's passes: the same arithmetic as the C
 * references in drive_dsp.cc (bit-exact; tests/host/drive checks them in the
 * GNU simulator). From te-op1-cfw ports/fx/src/drive/drive_bfin.S (commit
 * 9ced415), unchanged but for the fused mono wet kernel, cut (coop runs every
 * position stereo).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware
 * permission; the halfband's coefficients are Clouds' (MIT, ../LICENSE-MIT).
 * Both channels go through each pass at once, packed in the halves of a
 * register (channel 0 low, channel 1 high): vector adds, dual multiplies
 * and dual MACs. The 16-bit multiplies' and MACs' default rounding
 * (unbiased) is what the C models; RND_MOD is cleared on entry and
 * restored on exit, so the caller's ASTAT doesn't matter. L registers stay
 * 0. n > 0.
 *
 * Loads from the caller's arrays (in0, in1, dry0, dry1) never read past
 * their n samples: loops that load a step ahead run n - 1 times and finish
 * with a copy of the body without those loads. Loads from DRIVE's own
 * buffers may read one word past n (the buffers have a spare word).
 */

#define PROLOGUE                                                           \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	P5 = ASTAT;                                                        \
	[--SP] = P5;

/* Clears RND_MOD (ASTAT bit 8); uses R3. */
#define ROUNDING_CLEAR                                                     \
	R3 = ASTAT;                                                        \
	BITCLR (R3, 8);                                                    \
	ASTAT = R3;

#define EPILOGUE                                                           \
	P5 = [SP++];                                                       \
	ASTAT = P5;                                                        \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

/* Clouds' halfband: R5 = 11 | -151 << 16, R6 = 710 | -2397 << 16,
 * R7 = 10019 | 16384 << 16 (the centre). */
#define TAPS                                                               \
	R5.L = 11;                                                         \
	R5.H = -151;                                                       \
	R6.L = 710;                                                        \
	R6.H = -2397;                                                      \
	R7.L = 10019;                                                      \
	R7.H = 16384;

	.text

/*
 * void DriveInterpAsm(DriveInterpKernel* k, const int16_t* in0,
 *                     const int16_t* in1, int32_t* xh, int32_t* ys,
 *                     int32_t n);
 *
 * Output m: even = sum of the pairs (w[j], w[9 - j]) x tap (w = xh + m),
 * odd = w[5] x 0.5, both + beta (saturated).
 * I0 -> w going up, I1 -> w[8] going down, I2 -> in0, I3 -> in1, P0 ->
 * xh[9 + m] (the new word), P1 -> ys, P2 -> k; R3 = beta, R4 = dbeta;
 * R0, R1 the window; R2 the new word. On entry to a step R0 = w[0], R2 =
 * the new word and I0 -> w[1].
 */
#define INTERP_STEP(next_lo, next_hi)                                                 \
	A1 = R0.H * R5.L, A0 = R0.L * R5.L || R0 = [I0++] || R1 = [I1--];    /* w0; w1, w8 */  \
	A1 += R2.H * R5.L, A0 += R2.L * R5.L || [P0++] = R2 || NOP;          /* w9 */          \
	A1 += R0.H * R5.H, A0 += R0.L * R5.H || R0 = [I0++] || next_lo;      /* w1; w2 */      \
	A1 += R1.H * R5.H, A0 += R1.L * R5.H || R1 = [I1--] || next_hi;      /* w8; w7 */      \
	A1 += R0.H * R6.L, A0 += R0.L * R6.L || R0 = [I0++] || NOP;          /* w2; w3 */      \
	A1 += R1.H * R6.L, A0 += R1.L * R6.L || R1 = [I1--] || NOP;          /* w7; w6 */      \
	A1 += R0.H * R6.H, A0 += R0.L * R6.H || R0 = [I0 ++ M0] || NOP;      /* w3; w4 */      \
	A1 += R1.H * R6.H, A0 += R1.L * R6.H || R1 = [I1 ++ M1] || NOP;      /* w6; w5 */      \
	A1 += R0.H * R7.L, A0 += R0.L * R7.L;                                /* w4 */          \
	R0.H = (A1 += R1.H * R7.L), R0.L = (A0 += R1.L * R7.L);              /* w5: even */    \
	R1.H = R1.H * R7.H, R1.L = R1.L * R7.H;                              /* odd: w5 / 2 */ \
	R0 = R0 +|+ R3 (S);                                                                    \
	R1 = R1 +|+ R3 (S) || [P1++] = R0 || NOP;
#define INTERP_LAST(next_w0) R3 = R3 +|+ R4 || [P1++] = R1 || next_w0;

	.align 4
	.global _DriveInterpAsm
	.type _DriveInterpAsm, STT_FUNC
_DriveInterpAsm:
	PROLOGUE
	P2 = R0;
	I2 = R1;
	I3 = R2;
	ROUNDING_CLEAR
	R0 = [FP + 20];                 /* xh */
	I0 = R0;
	P0 = R0;
	P0 += 36;                       /* xh + 9 */
	R0 += 32;
	I1 = R0;                        /* xh + 8 */
	P1 = [FP + 24];                 /* ys */
	P3 = [FP + 28];                 /* n */
	TAPS
	R3 = [P2];                      /* beta */
	R4 = [P2 + 4];                  /* dbeta */
	M0 = -12 (X);                   /* I0: w[4] -> the next w[0] (w[1]) */
	M1 = 16 (X);                    /* I1: w[5] -> the next w[8] (w[9]) */
	R2.L = W[I2++];
	R2.H = W[I3++];
	R0 = [I0++];
	P3 += -1;
	CC = P3 == 0;
	IF CC JUMP .Li_last;

	LSETUP (.Li_start, .Li_end) LC0 = P3;
.Li_start:
	INTERP_STEP(R2.L = W[I2++], R2.H = W[I3++])
.Li_end:
	INTERP_LAST(R0 = [I0++])
.Li_last:
	INTERP_STEP(NOP, NOP)
	INTERP_LAST(NOP)

	[P2] = R3;
	EPILOGUE
	.size _DriveInterpAsm, .-_DriveInterpAsm

/*
 * void DriveShapeAsm(DriveShapeKernel* k, int32_t* y, int32_t words);
 *
 * Per word: v = x * gain; u = sat(v << shift); t = u^2;
 * y = u (A - t (B - C t)).
 * I0 -> y (reads, one word ahead), P0 -> y (writes), P2 -> k; R3 = gain,
 * R4 = dgain, R5 = shift | C << 16, R6 = A | A, R7 = B | B.
 */
	.align 4
	.global _DriveShapeAsm
	.type _DriveShapeAsm, STT_FUNC
_DriveShapeAsm:
	PROLOGUE
	P2 = R0;
	I0 = R1;
	P0 = R1;
	P3 = R2;
	ROUNDING_CLEAR
	R3 = [P2];                      /* gain */
	R4 = [P2 + 4];                  /* dgain */
	R5 = [P2 + 8];                  /* shift */
	R5.H = 6144;                    /* C = 3/16 */
	R6.L = 30720;                   /* A = 15/16 */
	R6.H = 30720;
	R7.L = 20480;                   /* B = 10/16 */
	R7.H = 20480;
	R0 = [I0++];

	LSETUP (.Ls_start, .Ls_end) LC0 = P3;
.Ls_start:
	R0.H = R0.H * R3.H, R0.L = R0.L * R3.L;      /* v */
	R0 = ASHIFT R0 BY R5.L (V, S);               /* u */
	R1.H = R0.H * R0.H, R1.L = R0.L * R0.L;      /* t */
	R2.H = R1.H * R5.H, R2.L = R1.L * R5.H;      /* C t */
	R2 = R7 -|- R2 (S);                          /* B - C t */
	R2.H = R1.H * R2.H, R2.L = R1.L * R2.L;      /* t (B - C t) */
	R2 = R6 -|- R2 (S);                          /* A - t (B - C t) */
	R2.H = R0.H * R2.H, R2.L = R0.L * R2.L || R0 = [I0++] || NOP;   /* y; the next x */
.Ls_end:
	R3 = R3 +|+ R4 || [P0++] = R2 || NOP;

	[P2] = R3;
	EPILOGUE
	.size _DriveShapeAsm, .-_DriveShapeAsm

/*
 * void DriveDecimAsm(const int32_t* ys, int32_t* out, int32_t n);
 *
 * Output k (w = ys + 2k), packed: w[9] x 0.5 + the pairs (w[8 - 2i],
 * w[10 + 2i]) x tap i.
 * I0 -> w[0] going up (2 words a step), I1 -> w[18] going down, P0 -> the
 * next centre (P2 = 8: its step), P1 -> out; R2 = the centre (loaded a
 * step ahead).
 */
	.align 4
	.global _DriveDecimAsm
	.type _DriveDecimAsm, STT_FUNC
_DriveDecimAsm:
	PROLOGUE
	P0 = R0;
	P0 += 36;                       /* w + 9 */
	P2 = 8;
	I0 = R0;
	R0 += 36;
	R0 += 36;
	I1 = R0;                        /* w + 18 */
	P1 = R1;
	P3 = R2;
	ROUNDING_CLEAR
	TAPS
	M0 = 8 (X);
	M1 = -8 (X);
	M2 = -24 (X);                   /* I0: w[8] -> the next w[0] (w[2]) */
	M3 = 40 (X);                    /* I1: w[10] -> the next w[18] (w[20]) */
	R2 = [P0 ++ P2];                /* w[9] */

	LSETUP (.Ld_start, .Ld_end) LC0 = P3;
.Ld_start:
	A1 = R2.H * R7.H, A0 = R2.L * R7.H || R0 = [I0 ++ M0] || R1 = [I1 ++ M1];   /* centre; w0, w18 */
	A1 += R0.H * R5.L, A0 += R0.L * R5.L || R0 = [I0 ++ M0] || NOP;            /* w0; w2 */
	A1 += R1.H * R5.L, A0 += R1.L * R5.L || R1 = [I1 ++ M1] || NOP;            /* w18; w16 */
	A1 += R0.H * R5.H, A0 += R0.L * R5.H || R0 = [I0 ++ M0] || NOP;            /* w2; w4 */
	A1 += R1.H * R5.H, A0 += R1.L * R5.H || R1 = [I1 ++ M1] || NOP;            /* w16; w14 */
	A1 += R0.H * R6.L, A0 += R0.L * R6.L || R0 = [I0 ++ M0] || NOP;            /* w4; w6 */
	A1 += R1.H * R6.L, A0 += R1.L * R6.L || R1 = [I1 ++ M1] || NOP;            /* w14; w12 */
	A1 += R0.H * R6.H, A0 += R0.L * R6.H || R0 = [I0 ++ M2] || NOP;            /* w6; w8 */
	A1 += R1.H * R6.H, A0 += R1.L * R6.H || R1 = [I1 ++ M3] || NOP;            /* w12; w10 */
	A1 += R0.H * R7.L, A0 += R0.L * R7.L || R2 = [P0 ++ P2] || NOP;            /* w8; the next centre */
	R3.H = (A1 += R1.H * R7.L), R3.L = (A0 += R1.L * R7.L);                    /* w10 */
.Ld_end:
	MNOP || [P1++] = R3 || NOP;

	EPILOGUE
	.size _DriveDecimAsm, .-_DriveDecimAsm

/*
 * void DriveToneAsm(DriveToneKernel* k, int32_t* yw, int32_t n);
 *
 * In place: x = sat(y - fb); lp = sat(lp + (sat(x - lp) * k));
 * w = (x Hm + lp Dm) (S2RND).
 * I0 -> y (one word ahead), P0 -> w, P2 -> k (dtone at +16); R3 = fb,
 * R4 = lp, R5 = tone (Hm | Dm << 16), R6 = dfb, R7.L = the low-pass's k.
 */
	.align 4
	.global _DriveToneAsm
	.type _DriveToneAsm, STT_FUNC
_DriveToneAsm:
	PROLOGUE
	P2 = R0;
	I0 = R1;
	P0 = R1;
	P3 = R2;
	ROUNDING_CLEAR
	R3 = [P2];                      /* fb */
	R6 = [P2 + 4];                  /* dfb */
	R4 = [P2 + 8];                  /* lp */
	R5 = [P2 + 12];                 /* tone */
	R7 = 4351 (X);                  /* kToneK */
	R0 = [I0++];

	LSETUP (.Lt_start, .Lt_end) LC0 = P3;
.Lt_start:
	R0 = R0 -|- R3 (S);                                                /* x */
	R1 = R0 -|- R4 (S);
	R1.H = R1.H * R7.L, R1.L = R1.L * R7.L;
	R4 = R4 +|+ R1 (S) || R1 = [P2 + 16] || NOP;                       /* lp; dtone */
	A1 = R0.H * R5.L, A0 = R0.L * R5.L || R0 = [I0++] || NOP;          /* x Hm; the next y */
	R2.H = (A1 += R4.H * R5.H), R2.L = (A0 += R4.L * R5.H) (S2RND);
	R5 = R5 +|+ R1 || [P0++] = R2 || NOP;
.Lt_end:
	R3 = R3 +|+ R6;

	[P2] = R3;
	[P2 + 8] = R4;
	[P2 + 12] = R5;
	EPILOGUE
	.size _DriveToneAsm, .-_DriveToneAsm

/*
 * void DriveDcAsm(DriveDcKernel* k, int32_t* w, int32_t n);
 *
 * x = sat(w - round(acc >> 16)); acc += x * k (A0 / A1 hold acc).
 * I0 -> w (reads), P0 -> w (writes), P2 -> k; R7.L = k.
 */
	.align 4
	.global _DriveDcAsm
	.type _DriveDcAsm, STT_FUNC
_DriveDcAsm:
	PROLOGUE
	P2 = R0;
	I0 = R1;
	P0 = R1;
	P3 = R2;
	ROUNDING_CLEAR
	R0 = [P2];
	A0 = R0;
	R1 = [P2 + 4];
	A1 = R1;
	R7 = 47 (X);                    /* kDcK */

	LSETUP (.Lc_start, .Lc_end) LC0 = P3;
.Lc_start:
	R1.H = A1, R1.L = A0 || R0 = [I0++] || NOP;
	R0 = R0 -|- R1 (S);
.Lc_end:
	A1 += R0.H * R7.L, A0 += R0.L * R7.L || [P0++] = R0 || NOP;

	R0 = A0.W;
	[P2] = R0;
	R1 = A1.W;
	[P2 + 4] = R1;
	EPILOGUE
	.size _DriveDcAsm, .-_DriveDcAsm

/*
 * void DriveMixAsm(DriveMixKernel* k, const int32_t* w, const int16_t* dry0,
 *                  const int16_t* dry1, int16_t* out0, int16_t* out1,
 *                  int32_t n);
 *
 * out = (dry gd + w gw) (S2RND): gd, gw Q14.
 * P0 -> w, I0 -> dry0, I1 -> dry1, I2 -> out0, I3 -> out1, P2 -> k;
 * R7 = mix (gd | gw << 16), R6 = dmix; R1 = the dry (loaded a step ahead).
 */
#define MIX_STEP(next_lo)                                                    \
	A1 = R1.H * R7.L, A0 = R1.L * R7.L || R0 = [P0++] || NOP;         /* dry gd; w */ \
	R2.H = (A1 += R0.H * R7.H), R2.L = (A0 += R0.L * R7.H) (S2RND);                   \
	R7 = R7 +|+ R6 || W[I2++] = R2.L || next_lo;
#define MIX_LAST(next_hi) MNOP || W[I3++] = R2.H || next_hi;

	.align 4
	.global _DriveMixAsm
	.type _DriveMixAsm, STT_FUNC
_DriveMixAsm:
	PROLOGUE
	P2 = R0;
	P0 = R1;
	I0 = R2;
	ROUNDING_CLEAR
	R0 = [FP + 20];
	I1 = R0;                        /* dry1 */
	R0 = [FP + 24];
	I2 = R0;                        /* out0 */
	R0 = [FP + 28];
	I3 = R0;                        /* out1 */
	P3 = [FP + 32];                 /* n */
	R7 = [P2];
	R6 = [P2 + 4];
	R1.L = W[I0++];
	R1.H = W[I1++];
	P3 += -1;
	CC = P3 == 0;
	IF CC JUMP .Lm_last;

	LSETUP (.Lm_start, .Lm_end) LC0 = P3;
.Lm_start:
	MIX_STEP(R1.L = W[I0++])
.Lm_end:
	MIX_LAST(R1.H = W[I1++])
.Lm_last:
	MIX_STEP(NOP)
	MIX_LAST(NOP)

	[P2] = R7;
	EPILOGUE
	.size _DriveMixAsm, .-_DriveMixAsm
