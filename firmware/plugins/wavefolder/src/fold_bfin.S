/*
 * Blackfin kernels for FOLD: the same arithmetic as FoldShapeRunC,
 * FoldToneRunC and FoldToneStereoRunC (fold.cc; bit-exact, checked by
 * tests/test_fold.cc). The fractional multiplies and the S2RND extraction
 * use the MAC's default unbiased rounding, which FoldMulFrac and S2Rnd
 * model; RND_MOD is cleared on entry and restored on exit. n > 0 and even.
 * L registers stay 0.
 *
 * Ramps (I += M) that are read before they're stepped in the loop are
 * started one step on, so each sample still uses "step, then value", as
 * the C does.
 */

#include "src/fold_layout.h"

#define FCH_SIZE 12        /* sizeof(FoldChannel) */

#define PROLOGUE                                                           \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	R3 = ASTAT;                                                        \
	[--SP] = R3;                                                       \
	BITCLR (R3, 8);                                                    \
	ASTAT = R3;

#define EPILOGUE                                                           \
	R3 = [SP++];                                                       \
	ASTAT = R3;                                                        \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

/*
 * One sample of the folder. UN = the new u's register, UP = u' (the
 * previous u). R4 = the fraction pattern, R5 = the sine's segment
 * pattern; I1 = g (step M1), I2 = c (M2), both one step on; P0 -> x, P1 =
 * sin table, P2 = sinc table, P3/P4 the segments' addresses; R1-R3, R6
 * temporaries; the result goes to [I0] after I0 steps by the stride (M0).
 */
#define FOLD_SAMPLE(UN, UP)                                                \
	UN = W[P0++] (X);                                                  \
	R1 = I1;                                                           \
	UN = UN.L * R1.H (IS);          /* x G, Q27 */                     \
	R1 = I2;                                                           \
	UN = UN + R1 (NS) || I1 += M1;               /* u */               \
	R1 = UN + UP, R2 = UN - UP || I0 += M0;      /* u + u', u - u' */  \
	R3 = EXTRACT (R1, R5.L) (Z);    /* sine segment */                 \
	P3 = R3;                                                           \
	R2 = ABS R2 || I2 += M2;                                           \
	R3 = R2 >> 20;                  /* sinc segment */                 \
	P4 = R3;                                                           \
	P3 = P1 + (P3 << 2);                                               \
	P4 = P2 + (P4 << 2);                                               \
	R1 = EXTRACT (R1, R4.L) (Z) || R3 = [P3];    /* sine fraction */   \
	R2 = EXTRACT (R2, R4.L) (Z) || R6 = [P4];    /* sinc fraction */   \
	R1.L = R3.H * R1.L;                                                \
	R3.L = R3.L + R1.L (NS);        /* sin, Q15 */                     \
	R2.L = R6.H * R2.L;                                                \
	R6.L = R6.L + R2.L (NS);        /* sinc, Q14 */                    \
	R3.L = R3.L * R6.L;             /* folded, Q14 */
/* ... then W[I0] = R3.L (outside the macro, so the loop's end label can sit
 * on the last store). */

	.text

/*
 * void FoldShapeRunAsm(const FoldShapeBlock* k, FoldChannel* ch,
 *                      const int16_t* x, int16_t* w, int32_t stride,
 *                      int32_t n);
 *
 * Two samples per pass: u alternates between R0 and R7.
 */
	.align 4
	.global _FoldShapeRunAsm
	.type _FoldShapeRunAsm, STT_FUNC
_FoldShapeRunAsm:
	PROLOGUE
	P3 = R0;                        /* k */
	P5 = R1;                        /* ch */
	P0 = R2;                        /* x */
	R1 = [FP + 24];                 /* stride, samples */
	R1 <<= 1;
	M0 = R1;
	R0 = [FP + 20];                 /* w, one stride back */
	R0 = R0 - R1;
	I0 = R0;
	R0 = [FP + 28];                 /* n */
	R0 >>= 1;
	P4 = R0;
	R0 = [P3 + FSB_G];
	R1 = [P3 + FSB_GSTEP];
	M1 = R1;
	R0 = R0 + R1;
	I1 = R0;
	R0 = [P3 + FSB_C];
	R1 = [P3 + FSB_CSTEP];
	M2 = R1;
	R0 = R0 + R1;
	I2 = R0;
	P1 = [P3 + FSB_SIN];
	P2 = [P3 + FSB_SINC];
	R7 = [P5 + FCH_UPREV];
	R4 = FOLD_PAT_FRAC (Z);
	R5 = FOLD_PAT_SINIDX (Z);

	LSETUP (.Ls_start, .Ls_end) LC0 = P4;
.Ls_start:
	FOLD_SAMPLE(R0, R7)
	W[I0] = R3.L;
	FOLD_SAMPLE(R7, R0)
.Ls_end:
	W[I0] = R3.L;

	[P5 + FCH_UPREV] = R7;
	EPILOGUE
	.size _FoldShapeRunAsm, .-_FoldShapeRunAsm

/*
 * void FoldToneRunAsm(const FoldToneBlock* k, FoldChannel* ch,
 *                     const int16_t* w, int16_t* io, int32_t n);
 *
 * P0 -> w, I3 -> io (dry), P1 -> io (out); I1 = sc (M1, one step on), I2
 * = mix (M2); R4 = t1 | t2 << 16, R6 = z, R7 = tone coefficient | DC
 * coefficient << 16; R0-R3 temporaries, A0 the mix.
 */
	.align 4
	.global _FoldToneRunAsm
	.type _FoldToneRunAsm, STT_FUNC
_FoldToneRunAsm:
	PROLOGUE
	P3 = R0;                        /* k */
	P5 = R1;                        /* ch */
	P0 = R2;                        /* w */
	P1 = [FP + 20];                 /* io */
	I3 = P1;
	P4 = [FP + 24];                 /* n */
	R0 = [P3 + FTB_SC];
	R1 = [P3 + FTB_SCSTEP];
	M1 = R1;
	R0 = R0 + R1;
	I1 = R0;
	R0 = [P3 + FTB_MIX];
	I2 = R0;
	R0 = [P3 + FTB_MIXSTEP];
	M2 = R0;
	R7 = [P3 + FTB_COEF];
	R4 = [P5 + FCH_T1];             /* t1 | t2 << 16 */
	R6 = [P5 + FCH_Z];

	LSETUP (.Lt_start, .Lt_end) LC0 = P4;
.Lt_start:
	R0 = W[P0++] (X);               /* w, Q14 */
	R2 = I1;
	R0.L = R0.L - R2.H (NS) || R3.L = W[I3++];  /* w - sc; dry */
	R1.L = R0.L - R4.L (NS) || I1 += M1;
	R1.L = R1.L * R7.L || I2 += M2;
	R4.L = R4.L + R1.L (NS);        /* t1 */
	R1.L = R4.L - R4.H (NS);
	R1.L = R1.L * R7.L;
	R4.H = R4.H + R1.L (NS);        /* t2 */
	R1.L = R4.H - R6.H (S);         /* e = t2 - z */
	R2 = R1.L * R7.H;               /* 2 e kdc */
	R6 = R6 + R2 (NS);              /* z */
	R2 = I2;
	A0 = R3.L * R2.L (IS);          /* dry (32767 - mix) */
	R3.L = (A0 += R1.L * R2.H) (S2RND);   /* + 2 e lm, >> 15 */
.Lt_end:
	W[P1++] = R3;

	[P5 + FCH_T1] = R4;
	[P5 + FCH_Z] = R6;
	EPILOGUE
	.size _FoldToneRunAsm, .-_FoldToneRunAsm

/*
 * void FoldToneStereoRunAsm(const FoldToneBlock* k, FoldChannel* ch,
 *                           const int16_t* w, int16_t* l, int16_t* r,
 *                           int32_t n);
 *
 * Both channels at once, left in the low halves, right in the high: P0 ->
 * w (pairs), I2 -> l, I3 -> r; I1 = sc (M1, one step on), I0 = mix (M0);
 * R4 = t1 pair, R5 = t2 pair, R6 = z left, R7 = z right, R3 = the
 * coefficients; R0-R2 temporaries, A0/A1 the mix.
 */
	.align 4
	.global _FoldToneStereoRunAsm
	.type _FoldToneStereoRunAsm, STT_FUNC
_FoldToneStereoRunAsm:
	PROLOGUE
	P3 = R0;                        /* k */
	P5 = R1;                        /* ch */
	P0 = R2;                        /* w */
	R0 = [FP + 20];                 /* l */
	I2 = R0;
	R0 = [FP + 24];                 /* r */
	I3 = R0;
	P4 = [FP + 28];                 /* n */
	R0 = [P3 + FTB_SC];
	R1 = [P3 + FTB_SCSTEP];
	M1 = R1;
	R0 = R0 + R1;
	I1 = R0;
	R0 = [P3 + FTB_MIX];
	I0 = R0;
	R0 = [P3 + FTB_MIXSTEP];
	M0 = R0;
	R0 = W[P5 + FCH_T1] (Z);
	R1 = W[P5 + FCH_SIZE + FCH_T1] (Z);
	R4 = PACK (R1.L, R0.L);
	R0 = W[P5 + FCH_T2] (Z);
	R1 = W[P5 + FCH_SIZE + FCH_T2] (Z);
	R5 = PACK (R1.L, R0.L);
	R6 = [P5 + FCH_Z];
	R7 = [P5 + FCH_SIZE + FCH_Z];
	R3 = [P3 + FTB_COEF];

	LSETUP (.Lp_start, .Lp_end) LC0 = P4;
.Lp_start:
	R0 = [P0++];                    /* w pair */
	R1 = I1;
	R1 = PACK (R1.H, R1.H) || I1 += M1 || R2.L = W[I2];   /* sc | sc; dry left */
	R0 = R0 -|- R1 || R2.H = W[I3];                       /* w - sc; dry right */
	R0 = R0 -|- R4 || I0 += M0;
	R1.H = R0.H * R3.L, R1.L = R0.L * R3.L;
	R4 = R4 +|+ R1;                 /* t1 */
	R0 = R4 -|- R5;
	R1.H = R0.H * R3.L, R1.L = R0.L * R3.L;
	R5 = R5 +|+ R1;                 /* t2 */
	R1 = PACK (R7.H, R6.H);         /* z pair */
	R0 = R5 -|- R1 (S);             /* e pair */
	R1 = I0;
	A1 = R2.H * R1.L, A0 = R2.L * R1.L (IS);
	R2.H = (A1 += R0.H * R1.H), R2.L = (A0 += R0.L * R1.H) (S2RND);
	R1 = R0.H * R3.H, R0 = R0.L * R3.H || W[I2++] = R2.L;   /* 2 e kdc */
	R6 = R6 + R0 (NS) || W[I3++] = R2.H;
.Lp_end:
	R7 = R7 + R1 (NS);

	W[P5 + FCH_T1] = R4;
	R4 >>= 16;
	W[P5 + FCH_SIZE + FCH_T1] = R4;
	W[P5 + FCH_T2] = R5;
	R5 >>= 16;
	W[P5 + FCH_SIZE + FCH_T2] = R5;
	[P5 + FCH_Z] = R6;
	[P5 + FCH_SIZE + FCH_Z] = R7;
	EPILOGUE
	.size _FoldToneStereoRunAsm, .-_FoldToneStereoRunAsm
