/*
 * Blackfin kernels for the modal resonator's coefficient update (ModalResonator::RecomputeFilters,
 * src/modal_resonator.cc; coop rings 0.14, docs/efficiency-audit.md finding 15): its passes A and C, operation for
 * operation as ModalTranslateC and ModalCoeffsC there (bit-exact; tests/host/rings compares the simulator, which
 * runs these, with the host, which runs the C).
 *
 * void ModalTranslateAsm(Mode* modes, const int32_t* geometry, int32_t n, int32_t u0);     n > 0
 *   A: per mode, s1 = ((bp x k1) << 3, sat) + (((u0 - lp) x tan) << 3, sat) with k1 = (rho x a_old) x sec,
 *   a_old = (cos x cos) / 2 - (sin x sin) / 2 - x / 2 (the old coefficients); s2 = bp x half_f + lp; bp = s1, lp = s2.
 * void ModalCoeffsAsm(ModalCoeffs* k);                                                         k->n > 0
 *   C: per mode, r = 1 / (1 + pf q) (r_m, r_shift) from l = log2_pf + log2_q; x, rho, half_f, half_q, gain; the state
 *   from s1 = bp, s2 = lp; then q_loss and log2_q move on (the C, line for line).
 * x is MulQ31Fast (src/fixed.h); ShlSat by a constant is `<< n (S)`; ShiftRight(x, n) is x >>> min(n, 31).
 * Layouts: Mode (src/mode_bank.h) lp 0, bp 4, half_f 8, half_q 12 (24 bytes); geometry sin 0, cos 4, sec 8, tan 12,
 * rho 16, x 20 (24 bytes); ModalCoeffs (modal_resonator.cc) below.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */

#define MUL(d, a, b)                                                       \
	A0 = a.H * b.H, A1 = a.H * b.L (M);                                \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

#define M_LP 0
#define M_BP 4
#define M_HALF_F 8
#define M_HALF_Q 12
#define G_SIN 0
#define G_COS 4
#define G_SEC 8
#define G_TAN 12
#define G_RHO 16
#define G_X 20

#define K_MODES 0
#define K_GEOMETRY 4
#define K_LOG2PF 8
#define K_GAIN 12
#define K_N 16
#define K_U0 20
#define K_LOG2Q 24
#define K_QLOSS 28
#define K_QRATE 32
#define K_EXP2NEG 36
#define K_RECIP1P 40
#define K_RHO 44
#define K_INVRHO 48
#define K_RECIP1M 52
#define K_LOG2M 56

	.text
	.align 4
	.global _ModalTranslateAsm
	.type _ModalTranslateAsm, STT_FUNC
_ModalTranslateAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P0 = R0;                        /* modes */
	P1 = R1;                        /* geometry */
	P2 = R2;                        /* n */
	R7 = [FP + 20];                 /* u0 */
	LSETUP (.Lmt_start, .Lmt_end) LC0 = P2;
.Lmt_start:
	R0 = [P1 + G_COS];
	MUL(R1, R0, R0)
	R1 >>>= 1;
	R0 = [P1 + G_SIN];
	MUL(R2, R0, R0)
	R2 >>>= 1;
	R1 = R1 - R2;
	R0 = [P1 + G_X];
	R0 >>>= 1;
	R1 = R1 - R0;                   /* a_old */
	R0 = [P1 + G_RHO];
	MUL(R1, R0, R1)
	R0 = [P1 + G_SEC];
	MUL(R1, R1, R0)                 /* k1 */
	R3 = [P0 + M_BP];
	MUL(R2, R3, R1)
	R2 = R2 << 3 (S);
	R4 = [P0 + M_LP];
	R0 = R7 - R4;
	R5 = [P1 + G_TAN];
	MUL(R0, R0, R5)
	R0 = R0 << 3 (S);
	R2 = R2 + R0;                   /* s1 */
	R5 = [P0 + M_HALF_F];
	MUL(R0, R3, R5)
	R0 = R0 + R4;                   /* s2 */
	[P0 + M_BP] = R2;
	[P0 + M_LP] = R0;
	P0 += 24;
.Lmt_end:
	P1 += 24;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ModalTranslateAsm, .-_ModalTranslateAsm

/* d = Exp2NegQ31(x) with the table at [P5 + K_EXP2NEG]: n = x >> 24, 0 from 31; else T[i] + (T[i + 1] - T[i]) x f
 * with i = (x >> 16) & 255, f = (x & 0xffff) << 15, then >> n. x >= 0; uses R1, R2, R3, P4; d may be x. */
#define EXP2NEG(d, x, label)                                               \
	R1 = x >>> 24;                                                     \
	R2 = 31;                                                           \
	CC = R1 < R2;                                                      \
	IF CC JUMP label##_go;                                             \
	d = 0;                                                             \
	JUMP label;                                                        \
label##_go:                                                                \
	R2 = x >> 16;                                                      \
	R2 = R2.B (Z);                                                     \
	R3 = x.L (Z);                                                      \
	R3 <<= 15;                                                         \
	P4 = [P5 + K_EXP2NEG];                                             \
	P2 = R2;                                                           \
	P4 = P4 + (P2 << 2);                                               \
	R2 = [P4];                                                         \
	d = [P4 + 4];                                                      \
	d = d - R2;                                                        \
	MUL(d, d, R3)                                                      \
	d = d + R2;                                                        \
	d >>>= R1;                                                         \
label:

/* d = Interp8(table at [P5 + off], x): x < 0 as 0; T[x >> 23] + (T[i + 1] - T[i]) x ((x & 0x7fffff) << 8).
 * Uses R2, R3, P4; d may be x. */
#define INTERP8(d, x, off)                                                 \
	R2 = 0;                                                            \
	x = MAX (x, R2);                                                   \
	R2 = x >> 23;                                                      \
	R3 = x << 9;                                                       \
	R3 >>= 1;                                                          \
	P4 = [P5 + off];                                                   \
	P2 = R2;                                                           \
	P4 = P4 + (P2 << 2);                                               \
	R2 = [P4];                                                         \
	d = [P4 + 4];                                                      \
	d = d - R2;                                                        \
	MUL(d, d, R3)                                                      \
	d = d + R2;

/* d = InterpHalf(table at [P5 + off], x): T[256] from x >= 1/2, else Interp8 of x << 1. x is kept (R1 its copy).
 * Uses R1, R2, R3, P4. */
#define INTERPHALF(d, x, off, label)                                       \
	P4 = [P5 + off];                                                   \
	d = [P4 + 1024];                                                   \
	R2.H = 0x4000;                                                     \
	R2.L = 0;                                                          \
	CC = x < R2;                                                       \
	IF !CC JUMP label;                                                 \
	R1 = x << 1;                                                       \
	INTERP8(d, R1, off)                                                \
label:

/* ShiftRight(d, n): d >>>= min(n, 31); n a register, kept; uses R3. */
#define SHIFTRIGHT(d, n)                                                   \
	R3 = 31;                                                           \
	R3 = MIN (R3, n);                                                  \
	d >>>= R3;

	.text
	.align 4
	.global _ModalCoeffsAsm
	.type _ModalCoeffsAsm, STT_FUNC
_ModalCoeffsAsm:
	LINK 16;                        /* [FP - 4] sc, [FP - 8] inv_b, [FP - 12] inv_rho, [FP - 16] sin^2 u0 */
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;                        /* k */
	P0 = [P5 + K_MODES];
	P1 = [P5 + K_GEOMETRY];
	P3 = [P5 + K_LOG2PF];
	R1 = [P5 + K_GAIN];
	I0 = R1;
	P2 = [P5 + K_N];
	LSETUP (.Lmc_start, .Lmc_end) LC0 = P2;
.Lmc_start:
	/* r = 1 / (1 + pf q): R6 r_m, R7 r_shift */
	R0 = [P3++];
	R1 = [P5 + K_LOG2Q];
	R0 = R0 + R1;                   /* l */
	CC = R0 <= 0;
	IF !CC JUMP .Lmc_pos;
	R0 = -R0;
	EXP2NEG(R0, R0, .Lmc_e1)
	INTERP8(R6, R0, K_RECIP1P)
	R7 = 0;
	JUMP .Lmc_r;
.Lmc_pos:
	R7 = R0 >>> 24;                 /* r_shift */
	R0 <<= 8;
	R0 >>= 8;                       /* l & 0xffffff */
	EXP2NEG(R4, R0, .Lmc_e2)        /* t_m */
	R0 = R4;
	SHIFTRIGHT(R0, R7)
	INTERP8(R0, R0, K_RECIP1P)
	MUL(R6, R4, R0)                 /* r_m */
.Lmc_r:
	R4 = [P1 + G_SIN];
	R5 = [P1 + G_COS];
	MUL(R0, R4, R5)                 /* sc */
	[FP - 4] = R0;
	MUL(R1, R6, R0)
	SHIFTRIGHT(R1, R7)              /* x */
	[P1 + G_X] = R1;
	INTERPHALF(R0, R1, K_RHO, .Lmc_h1)   /* rho */
	[P1 + G_RHO] = R0;
	MUL(R2, R4, R0)                 /* half_f */
	[P0 + M_HALF_F] = R2;
	MUL(R2, R6, R5)
	MUL(R2, R2, R0)
	R7 += 1;
	SHIFTRIGHT(R2, R7)              /* half_q */
	[P0 + M_HALF_Q] = R2;
	MUL(R2, R5, R0)
	R2 >>>= 1;                      /* gain */
	[I0++] = R2;

	/* The state: R6 a_new, R7 sin^2 */
	MUL(R0, R5, R5)
	R0 >>>= 1;
	MUL(R7, R4, R4)
	R1 = R7 >>> 1;
	R0 = R0 - R1;
	R1 = [P1 + G_X];
	R2 = R1 >>> 1;
	R6 = R0 - R2;                   /* a_new */
	INTERPHALF(R0, R1, K_RECIP1M, .Lmc_h2)   /* inv_b */
	[FP - 8] = R0;
	R1 = [P1 + G_X];
	INTERPHALF(R0, R1, K_INVRHO, .Lmc_h3)    /* inv_rho */
	[FP - 12] = R0;
	R2 = [P5 + K_U0];
	MUL(R3, R7, R2)
	[FP - 16] = R3;                 /* sin^2 u0 */
	R1 = [P0 + M_LP];               /* s2 */
	MUL(R0, R1, R6)
	R2 = [P0 + M_BP];               /* s1 */
	R3 = [FP - 4];
	R3 = R3 << 1 (S);
	MUL(R3, R2, R3)
	R3 >>>= 1;
	R0 = R0 - R3;
	R3 = [FP - 16];
	R0 = R0 + R3;                   /* sum_lp */
	R3 = [FP - 8];
	MUL(R0, R0, R3)
	R0 = R0 << 2 (S);               /* lp */
	MUL(R6, R2, R5)
	R7 = [P5 + K_U0];
	R1 = R1 - R7;
	MUL(R1, R1, R4)
	R6 = R6 + R1;                   /* sum_bp */
	R1 = [FP - 8];
	R7 = [FP - 12];
	MUL(R1, R1, R7)
	MUL(R6, R6, R1)
	R6 = R6 << 3 (S);               /* bp */
	[P0 + M_LP] = R0;
	[P0 + M_BP] = R6;

	/* q_loss and log2_q */
	R0 = [P5 + K_QLOSS];
	R1.H = 0x7fff;
	R1.L = 0xffff;
	R1 = R1 - R0;
	R2 = [P5 + K_QRATE];
	MUL(R1, R2, R1)
	R0 = R0 + R1;
	[P5 + K_QLOSS] = R0;
	/* Log2Q24(q_loss): n = clrsb; m = v << n; t = m - 2^30; T[t >> 22] interpolated by (t & 0x3fffff) << 9;
	 * (l >> 6) - ((1 + n) << 24) */
	R1.L = SIGNBITS R0;
	R1 = R1.L (Z);
	R0 <<= R1;
	R2.H = 0x4000;
	R2.L = 0;
	R0 = R0 - R2;                   /* t */
	R2 = R0 >>> 22;
	R3 = R0 << 10;
	R3 >>= 1;
	P4 = [P5 + K_LOG2M];
	P2 = R2;
	P4 = P4 + (P2 << 2);
	R2 = [P4];
	R0 = [P4 + 4];
	R0 = R0 - R2;
	MUL(R0, R0, R3)
	R0 = R0 + R2;
	R0 >>>= 6;
	R1 += 1;
	R1 <<= 24;
	R0 = R0 - R1;
	R2 = [P5 + K_LOG2Q];
	R0 = R0 + R2;
	R1.H = 0xc000;                  /* -(64 << 24) */
	R1.L = 0;
	R0 = MAX (R0, R1);
	[P5 + K_LOG2Q] = R0;
	P0 += 24;
.Lmc_end:
	P1 += 24;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ModalCoeffsAsm, .-_ModalCoeffsAsm
