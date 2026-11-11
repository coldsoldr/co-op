#include "src/rate_config.h"
/*
 * Blackfin kernel for MonoLimiter's sample loop (src/op1_voice.cc): same
 * arithmetic as the C loop (bit-exact; tests/bench_models.cc checks it).
 *
 * void MonoLimiterAsm(int32_t* x, int32_t size, int32_t* peak, int32_t gain,
 *                     int32_t pre_gain);
 *
 * Per sample:
 *   peak follower on |(x * pre_gain) << 3|, attack 0.05 / release 0.00002;
 *   y = SoftLimit((x * gain) << 1 (saturated)), by table over [0, 4] (Q27):
 *   the magnitude is clamped to 4.0, where the table index is 256 and the
 *   interpolation fraction exactly 0, which is the C code's y = table[256].
 * size > 0.
 */

#define MUL(d, a, b)                                                       \
	A0 = a.H * b.H, A1 = a.H * b.L (M);                                \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

	.text
	.align 4
	.global _MonoLimiterAsm
	.type _MonoLimiterAsm, STT_FUNC
_MonoLimiterAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;

	I0 = R0;                        /* x */
	P2 = R1;                        /* size */
	P5 = R2;                        /* &peak */
	R5 = [FP + 20];                 /* gain */
	R6 = [FP + 24];                 /* pre-gain */
	R7 = [P5];                      /* peak */
	R4.H = (OP1RINGS_LIMITER_ATTACK_Q31 >> 16) & 0xffff;   /* attack: 0.05 at 44.1 kHz (src/rate_config.h) */
	R4.L = OP1RINGS_LIMITER_ATTACK_Q31 & 0xffff;
	P0.H = _kSoftLimitTable;
	P0.L = _kSoftLimitTable;
	P0 = [P0];                      /* table base */

	LSETUP (.Lstart, .Lend) LC0 = P2;
.Lstart:
	R0 = [I0];
	/* Peak follower on the pre-gain signal. */
	MUL(R1, R0, R6)
	R1 <<= 3;
	R1 = ABS R1;
	R1 = R1 - R7 (NS);              /* error */
#if OP1RINGS_LIMITER_RELEASE_Q31 < 65536
	R3 = OP1RINGS_LIMITER_RELEASE_Q31 (Z);   /* release: 0.00002 at 44.1 kHz */
#else
	R3.H = (OP1RINGS_LIMITER_RELEASE_Q31 >> 16) & 0xffff;   /* release (src/rate_config.h) */
	R3.L = OP1RINGS_LIMITER_RELEASE_Q31 & 0xffff;
#endif
	CC = R1 <= 0;
	IF !CC R3 = R4;
	MUL(R2, R1, R3)
	R7 = R7 + R2 (NS);
	/* y = (x gain) << 1, saturated. */
	MUL(R0, R0, R5)
	R0 = R0 << 1 (S);
	/* SoftLimit: table over |y| in [0, 4]. */
	R1 = ABS R0;
	R2.H = 0x2000;                  /* 4.0 in Q27 */
	R2.L = 0x0000;
	R1 = MIN (R1, R2);
	R2 = R1 >> 21;                  /* index */
	P1 = R2;
	R1 <<= 11;
	R1 >>= 1;                       /* fraction, Q31 */
	P1 = P0 + (P1 << 2);
	R2 = [P1];
	R3 = [P1 + 4];
	R3 = R3 - R2 (NS);
	MUL(R3, R3, R1)
	R2 = R2 + R3 (NS);
	R2 >>>= 3;                      /* Q30 -> Q27 */
	R0 >>>= 31;                     /* sign of y */
	R2 = R2 ^ R0;
	R2 = R2 - R0 (NS);
.Lend:
	[I0++] = R2;

	[P5] = R7;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _MonoLimiterAsm, .-_MonoLimiterAsm
