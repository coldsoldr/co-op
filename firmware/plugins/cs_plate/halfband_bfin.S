/*
 * Blackfin kernels for the half-rate path (src/core/halfband.h): same
 * arithmetic as HalfbandDecimateC, HalfbandInterpolateC and MixSoftLimitC
 * (halfband.cc; bit-exact, checked by tests/test_kernels.cc).
 *
 * void HalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
 * void HalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
 * void MixSoftLimitAsm(int16_t* io, const int16_t* wet, int32_t n,
 *                      int32_t gains, const SoftLimitTable* t);
 *
 * n > 0. Taps: R4 = 10019 | -2397 << 16, R5 = 710 | -151 << 16, R6 = 11 |
 * 16384 << 16 (the decimator's centre); A0 the accumulator (16 x 16
 * integer products, exact in 40 bits).
 */

#define PROLOGUE                                                           \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;

#define EPILOGUE                                                           \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

#define TAPS                                                               \
	R4.L = 10019;                                                      \
	R4.H = -2397;                                                      \
	R5.L = 710;                                                        \
	R5.H = -151;                                                       \
	R6.L = 11;                                                         \
	R6.H = 16384;

/* A0 += x[a] * tap + x[b] * tap (byte offsets from P0). */
#define PAIR(a, b, tap)                                                    \
	R0 = W[P0 + (a)] (X);                                              \
	R1 = W[P0 + (b)] (X);                                              \
	A0 += R0.L * tap (IS);                                             \
	A0 += R1.L * tap (IS);

	.text
	.align 4
	.global _HalfbandDecimateAsm
	.type _HalfbandDecimateAsm, STT_FUNC
_HalfbandDecimateAsm:
	PROLOGUE
	P0 = R0;                        /* x; output k's centre is x[2k + 10] */
	P1 = R1;
	P2 = R2;
	TAPS
	R7 = 16384;                     /* rounding */
	LSETUP (.Ld_start, .Ld_end) LC0 = P2;
.Ld_start:
	A0 = R7;
	R0 = W[P0 + 20] (X);
	A0 += R0.L * R6.H (IS);
	PAIR(22, 18, R4.L)
	PAIR(26, 14, R4.H)
	PAIR(30, 10, R5.L)
	PAIR(34, 6, R5.H)
	PAIR(38, 2, R6.L)
	A0 = A0 >>> 15;
	R0.L = A0 (IS);
	P0 += 4;
.Ld_end:
	W[P1++] = R0;
	EPILOGUE
	.size _HalfbandDecimateAsm, .-_HalfbandDecimateAsm

	.align 4
	.global _HalfbandInterpolateAsm
	.type _HalfbandInterpolateAsm, STT_FUNC
_HalfbandInterpolateAsm:
	PROLOGUE
	P0 = R0;                        /* x; pair m uses x[m .. m + 9] */
	P1 = R1;
	P2 = R2;
	TAPS
	R7 = 8192;                      /* rounding */
	LSETUP (.Li_start, .Li_end) LC0 = P2;
.Li_start:
	A0 = R7;
	PAIR(10, 8, R4.L)
	PAIR(12, 6, R4.H)
	PAIR(14, 4, R5.L)
	PAIR(16, 2, R5.H)
	PAIR(18, 0, R6.L)
	A0 = A0 >>> 14;
	R0.L = A0 (IS);
	W[P1++] = R0;
	R1 = W[P0 + 10] (X);            /* the odd output: x[m + 5] */
	P0 += 2;
.Li_end:
	W[P1++] = R1;
	EPILOGUE
	.size _HalfbandInterpolateAsm, .-_HalfbandInterpolateAsm

/*
 * Registers: P0 -> io, P1 -> wet, P3 -> t->value (t->step at +514), R7 =
 * gains, R6 = 2^29 - 1 (the table's end), R5 = 0x7fff.
 */
	.align 4
	.global _MixSoftLimitAsm
	.type _MixSoftLimitAsm, STT_FUNC
_MixSoftLimitAsm:
	PROLOGUE
	P0 = R0;
	P1 = R1;
	P2 = R2;
	R7 = [FP + 20];                 /* gains */
	P3 = [FP + 24];                 /* t */
	R6 = -1;
	R6.H = 0x1fff;                  /* 0x1fffffff */
	R5 = 0x7fff (Z);
	LSETUP (.Lm_start, .Lm_end) LC0 = P2;
.Lm_start:
	R0 = W[P0] (X);
	R1 = W[P1++] (X);
	R0 = R0.L * R7.L (IS);          /* dry x fade_out */
	R1 = R1.L * R7.H (IS);          /* wet x wet_gain */
	R0 >>>= 4;
	R1 >>>= 2;
	R0 = R0 + R1;                   /* u, Q27 */
	R2 = ABS R0;
	R2 = MIN (R2, R6);
	R3 = R2 >> 21;                  /* segment */
	R2 >>= 6;
	R2 = R2 & R5;                   /* fraction, Q15 */
	P5 = R3;
	P5 = P3 + (P5 << 1);
	R3 = W[P5] (X);                 /* value */
	R1 = W[P5 + 514] (X);           /* step */
	R1 = R1.L * R2.L (IS);
	R1 >>>= 15;
	R3 = R3 + R1;
	CC = R0 < 0;
	R1 = -R3;
	IF CC R3 = R1;
.Lm_end:
	W[P0++] = R3;
	EPILOGUE
	.size _MixSoftLimitAsm, .-_MixSoftLimitAsm
