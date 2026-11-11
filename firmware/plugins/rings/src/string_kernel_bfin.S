/*
 * Blackfin kernels for the string's per-sample loops. Same arithmetic as
 * StringKernelC() and StringKernelStretchC() in string_kernel.h (bit-exact;
 * checked by tests/bench_string.cc).
 *
 * void StringKernelAsm(StringKernel* k, const int32_t* in, int32_t* out,
 *                      int32_t size);
 * void StringKernelStretchAsm(StringKernel* k, const int32_t* in,
 *                             int32_t* out, int32_t size);
 *
 * The state is read from and written back to *k as the sample loop goes;
 * only the write pointer stays in a register. size > 0.
 *
 * Registers: P5 -> k, P0 = line, P4 = 8192 (guard offset), R7 = write
 * pointer; I0 -> in[], I1 -> out[]; R0-R6, P1, P2 temporaries.
 *
 * StringKernelAsm, which every sympathetic string runs, is scheduled by
 * hand: the state's loads and stores ride in the multiplies' parallel
 * slots. The stretch kernel uses the plain macros below.
 */

#define K_LINE 0
#define K_WP 4
#define K_LD 8
#define K_LDS 12
#define K_CD 16
#define K_CDS 20
#define K_H0 24
#define K_H0S 28
#define K_H1 32
#define K_H1S 36
#define K_G 40
#define K_X1 44
#define K_X2 48
#define K_S1 52
#define K_S2 56
#define K_SL 60
#define K_SP 64
#define K_ST 68
#define K_STS 72
#define K_APG 76
#define K_APGS 80
#define K_SCRATCH 84

/* d = (a * b) >> 31 without the low x low product (MulQ31Fast). d may be a
 * or b. */
#define MUL(d, a, b)                                                       \
	A0 = a.H * b.H, A1 = a.H * b.L (M);                                \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

/* R6 = Hermite read at delay R0 (Q19): base (wp + (d >> 19) - 1) & 2047,
 * fraction (d & 0x7ffff) << 12. Clobbers R0-R5, P1. */
#define HERMITE                                                            \
	R1 = R0 >>> 19;                                                    \
	R1 = R1 + R7 (NS);                                                 \
	R1 += -1;                                                          \
	R1 <<= 21;                                                         \
	R1 >>= 21;                                                         \
	P1 = R1;                                                           \
	R0 <<= 13;                                                         \
	R0 >>= 1;                       /* f */                            \
	P1 = P0 + (P1 << 2);                                               \
	R1 = [P1];                      /* xm1 */                          \
	R2 = [P1 + 4];                  /* x0 */                           \
	R3 = [P1 + 8];                  /* x1 */                           \
	R4 = [P1 + 12];                 /* x2 */                           \
	R1 = R3 - R1 (NS);                                                 \
	R1 >>>= 1;                      /* c */                            \
	R3 = R2 - R3 (NS);              /* v */                            \
	R4 = R4 - R2 (NS);                                                 \
	R4 >>>= 1;                                                         \
	R5 = R1 + R3 (NS);              /* w */                            \
	R4 = R4 + R3 (NS);                                                 \
	R4 = R4 + R5 (NS);              /* a */                            \
	R5 = R5 + R4 (NS);              /* b_neg */                        \
	MUL(R6, R4, R0)                                                    \
	R6 = R6 - R5 (NS);                                                 \
	MUL(R6, R6, R0)                                                    \
	R6 = R6 + R1 (NS);                                                 \
	MUL(R6, R6, R0)                                                    \
	R6 = R6 + R2 (NS);

/* s = R6 + in; FIR, two one-poles, clamp; write at wp (and the guard);
 * wp--; out += s - linear read at the pickup delay. */
#define DAMP_WRITE_PICKUP                                                  \
	R0 = [I0++];                                                       \
	R6 = R6 + R0 (NS);              /* s */                            \
	/* FIR: y = h0 x1 + h1 (s + x2); x2 = x1; x1 = s. */               \
	R1 = [P5 + K_X1];                                                  \
	R2 = [P5 + K_X2];                                                  \
	R3 = [P5 + K_H0];                                                  \
	R4 = [P5 + K_H1];                                                  \
	[P5 + K_X2] = R1;                                                  \
	[P5 + K_X1] = R6;                                                  \
	R6 = R6 + R2 (NS);                                                 \
	MUL(R5, R3, R1)                                                    \
	MUL(R6, R4, R6)                                                    \
	R5 = R5 + R6 (NS);              /* y */                            \
	R0 = [P5 + K_H0S];                                                 \
	R3 = R3 + R0 (NS);                                                 \
	[P5 + K_H0] = R3;                                                  \
	R0 = [P5 + K_H1S];                                                 \
	R4 = R4 + R0 (NS);                                                 \
	[P5 + K_H1] = R4;                                                  \
	/* Two one-poles: v = (x - s) G; lp = v + s; s = lp + v. */        \
	R0 = [P5 + K_G];                                                   \
	R1 = [P5 + K_S1];                                                  \
	R2 = [P5 + K_S2];                                                  \
	R5 = R5 - R1 (NS);                                                 \
	MUL(R3, R5, R0)                 /* v1 */                           \
	R4 = R3 + R1 (NS);              /* lp1 */                          \
	R1 = R4 + R3 (NS);                                                 \
	[P5 + K_S1] = R1;                                                  \
	R4 = R4 - R2 (NS);                                                 \
	MUL(R3, R4, R0)                 /* v2 */                           \
	R4 = R3 + R2 (NS);                                                 \
	R5.H = 0x0e00;                  /* 7 << 25 */                      \
	R5.L = 0x0000;                                                     \
	R4 = MIN (R4, R5);                                                 \
	R5 = -R5;                                                          \
	R4 = MAX (R4, R5);              /* lp, clamped */                  \
	R2 = R4 + R3 (NS);                                                 \
	[P5 + K_S2] = R2;                                                  \
	/* Write at wp (and in the guard if wp < 4); wp-- (mod 2048). */    \
	P1 = R7;                                                           \
	P1 = P0 + (P1 << 2);                                               \
	CC = R7 <= 3;                                                      \
	P2 = P1 + P4;                                                      \
	IF !CC P2 = P1;                                                    \
	[P1] = R4;                                                         \
	[P2] = R4;                                                         \
	R7 += -1;                                                          \
	R7 <<= 21;                                                         \
	R7 >>= 21;                                                         \
	/* Linear read at the pickup delay: base (wp + (c >> 19)) & 2047. */ \
	R0 = [P5 + K_CD];                                                  \
	R1 = [P5 + K_CDS];                                                 \
	R0 = R0 + R1 (NS);                                                 \
	[P5 + K_CD] = R0;                                                  \
	R1 = R0 >>> 19;                                                    \
	R1 = R1 + R7 (NS);                                                 \
	R1 <<= 21;                                                         \
	R1 >>= 21;                                                         \
	P1 = R1;                                                           \
	R0 <<= 13;                                                         \
	R0 >>= 1;                       /* fraction */                     \
	P1 = P0 + (P1 << 2);                                               \
	R1 = [P1];                                                         \
	R2 = [P1 + 4];                                                     \
	R2 = R2 - R1 (NS);                                                 \
	MUL(R3, R2, R0)                                                    \
	R1 = R1 + R3 (NS);              /* aux */                          \
	R4 = R4 - R1 (NS);                                                 \
	R0 = [I1];                                                         \
	R0 = R0 + R4 (NS);

#define PROLOGUE                                                           \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	P5 = R0;                                                           \
	I0 = R1;                                                           \
	I1 = R2;                                                           \
	R3 = [FP + 20];                                                    \
	P2 = R3;                                                           \
	P0 = [P5 + K_LINE];                                                \
	R7 = [P5 + K_WP];                                                  \
	P4 = 0x2000 (Z);

#define EPILOGUE                                                           \
	[P5 + K_WP] = R7;                                                  \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

	.text
	.align 4
	.global _StringKernelAsm
	.type _StringKernelAsm, STT_FUNC
_StringKernelAsm:
	PROLOGUE
	LSETUP (.Lstart, .Lend) LC0 = P2;
.Lstart:
	/* Loop delay; Hermite base (wp + (d >> 19) - 1) & 2047. */
	R0 = [P5 + K_LD];
	R1 = [P5 + K_LDS];
	R0 = R0 + R1 (NS);
	R1 = R0 >>> 19 || [P5 + K_LD] = R0 || NOP;
	R1 = R1 + R7 (NS);
	R1 += -1;
	R1 = R1 << 21;
	R1 = R1 >> 21;
	P1 = R1;
	R0 = R0 << 13;
	R0 = R0 >> 1;                   /* f */
	P1 = P0 + (P1 << 2);
	R1 = [P1];                      /* xm1 */
	R2 = [P1 + 4];                  /* x0 */
	R3 = [P1 + 8];                  /* x1 */
	R4 = [P1 + 12];                 /* x2 */
	R1 = R3 - R1 (NS);
	R1 >>>= 1;                      /* c */
	R3 = R2 - R3 (NS);              /* v */
	R4 = R4 - R2 (NS);
	R4 >>>= 1;
	R5 = R1 + R3 (NS);              /* w */
	R4 = R4 + R3 (NS);
	R4 = R4 + R5 (NS);              /* a */
	R5 = R5 + R4 (NS);              /* b_neg */
	/* r = a f - b_neg; load the FIR's x1 and the input meanwhile. */
	A0 = R4.H * R0.H, A1 = R4.H * R0.L (M) || R3 = [P5 + K_X1] || NOP;
	A1 += R0.H * R4.L (M);
	A1 = A1 >>> 15 || R4 = [I0++] || NOP;
	R6 = (A0 += A1);
	R6 = R6 - R5 (NS);
	/* r = r f + c; load x2. */
	A0 = R6.H * R0.H, A1 = R6.H * R0.L (M) || R5 = [P5 + K_X2] || NOP;
	A1 += R0.H * R6.L (M);
	A1 = A1 >>> 15;
	R6 = (A0 += A1);
	R6 = R6 + R1 (NS);
	/* r f + x0 + input; load h0 and h1. */
	A0 = R6.H * R0.H, A1 = R6.H * R0.L (M) || R1 = [P5 + K_H0] || NOP;
	A1 += R0.H * R6.L (M);
	A1 = A1 >>> 15 || R0 = [P5 + K_H1] || NOP;
	R6 = (A0 += A1);
	R6 = R6 + R2 (NS);
	R6 = R6 + R4 (NS);              /* s */
	/* FIR: y = h0 x1 + h1 (s + x2); x1 = s, x2 = x1; load the steps. */
	R2 = R6 + R5 (NS);
	A0 = R1.H * R3.H, A1 = R1.H * R3.L (M) || [P5 + K_X1] = R6 || NOP;
	A1 += R3.H * R1.L (M) || [P5 + K_X2] = R3 || NOP;
	A1 = A1 >>> 15 || R5 = [P5 + K_H0S] || NOP;
	R4 = (A0 += A1) || R3 = [P5 + K_H1S] || NOP;
	A0 = R0.H * R2.H, A1 = R0.H * R2.L (M) || R6 = [P5 + K_G] || NOP;
	A1 += R2.H * R0.L (M);
	A1 = A1 >>> 15;
	R2 = (A0 += A1);
	R4 = R4 + R2 (NS);              /* y */
	R1 = R1 + R5 (NS);              /* h0 += step */
	R0 = R0 + R3 (NS);              /* h1 += step */
	R5 = [P5 + K_S1];
	R4 = R4 - R5 (NS);
	/* Two one-poles; store h0, h1, s1 and load s2 meanwhile. */
	A0 = R4.H * R6.H, A1 = R4.H * R6.L (M) || [P5 + K_H0] = R1 || NOP;
	A1 += R6.H * R4.L (M) || [P5 + K_H1] = R0 || NOP;
	A1 = A1 >>> 15 || R1 = [P5 + K_S2] || NOP;
	R3 = (A0 += A1);                /* v1 */
	R4 = R3 + R5 (NS);              /* lp1 */
	R5 = R4 + R3 (NS);              /* s1 */
	R4 = R4 - R1 (NS);
	A0 = R4.H * R6.H, A1 = R4.H * R6.L (M) || [P5 + K_S1] = R5 || NOP;
	A1 += R6.H * R4.L (M);
	A1 = A1 >>> 15;
	R3 = (A0 += A1);                /* v2 */
	R4 = R3 + R1 (NS);
	R5.H = 0x0e00;                  /* 7 << 25 */
	R5.L = 0x0000;
	R4 = MIN (R4, R5);
	R5 = -R5;
	R4 = MAX (R4, R5);              /* lp, clamped */
	R2 = R4 + R3 (NS);              /* s2 */
	/* Write at wp (and in the guard if wp < 4); wp-- (mod 2048). */
	P1 = R7;
	R0 = [P5 + K_CD];
	R1 = [P5 + K_CDS];
	P1 = P0 + (P1 << 2);
	CC = R7 <= 3;
	P2 = P1 + P4;
	IF !CC P2 = P1;
	[P1] = R4;
	[P2] = R4;
	R7 += -1;
	R7 <<= 21;
	R7 >>= 21;
	/* Linear read at the pickup delay: base (wp + (c >> 19)) & 2047. */
	R0 = R0 + R1 (NS);
	R1 = R0 >>> 19 || [P5 + K_CD] = R0 || NOP;
	R1 = R1 + R7 (NS);
	R1 = R1 << 21;
	R1 = R1 >> 21;
	P1 = R1;
	R0 = R0 << 13 || [P5 + K_S2] = R2 || NOP;
	R0 = R0 >> 1;                   /* fraction */
	P1 = P0 + (P1 << 2);
	R1 = [P1];
	R2 = [P1 + 4];
	R2 = R2 - R1 (NS);
	A0 = R2.H * R0.H, A1 = R2.H * R0.L (M) || R3 = [I1] || NOP;
	A1 += R0.H * R2.L (M);
	A1 = A1 >>> 15;
	R5 = (A0 += A1);
	R1 = R1 + R5 (NS);              /* aux */
	R4 = R4 - R1 (NS);
	R3 = R3 + R4 (NS);
.Lend:
	[I1++] = R3;
	EPILOGUE
	.size _StringKernelAsm, .-_StringKernelAsm

	.align 4
	.global _StringKernelStretchAsm
	.type _StringKernelStretchAsm, STT_FUNC
_StringKernelStretchAsm:
	PROLOGUE
	LSETUP (.Ls_start, .Ls_end) LC0 = P2;
.Ls_start:
	/* d = loop delay with its lowest bit cleared (upstream's delay_fm = 1). */
	R0 = [P5 + K_LD];
	R1 = [P5 + K_LDS];
	R0 = R0 + R1 (NS);
	[P5 + K_LD] = R0;
	BITCLR (R0, 0);
	R1 = [P5 + K_ST];
	R2 = [P5 + K_STS];
	R1 = R1 + R2 (NS);
	[P5 + K_ST] = R1;
	R2 = [P5 + K_APG];
	R3 = [P5 + K_APGS];
	R2 = R2 + R3 (NS);
	[P5 + K_APG] = R2;
	MUL(R1, R0, R1)                 /* ap_delay */
	R2 = R0 - R1 (NS);              /* main_delay */
	R3.H = 0x0020;                  /* 4 << 19 */
	R3.L = 0x0000;
	CC = R1 < R3;
	IF CC JUMP .Ls_plain;
	CC = R2 < R3;
	IF CC JUMP .Ls_plain;
	[P5 + K_SCRATCH] = R1;
	R0 = R2;
	HERMITE
	/* The stretch allpass: read at (sp + (ap >> 19)) & 1023; write s +
	 * read g at sp; s = read - write g; sp-- (mod 1024). */
	R1 = [P5 + K_SCRATCH];
	R1 >>>= 19;
	R2 = [P5 + K_SP];
	R1 = R1 + R2 (NS);
	R1 <<= 22;
	R1 >>= 22;
	P1 = R1;
	P2 = [P5 + K_SL];
	P1 = P2 + (P1 << 2);
	R1 = [P1];                      /* read */
	R3 = [P5 + K_APG];
	MUL(R4, R1, R3)
	R4 = R6 + R4 (NS);              /* write */
	P1 = R2;
	P1 = P2 + (P1 << 2);
	[P1] = R4;
	R2 += -1;
	R2 <<= 22;
	R2 >>= 22;
	[P5 + K_SP] = R2;
	MUL(R4, R4, R3)
	R6 = R1 - R4 (NS);
.Ls_join:
	DAMP_WRITE_PICKUP
.Ls_end:
	[I1++] = R0;
	EPILOGUE

	/* Outside the loop body: the plain read (no allpass), back into it. */
.Ls_plain:
	HERMITE
	JUMP .Ls_join;
	.size _StringKernelStretchAsm, .-_StringKernelStretchAsm
