/*
 * The tape echo's stereo run kernel, from te-op1-cfw ports/fx/src/tape_echo/tape_echo_bfin.S (5aa314b), its mono
 * kernel removed (coop runs every effect in stereo). The same arithmetic as TapeEchoRunC (tape_echo_core.cc; bit-exact,
 * checked by te-op1-cfw's tests/test_tape_echo.cc and, for this copy, tests/host/tape_echo). The 16-bit MACs'
 * default rounding (unbiased) is what the C reference models; RND_MOD is cleared on entry and restored on exit.
 *
 * void TapeEchoRunAsm(TapeEchoKernel* k, int32_t* x, int32_t* tape, int32_t* p, int32_t n);
 *
 * P2 = the write position (moves down one pair per sample; the run doesn't wrap) and R7 = the same / 4; P3 -> x
 * (input pairs in, wet pairs out); P5 -> k; I1-I3 = the heads' delays (Q15 samples), M1-M3 = their steps; A0 / A1 the
 * left / right accumulators. Each head's read wrap is constant over a run (TapeEchoPrepareRun cuts the runs): k->adj.
 * The block's TEK_SAT holds the saturation table's centre. L registers stay 0.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */

#include "src/tape_echo_layout.h"

#define SAT_TABLE __ZN5op1fx17kTapeEchoSatTableE

/* Rout = the head at delay Ipos (two pairs, weights 32767 - f and f),
 * then Ipos += Mpos. Uses R0-R4, P0; Rout may be R3. */
#define HEAD(Ipos, Mpos, ADJ, Rout)                                        \
	R0 = Ipos;                                                         \
	Ipos += Mpos;                                                      \
	R1 = R0 >> 15;                 /* ip */                            \
	R1 = (R1 + R7) << 2;           /* its address */                   \
	R3 = [P5 + ADJ];                                                   \
	R1 = R1 + R3;                  /* wrapped */                       \
	P0 = R1;                                                           \
	R2 = 32767 (X);                                                    \
	R0 <<= 17;                                                         \
	R0 >>= 17;                     /* f */                             \
	R2 = R2 - R0;                                                      \
	R2 = PACK (R0.L, R2.L);        /* f | 32767 - f */                 \
	R3 = [P0++];                                                       \
	R4 = [P0];                                                         \
	A1 = R3.H * R2.L, A0 = R3.L * R2.L;                                \
	Rout.H = (A1 += R4.H * R2.H), Rout.L = (A0 += R4.L * R2.H);

/* Weights: Rw = the head's (out | feedback); the stored one steps. */
#define WEIGHT(Rw, W, WS)                                                  \
	Rw = [P5 + W];                                                     \
	R4 = [P5 + WS];                                                    \
	R4 = Rw +|+ R4;                                                    \
	[P5 + W] = R4;

/* Everything up to the left low cut: the heads (R6, R5, R3), the wet
 * (R4, out through IO), the record sum (R6, half scale), the left low
 * cut's output (R1.L), the coefficients (R2). */
#define FRONT(IO)                                                          \
	P2 += -4;                                                          \
	R7 += -1;                                                          \
	HEAD(I1, M1, TEK_ADJ0, R6)                                         \
	HEAD(I2, M2, TEK_ADJ1, R5)                                         \
	HEAD(I3, M3, TEK_ADJ2, R3)                                         \
	WEIGHT(R0, TEK_W0, TEK_WS0)                                        \
	WEIGHT(R1, TEK_W1, TEK_WS1)                                        \
	WEIGHT(R2, TEK_W2, TEK_WS2)                                        \
	A1 = R6.H * R0.L, A0 = R6.L * R0.L;                                \
	A1 += R5.H * R1.L, A0 += R5.L * R1.L;                              \
	R4.H = (A1 += R3.H * R2.L), R4.L = (A0 += R3.L * R2.L);            \
	A1 = R6.H * R0.H, A0 = R6.L * R0.H;                                \
	A1 += R5.H * R1.H, A0 += R5.L * R1.H;                              \
	A1 += R3.H * R2.H, A0 += R3.L * R2.H;                              \
	IO                             /* the input to R0, the wet out */  \
	R1 = [P5 + TEK_GAIN];                                              \
	R2 = [P5 + TEK_GSTEP];                                             \
	R2 = R1 + R2;                                                      \
	[P5 + TEK_GAIN] = R2;                                              \
	R6.H = (A1 += R0.H * R1.H), R6.L = (A0 += R0.L * R1.H);            \
	/* Low cut, left: y = sat(s - (hp >> 16)); hp = sat32(hp + 2 k y). */ \
	R0 = [P5 + TEK_HPL];                                               \
	R1 = R0 >>> 16;                                                    \
	R2 = [P5 + TEK_K];                                                 \
	R1.L = R6.L - R1.L (S);                                            \
	A0 = R0;                                                           \
	A0 += R1.L * R2.L;                                                 \
	R0 = A0;                                                           \
	[P5 + TEK_HPL] = R0;

/* Stereo: pairs in and out of x (P3). */
#define IO_STEREO                                                          \
	R0 = [P3];                                                         \
	[P3++] = R4;
#define ROUNDING_ENTER                                                     \
	R0 = ASTAT;                                                        \
	[--SP] = R0;                                                       \
	BITCLR (R0, 8);                                                    \
	ASTAT = R0;

#define ROUNDING_EXIT                                                      \
	R0 = [SP++];                                                       \
	ASTAT = R0;

#define SAVE                                                               \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;

/* With P5 = k, P2 = p, P1 = n (the tape itself isn't needed: k->adj). */
#define ENTRY                                                              \
	ROUNDING_ENTER                                                     \
	P0.H = SAT_TABLE;                                                  \
	P0.L = SAT_TABLE;                                                  \
	R0 = P0;                                                           \
	R1 = 256;                                                          \
	R0 = R0 + R1;                                                      \
	[P5 + TEK_SAT] = R0;           /* the table's centre (index 128) */ \
	R7 = P2;                                                           \
	R7 >>= 2;                                                          \
	R0 = [P5 + TEK_POS0];                                              \
	I1 = R0;                                                           \
	R0 = [P5 + TEK_STEP0];                                             \
	M1 = R0;                                                           \
	R0 = [P5 + TEK_POS1];                                              \
	I2 = R0;                                                           \
	R0 = [P5 + TEK_STEP1];                                             \
	M2 = R0;                                                           \
	R0 = [P5 + TEK_POS2];                                              \
	I3 = R0;                                                           \
	R0 = [P5 + TEK_STEP2];                                             \
	M3 = R0;

#define EXIT                                                               \
	R0 = I1;                                                           \
	[P5 + TEK_POS0] = R0;                                              \
	R0 = I2;                                                           \
	[P5 + TEK_POS1] = R0;                                              \
	R0 = I3;                                                           \
	[P5 + TEK_POS2] = R0;                                              \
	ROUNDING_EXIT                                                      \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

	.text

	.align 4
	.global _TapeEchoRunAsm
	.type _TapeEchoRunAsm, STT_FUNC
_TapeEchoRunAsm:
	SAVE
	P5 = R0;
	P3 = R1;
	P2 = [FP + 20];                /* p */
	P1 = [FP + 24];                /* n */
	ENTRY
	LSETUP (.Ls_start, .Ls_end) LC0 = P1;
.Ls_start:
	FRONT(IO_STEREO)
	/* Low cut, right. */
	R3 = [P5 + TEK_HPR];
	R4 = R3 >>> 16;
	R4.L = R6.H - R4.L (S);
	A1 = R3;
	A1 += R4.L * R2.L;
	R3 = A1;
	[P5 + TEK_HPR] = R3;
	R1 = PACK (R4.L, R1.L);
	/* Tape loss, both: lp = sat(lp + rnd(k sat(y - lp))). */
	R0 = [P5 + TEK_LP];
	R3 = R1 -|- R0 (S);
	R3.H = R3.H * R2.H, R3.L = R3.L * R2.H;
	R0 = R0 +|+ R3 (S);
	[P5 + TEK_LP] = R0;
	/* Saturation, both: T[i] + ((T[i+1] - T[i]) f >> 8), i = x >> 8,
	 * f = x & 255; then record. */
	R1 = R0.L (X);
	R2 = R1 >>> 8;
	R3 = R0 >>> 24;
	R5 = [P5 + TEK_SAT];
	R2 <<= 1;
	R3 <<= 1;
	R2 = R2 + R5;
	R3 = R3 + R5;
	P0 = R2;
	P1 = R3;
	R1 <<= 24;
	R1 >>= 24;
	R4 = R0 << 8;
	R4 >>= 24;
	R2 = W[P0++] (X);
	R3 = W[P1++] (X);
	R5 = W[P0] (X);
	R6 = W[P1] (X);
	R5 = R5 - R2;
	R6 = R6 - R3;
	R0 = R5.L * R1.L (IS);
	R6 = R6.L * R4.L (IS);
	R0 >>>= 8;
	R6 >>>= 8;
	R2 = R2 + R0;
	R3 = R3 + R6;
	R3 = PACK (R3.L, R2.L);
.Ls_end:
	[P2] = R3;
	EXIT
	.size _TapeEchoRunAsm, .-_TapeEchoRunAsm
