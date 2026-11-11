/*
 * Blackfin kernels for the ensemble's sides: the same arithmetic as
 * EnsembleSideRunC (ensemble.cc; bit-exact, checked by
 * tests/test_ensemble.cc). The 16-bit MACs' default rounding (unbiased) is
 * what the C reference models; RND_MOD is cleared on entry and restored on
 * exit, so the caller's ASTAT doesn't matter. L registers stay 0.
 *
 * void EnsSide3Asm(const EnsembleSideKernel* k, int16_t* out, int32_t n);
 * void EnsSide1Asm(const EnsembleSideKernel* k, int16_t* out, int32_t n);
 *
 * P4 -> line_a, P3 -> line_b (both at this sample's newest; each sample
 * moves them down one), I3 -> out; I0/M0, I1/M1, I2/M2 = the three delays
 * (Q16) and their steps; R7 = g1 | g2, R6 = their steps, R5 = g3 | its
 * step, R4 = 32767; A1 the sum, A0 the interpolation; R0-R3, P0
 * temporaries. n > 0.
 */

#include "src/ensemble_layout.h"

#define ROUNDING_ENTER                                                     \
	R0 = ASTAT;                                                        \
	[--SP] = R0;                                                       \
	BITCLR (R0, 8);                                                    \
	ASTAT = R0;

#define ROUNDING_EXIT                                                      \
	R0 = [SP++];                                                       \
	ASTAT = R0;

/* A tap in two parts. ADDR: P0 -> the line at Pline read Ipos (Q16)
 * samples back, R2 = f | 32767 - f. Uses R0, R1. */
#define ADDR(Ipos, Pline)                                                  \
	R0 = Ipos;                                                         \
	R1 = R0.L (Z);                                                     \
	R0 >>>= 16;                    /* ip */                            \
	P0 = R0;                                                           \
	R1 >>= 1;                      /* f */                             \
	R2 = R4 - R1;                                                      \
	P0 = Pline + (P0 << 1);                                            \
	R2 = PACK (R1.L, R2.L);        /* f | 32767 - f */

/* READ: R3.L = the two samples, weighted (rounded); Ipos += Mstep. The
 * first load goes in parallel with `prev` (the previous voice's gain MAC,
 * or MNOP). Uses A0, R0, R1. */
#define READ(Ipos, Mstep, prev)                                            \
	prev || R0 = W[P0++] (X) || NOP;                                   \
	A0 = R0.L * R2.L || R1 = W[P0] (X) || NOP;                         \
	R3.L = (A0 += R1.L * R2.H) || Ipos += Mstep || NOP;

#define ENTER                                                              \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	P5 = R0;                                                           \
	I3 = R1;                                                           \
	P1 = R2;                                                           \
	ROUNDING_ENTER                                                     \
	P4 = [P5 + ESK_LINE_A];                                            \
	P3 = [P5 + ESK_LINE_B];                                            \
	R0 = [P5 + ESK_POS];                                               \
	I0 = R0;                                                           \
	R0 = [P5 + ESK_STEP];                                              \
	M0 = R0;                                                           \
	R0 = [P5 + ESK_POS + 4];                                           \
	I1 = R0;                                                           \
	R0 = [P5 + ESK_STEP + 4];                                          \
	M1 = R0;                                                           \
	R0 = [P5 + ESK_POS + 8];                                           \
	I2 = R0;                                                           \
	R0 = [P5 + ESK_STEP + 8];                                          \
	M2 = R0;                                                           \
	R7 = [P5 + ESK_G12];                                               \
	R6 = [P5 + ESK_G12_STEP];                                          \
	R5 = [P5 + ESK_G3];                                                \
	R4 = 32767 (X);

#define EXIT                                                               \
	ROUNDING_EXIT                                                      \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

	.text

	.align 4
	.global _EnsSide3Asm
	.type _EnsSide3Asm, STT_FUNC
_EnsSide3Asm:
	ENTER
	LSETUP (.L3_start, .L3_end) LC0 = P1;
.L3_start:
	ADDR(I0, P4)
	READ(I0, M0, MNOP)
	ADDR(I1, P4)
	READ(I1, M1, A1 = R3.L * R7.L)
	ADDR(I2, P3)
	READ(I2, M2, A1 += R3.L * R7.H)
	R0.H = (A1 += R3.L * R5.L);
	R7 = R7 +|+ R6 || W[I3++] = R0.H || NOP;
	R5.L = R5.L + R5.H (NS);
	P4 += -2;
.L3_end:
	P3 += -2;
	EXIT
	.size _EnsSide3Asm, .-_EnsSide3Asm

	.align 4
	.global _EnsSide1Asm
	.type _EnsSide1Asm, STT_FUNC
_EnsSide1Asm:
	ENTER
	LSETUP (.L1_start, .L1_end) LC0 = P1;
.L1_start:
	ADDR(I0, P4)
	READ(I0, M0, MNOP)
	R0.H = (A1 = R3.L * R7.L);
	R7 = R7 +|+ R6 || W[I3++] = R0.H || NOP;
.L1_end:
	P4 += -2;
	EXIT
	.size _EnsSide1Asm, .-_EnsSide1Asm
