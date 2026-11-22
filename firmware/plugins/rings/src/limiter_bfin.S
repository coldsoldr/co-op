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

/*
 * coop rings 0.14: RenderMono's sub-block tail in one call (src/op1_voice.cc MonoLimiter::ProcessPeak): the voice's
 * own peak (max |x|, from peak_in), then x += wet when there is a share of the shared reverb, then the loop above.
 * The same arithmetic as PeakOf, the add and MonoLimiterAsm in turn (bit-exact).
 *
 * int32_t MonoLimiterPeakAsm(int32_t* x, int32_t size, int32_t* peak, int32_t gain, int32_t pre_gain,
 *                            const int32_t* wet, int32_t peak_in);     returns the own peak; size > 0; wet may be 0
 * Registers: R4 the own peak, P4 the attack (a conditional move's source), I1 -> wet.
 */
#define LIMIT_SAMPLE(WET)                                                  \
	R0 = [I0];                                                         \
	R1 = ABS R0;                                                       \
	R4 = MAX (R4, R1);                                                 \
	WET                                                                \
	MUL(R1, R0, R6)                                                    \
	R1 <<= 3;                                                          \
	R1 = ABS R1;                                                       \
	R1 = R1 - R7 (NS);                                                 \
	LIMIT_RELEASE(R3)                                                  \
	CC = R1 <= 0;                                                      \
	IF !CC R3 = P4;                                                    \
	MUL(R2, R1, R3)                                                    \
	R7 = R7 + R2 (NS);                                                 \
	MUL(R0, R0, R5)                                                    \
	R0 = R0 << 1 (S);                                                  \
	R1 = ABS R0;                                                       \
	R2.H = 0x2000;                                                     \
	R2.L = 0x0000;                                                     \
	R1 = MIN (R1, R2);                                                 \
	R2 = R1 >> 21;                                                     \
	P1 = R2;                                                           \
	R1 <<= 11;                                                         \
	R1 >>= 1;                                                          \
	P1 = P0 + (P1 << 2);                                               \
	R2 = [P1];                                                         \
	R3 = [P1 + 4];                                                     \
	R3 = R3 - R2 (NS);                                                 \
	MUL(R3, R3, R1)                                                    \
	R2 = R2 + R3 (NS);                                                 \
	R2 >>>= 3;                                                         \
	R0 >>>= 31;                                                        \
	R2 = R2 ^ R0;                                                      \
	R2 = R2 - R0 (NS);

#if OP1RINGS_LIMITER_RELEASE_Q31 < 65536
#define LIMIT_RELEASE(r) r = OP1RINGS_LIMITER_RELEASE_Q31 (Z);
#else
#define LIMIT_RELEASE(r) r.H = (OP1RINGS_LIMITER_RELEASE_Q31 >> 16) & 0xffff; r.L = OP1RINGS_LIMITER_RELEASE_Q31 & 0xffff;
#endif
#define ADD_WET R1 = [I1++]; R0 = R0 + R1;
#define NO_WET

	.text
	.align 4
	.global _MonoLimiterPeakAsm
	.type _MonoLimiterPeakAsm, STT_FUNC
_MonoLimiterPeakAsm:
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
	R0 = [FP + 28];                 /* wet */
	I1 = R0;
	R4 = [FP + 32];                 /* the own peak so far */
	R7 = [P5];                      /* the limiter's peak */
	R1.H = (OP1RINGS_LIMITER_ATTACK_Q31 >> 16) & 0xffff;
	R1.L = OP1RINGS_LIMITER_ATTACK_Q31 & 0xffff;
	P4 = R1;
	P0.H = _kSoftLimitTable;
	P0.L = _kSoftLimitTable;
	P0 = [P0];
	CC = R0 == 0;
	IF CC JUMP .Lpk_dry;
	LSETUP (.Lpk_wet_start, .Lpk_wet_end) LC0 = P2;
.Lpk_wet_start:
	LIMIT_SAMPLE(ADD_WET)
.Lpk_wet_end:
	[I0++] = R2;
	JUMP .Lpk_done;
.Lpk_dry:
	LSETUP (.Lpk_dry_start, .Lpk_dry_end) LC0 = P2;
.Lpk_dry_start:
	LIMIT_SAMPLE(NO_WET)
.Lpk_dry_end:
	[I0++] = R2;
.Lpk_done:
	[P5] = R7;
	R0 = R4;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _MonoLimiterPeakAsm, .-_MonoLimiterPeakAsm
