/*
 * Blackfin kernels for SHIFT: the same arithmetic as ShiftHilbertC and
 * ShiftChannelC (shift.cc), bit for bit (tests/test_shift.cc).
 *
 * Rounding: the MACs' default, unbiased rounding (ties to even), which the
 * C models; RND_MOD (ASTAT bit 8) is cleared on entry and the caller's
 * ASTAT restored on exit. (Setting it, for biased rounding, didn't change
 * the MAC extractions in the simulator.) L registers stay 0. n even, 2..64.
 */

#include "src/shift_layout.h"

#define ROUNDING_ENTER                                                     \
	R0 = ASTAT;                                                        \
	[--SP] = R0;                                                       \
	BITCLR (R0, 8);                                                    \
	ASTAT = R0;

#define ROUNDING_EXIT                                                      \
	R0 = [SP++];                                                       \
	ASTAT = R0;

	.text

/*
 * void ShiftHilbertAsm(ShiftHilbertKernel* k, const int16_t* l,
 *                      const int16_t* r, uint32_t* iq, int32_t* buf,
 *                      int32_t n);
 *
 * buf (shift_layout.h): X with 3 words of history before it, A, B, C, D
 * with 2. The sections run out of place, so each reads its input's
 * history (x[i-2], x[i-1]) from the two words before its input, and its
 * first two stores (its y history) go to the two words before its
 * output, which is the next section's input:
 *   I path: X -> A -> B -> A -> B;   Q path: X - 1 word -> C -> D -> C -> D
 * (X - 1 word: the Q path's z^-1). Then iq from B and D.
 */

/* One section: in, out = word offsets in buf (P3); coefficient coef[j],
 * y history k->y[j]. Pairs of samples: R0/R1 = x[i], x[i+1] (read
 * ahead), R4/R5 = x[i-2], x[i-1], R6/R7 = t, then y; R2.L = a. A0 works
 * the even sample, A1 the odd. P4 = n / 2.
 * A store in parallel with a 32-bit ALU op that writes the same register
 * stores the new value (as the simulator runs it), so each y is stored by
 * a later instruction that doesn't write it. */
#define SECTION(j, in, out)                                                \
	R0 = P3;                                                           \
	R1 = 4 * (in) (Z);                                                 \
	R1 = R1 + R0;                                                      \
	P0 = R1;                                                           \
	R1 += -8;                                                          \
	I1 = R1;                                                           \
	R1 = 4 * (out) - 8 (Z);                                            \
	R1 = R1 + R0;                                                      \
	I0 = R1;                                                           \
	R7 = [P5 + SHK_Y + 8 * (j)];                                       \
	R6 = [P5 + SHK_Y + 8 * (j) + 4];                                   \
	R2 = W[P5 + SHK_COEF + 2 * (j)] (Z);                               \
	R0 = [P0++];                                                       \
	R1 = [P0++];                                                       \
	[I0++] = R6;                                                       \
	LSETUP (.Ls_start##j, .Ls_end##j) LC0 = P4;                        \
.Ls_start##j:                                                              \
	R6 = R0 + R6 (S) || [I0++] = R7 || R4 = [I1++];                               \
	R7 = R1 + R7 (S) || R5 = [I1++];                                  \
	A0 = R6.L * R2.L (FU) || R0 = [P0++];                                \
	A1 = R7.L * R2.L (FU) || R1 = [P0++];                                \
	A0 = A0 >>> 15;                                                    \
	A1 = A1 >>> 15;                                                    \
	R6 = (A0 += R6.H * R2.L);                                          \
	R7 = (A1 += R7.H * R2.L);                                          \
	R6 = R6 - R4 (S);                                                  \
.Ls_end##j:                                                                \
	R7 = R7 - R5 (S) || [I0++] = R6;                                   \
	[I0++] = R7;                                                       \
	[P5 + SHK_Y + 8 * (j)] = R7;                                       \
	[P5 + SHK_Y + 8 * (j) + 4] = R6;

	.align 4
	.global _ShiftHilbertAsm
	.type _ShiftHilbertAsm, STT_FUNC
_ShiftHilbertAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;
	I0 = R1;                        /* l */
	I1 = R2;                        /* r */
	ROUNDING_ENTER
	P3 = [FP + 24];                 /* buf */
	P2 = P3;
	P2 += 4 * SH_X;                 /* X[0] */
	R0 = [P5 + SHK_XH];
	[P2 + -4] = R0;
	R0 = [P5 + SHK_XH + 4];
	[P2 + -8] = R0;
	R0 = [P5 + SHK_XH + 8];
	[P2 + -12] = R0;
	R3 = [FP + 28];                 /* n */
	P4 = R3;
	CC = R2 == 0;
	IF CC JUMP .Lh_mono;
	/* X = l 2^9 + r 2^9: two samples per turn, the next pair read ahead
	 * (the last turn's reads stay inside l and r: n - 2 pairs ahead). */
	R7 = 512 (Z);
	P4 = P4 >> 1;
	P4 += -1;
	R0.L = W[I0++];
	R0.H = W[I1++];
	R1.L = W[I0++];
	R1.H = W[I1++];
	A0 = R0.L * R7.L (IS);
	R2 = (A0 += R0.H * R7.L) (IS);
	CC = P4 == 0;
	IF CC JUMP .Lh_st_last;
	LSETUP (.Lh_st_start, .Lh_st_end) LC0 = P4;
.Lh_st_start:
	A0 = R1.L * R7.L (IS) || [P2++] = R2 || R0.L = W[I0++];
	R2 = (A0 += R1.H * R7.L) (IS) || R0.H = W[I1++];
	A0 = R0.L * R7.L (IS) || [P2++] = R2 || R1.L = W[I0++];
.Lh_st_end:
	R2 = (A0 += R0.H * R7.L) (IS) || R1.H = W[I1++];
.Lh_st_last:
	A0 = R1.L * R7.L (IS) || [P2++] = R2;
	R2 = (A0 += R1.H * R7.L) (IS);
	[P2++] = R2;
	JUMP .Lh_filled;
.Lh_mono:
	/* X = l 2^10; sample i computed while i - 1 is stored and i + 1 read. */
	R7 = 1024 (Z);
	P4 += -2;
	R0.L = W[I0++];
	R2 = (A0 = R0.L * R7.L) (IS) || R0.L = W[I0++];
	CC = P4 == 0;
	IF CC JUMP .Lh_mo_last;
	LSETUP (.Lh_mo_start, .Lh_mo_end) LC0 = P4;
.Lh_mo_start:
.Lh_mo_end:
	R2 = (A0 = R0.L * R7.L) (IS) || [P2++] = R2 || R0.L = W[I0++];
.Lh_mo_last:
	R2 = (A0 = R0.L * R7.L) (IS) || [P2++] = R2;
	[P2++] = R2;
.Lh_filled:
	/* xh = X[n-1], X[n-2], X[n-3] (P2 = X + n) */
	R0 = [P2 + -4];
	[P5 + SHK_XH] = R0;
	R0 = [P2 + -8];
	[P5 + SHK_XH + 4] = R0;
	R0 = [P2 + -12];
	[P5 + SHK_XH + 8] = R0;

	P4 = R3;
	P4 = P4 >> 1;                   /* pairs */
	SECTION(0, SH_X, SH_A)
	SECTION(2, SH_A, SH_B)
	SECTION(4, SH_B, SH_A)
	SECTION(6, SH_A, SH_B)
	SECTION(1, SH_X - 1, SH_C)
	SECTION(3, SH_C, SH_D)
	SECTION(5, SH_D, SH_C)
	SECTION(7, SH_C, SH_D)

	/* iq from B (I) and D (Q): round16, Q x m unless m = 32768 steady. */
	P0 = [FP + 20];                 /* iq */
	R1 = P3;
	R0 = 4 * SH_B (Z);
	R0 = R0 + R1;
	P1 = R0;
	R0 = 4 * SH_D (Z);
	R0 = R0 + R1;
	P2 = R0;
	R6 = [P5 + SHK_M];
	R4 = [P5 + SHK_DM];
	R0 = 32768 (Z);
	CC = R6 == R0;
	IF !CC JUMP .Lh_ramp;
	CC = R4 == 0;
	IF !CC JUMP .Lh_ramp;
	P4 = R3;
	P4 += -1;
	R0 = [P1++];
	R1 = [P2++];
	R0 = R0 << 6 (S);
	R1 = R1 << 6 (S);
	R2.L = R0 (RND) || R0 = [P1++];
	R2.H = R1 (RND) || R1 = [P2++];
	LSETUP (.Lh_cv_start, .Lh_cv_end) LC0 = P4;
.Lh_cv_start:
	R0 = R0 << 6 (S) || [P0++] = R2;
	R1 = R1 << 6 (S);
	R2.L = R0 (RND) || R0 = [P1++];
.Lh_cv_end:
	R2.H = R1 (RND) || R1 = [P2++];
	[P0++] = R2;
	JUMP .Lh_done;
.Lh_ramp:
	P4 = R3;
	R5 = 32767 (Z);
	LSETUP (.Lh_cr_start, .Lh_cr_end) LC0 = P4;
.Lh_cr_start:
	R0 = [P1++];
	R1 = [P2++];
	R0 = R0 << 6 (S);
	R1 = R1 << 6 (S);
	R2.L = R0 (RND);
	R1.L = R1 (RND);
	R3 = MIN (R6, R5);
	R2.H = R1.L * R3.L (T);
	R6 = R6 + R4;
.Lh_cr_end:
	[P0++] = R2;
	[P5 + SHK_M] = R6;
.Lh_done:

	ROUNDING_EXIT
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ShiftHilbertAsm, .-_ShiftHilbertAsm

/*
 * void ShiftChannelAsm(ShiftChannelKernel* k, const uint32_t* iq,
 *                      int16_t* io, int32_t n, int32_t write,
 *                      uint32_t* scratch);
 *
 * 1. the oscillator: scratch[i] = sin | cos << 16;
 * 2. the loop, in one or two runs (where the delay index wraps):
 *    feedback, rotation, low-pass, delay write; scratch[i] = w;
 * 3. if write: the mix into io.
 */

/* Pass 2, one sample. P1 -> delay word, R0 = its contents (read ahead),
 * I0 -> iq, I2 -> scratch (sin | cos << 16 in, w out); R4 = lp,
 * R5 = K | K2 << 16, R6.H = g, R7 = dg << 16 | -32767. The next delay word
 * is read ahead (each run's prologue reads its first). */
#define LOOP_SAMPLE(end)                                                   \
	R1 = R0 >>> 15 (V) || R3 = [I0++];                                 \
	A1 = R0.H * R6.H, A0 = R0.L * R6.H || R0 = [P1 + 4];               \
	R2.H = (A1 += R1.H * R7.L), R2.L = (A0 += R1.L * R7.L) (T) || R1 = [I2]; \
	R2 = R2 +|+ R3 (S);                                                \
	A1 = R2.L * R1.L, A0 = R2.L * R1.H;                                \
	R2.H = (A1 += R2.H * R1.H), R2.L = (A0 -= R2.H * R1.L);            \
	A1 = R2.H * R5.L, A0 = R2.L * R5.L || [I2++] = R2;                 \
	R4.H = (A1 += R4.H * R5.H), R4.L = (A0 += R4.L * R5.H);            \
end:                                                                       \
	R6 = R6 +|+ R7 || [P1++] = R4;

	.align 4
	.global _ShiftChannelAsm
	.type _ShiftChannelAsm, STT_FUNC
_ShiftChannelAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;
	I0 = R1;                        /* iq */
	I1 = R2;                        /* io (written) */
	P4 = R2;                        /* io (read): kept in M0 */
	M0 = P4;
	ROUNDING_ENTER

	/* 1. Oscillator. R7 = phase, R6 = inc, R5 = dinc, R4.L = the
	 * EXTRACT pattern for frac (bits 8..22), P3 -> kShiftOsc. One sample
	 * ahead: in the loop, P2 -> entry i, R1 = frac i, R7 = phase i + 1,
	 * R6 = inc i + 1. */
	R0 = [FP + 28];                 /* scratch */
	I2 = R0;
	P4 = [FP + 20];                 /* n */
	P3 = [P5 + SCK_OSC];
	R7 = [P5 + SCK_PHASE];
	R6 = [P5 + SCK_INC];
	R5 = [P5 + SCK_DINC];
	R4 = 0x080f (Z);
	R0 = R7 >> 23;
	R1 = EXTRACT (R7, R4.L) (Z);
	P2 = R0;
	R7 = R7 + R6 (NS);
	R6 = R6 + R5 (NS);
	P2 = P3 + (P2 << 2);
	LSETUP (.Lo_start, .Lo_end) LC0 = P4;
.Lo_start:
	R0 = R7 >> 23 || R2 = [P2];
	R3 = [P2 + 2048];
	P2 = R0;
	R3.H = (A1 = R3.H * R1.L), R3.L = (A0 = R3.L * R1.L) (T);
	R1 = EXTRACT (R7, R4.L) (Z);
	R2 = R2 +|+ R3;
	R7 = R7 + R6 (NS) || [I2++] = R2;
	P2 = P3 + (P2 << 2);
.Lo_end:
	R6 = R6 + R5 (NS);
	/* One step back: inc n = R6 - dinc, phase n = R7 - inc n. */
	R6 = R6 - R5 (NS);
	R7 = R7 - R6 (NS);
	[P5 + SCK_PHASE] = R7;
	[P5 + SCK_INC] = R6;

	/* 2. The loop. P0 = pos (bytes), P2 -> delay. */
	R0 = [FP + 28];
	I2 = R0;
	P0 = [P5 + SCK_POS];
	P2 = P5;
	P2 += SCK_DELAY;
	R4 = [P5 + SCK_LP];
	R5.L = SH_LP_K;
	R5.H = SH_LP_K2;
	R6 = [P5 + SCK_G];
	R6 <<= 16;
	R7 = [P5 + SCK_DG];
	R7 <<= 16;
	R0 = -32767 (X);
	R7 = PACK (R7.H, R0.L);
	/* First run: min(n, (1024 - pos) / 4) samples. */
	R0 = P0;
	R1 = 4 * SH_DELAY (Z);
	R1 = R1 - R0;
	R1 >>= 2;                       /* room before the wrap */
	R2 = [FP + 20];                 /* n */
	R3 = R2;
	R3 = MIN (R3, R1);
	R2 = R2 - R3;
	P3 = R2;                        /* the second run */
	P4 = R3;
	P1 = P2 + P0;
	R0 = [P1];
	LSETUP (.Ll1_start, .Ll1_end) LC0 = P4;
.Ll1_start:
	LOOP_SAMPLE(.Ll1_end)
	CC = P3 == 0;
	IF CC JUMP .Ll_done;
	P4 = P3;
	P1 = P2;
	R0 = [P1];
	LSETUP (.Ll2_start, .Ll2_end) LC0 = P4;
.Ll2_start:
	LOOP_SAMPLE(.Ll2_end)
.Ll_done:
	/* pos = (P1 - delay) & 1023 */
	R0 = P1;
	R1 = P2;
	R0 = R0 - R1;
	R1 = 4 * SH_DELAY - 1 (Z);
	R0 = R0 & R1;
	[P5 + SCK_POS] = R0;
	[P5 + SCK_LP] = R4;
	R6 >>>= 16;
	[P5 + SCK_G] = R6;

	/* 3. Mix. R7 = 32767 - mix | mix << 16, R6 = -dmix | dmix << 16;
	 * P4 reads the dry signal one sample ahead, I1 writes. */
	R0 = [P5 + SCK_MIX];
	R1 = [P5 + SCK_DMIX];
	R2 = [FP + 20];                 /* n */
	R3 = R1;
	R3 *= R2;
	R3 = R3 + R0;
	[P5 + SCK_MIX] = R3;
	R3 = [FP + 24];                 /* write */
	CC = R3 == 0;
	IF CC JUMP .Lm_done;
	R3 = 32767 (Z);
	R3 = R3 - R0;
	R7 = PACK (R0.L, R3.L);
	R3 = 0;
	R3 = R3 - R1;
	R6 = PACK (R1.L, R3.L);
	R0 = [FP + 28];
	I2 = R0;
	P4 = M0;
	R2 += -1;
	P3 = R2;
	R0 = W[P4++] (X);
	R1 = [I2++];
	LSETUP (.Lm_start, .Lm_end) LC0 = P3;
.Lm_start:
	A0 = R0.L * R7.L || R0 = W[P4++] (X);
	R3.L = (A0 += R1.L * R7.H) (T) || R1 = [I2++];
.Lm_end:
	R7 = R7 +|+ R6 || W[I1++] = R3.L;
	A0 = R0.L * R7.L;
	R3.L = (A0 += R1.L * R7.H) (T);
	W[I1++] = R3.L;
.Lm_done:

	ROUNDING_EXIT
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ShiftChannelAsm, .-_ShiftChannelAsm
