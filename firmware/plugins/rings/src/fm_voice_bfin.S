/*
 * Blackfin kernel for the FM voice's sample loop (FmVoiceFixed::Process, src/fm_voice_fixed.cc; coop rings 0.14,
 * docs/efficiency-audit.md finding 15). The C loop's arithmetic, operation for operation (bit-exact; the C loop stays
 * for the host and is what tests/host/rings compares this with in the simulator).
 *
 * void FmLoopAsm(FmKernel* k, int32_t* out, int32_t size);       size > 0
 *
 * FmKernel (fm_voice_fixed.cc): the loop's state (0..40, read and written back), its per-call values (44..72) and
 * its constants (76..104). P5 -> k, P4 -> k + 64 (the 16-bit loads reach 60 bytes), P3 -> kSineQ31 (from k), I0 -> out.
 *
 * Per sample, as the C:
 *   ae -= ae x adecay;  be -= be x bdecay;  br += br_step;  b2 = br x br
 *   fm_max = min(b2, 1/2), fm_min = b2 - fm_max         (the C's two cases: b2 >= 0)
 *   fm_env = 1/2 + env x (be - 1/2);  target = fm_min / 2 + (fm_max x fm_env) / 2
 *   slew = 0.005 (Q28) + (fm_max x 0.015) / 4;  fm += clamp(target - fm, -slew, slew)
 *   cinc += cstep;  minc += mstep;  x = (prev x phase_fb) << 1
 *   mphase += minc + (((minc >> 1) x x) << 1);  cphase += cinc;  fbv += fb_step
 *   mfb = fbv > 0 ? fbv x fbv (the exact product) : 0
 *   mod = Sine(mphase + ((mfb x prev) << 2));  car = Sine(cphase + ((fm x mod) << 4))
 *   prev += ((car >> 1) - prev) x 0.1
 *   gain += (1 + env x (ae - 1) - gain) x (0.005 + (fm x 0.045) << 3)
 *   out = ((car >> 1) + (mod >> 1)) x gain >> 3
 * x is MulQ31Fast (MUL below, as src/fixed.h's), but the one MulQ31 (MULX, __builtin_bfin_mult_fr1x32x32 as GCC 14
 * emits it). Sine: the 4097-entry table, index phase >> 20, linear between two entries by (phase & 0xfffff) << 11.
 * Adds and shifts wrap, as the C's do.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */

#define MUL(d, a, b)                                                       \
	A0 = a.H * b.H, A1 = a.H * b.L (M);                                \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

#define MULX(d, a, b)                                                      \
	A1 = a.L * b.L (FU);                                               \
	A1 = A1 >> 16;                                                     \
	A0 = a.H * b.H, A1 += a.H * b.L (M);                               \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

/* The state, at P5. */
#define K_AE 0
#define K_BE 4
#define K_BR 8
#define K_FM 12
#define K_CINC 16
#define K_MINC 20
#define K_MPHASE 24
#define K_CPHASE 28
#define K_FBV 32
#define K_PREV 36
#define K_GAIN 40
#define K_ADECAY 44
#define K_BDECAY 48
#define K_BRSTEP 52
#define K_ENV 56
#define K_CSTEP 60
/* At P4 = k + 64. */
#define Q_MSTEP 0
#define Q_FBSTEP 4
#define Q_PHASEFB 8
#define Q_K0_1 12
#define Q_K0_005 16
#define Q_K0_045 20
#define Q_K0_015 24
#define Q_K0_005Q28 28
#define Q_HALF 32
#define Q_ONE 36
#define Q_SINE 40

	.text
	.align 8
	.global _FmLoopAsm
	.type _FmLoopAsm, STT_FUNC
_FmLoopAsm:
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;
	I0 = R1;
	P1 = R2;
	P4 = P5;
	P4 += 32;
	P4 += 32;
	P3 = [P4 + Q_SINE];

	LSETUP (.Lfm_start, .Lfm_end) LC0 = P1;
.Lfm_start:
	/* The envelopes. */
	R0 = [P5 + K_AE];
	R1 = [P5 + K_ADECAY];
	MUL(R2, R0, R1)
	R0 = R0 - R2;
	[P5 + K_AE] = R0;
	R0 = [P5 + K_BE];
	R1 = [P5 + K_BDECAY];
	MUL(R2, R0, R1)
	R0 = R0 - R2;
	[P5 + K_BE] = R0;

	/* The phases. */
	R0 = [P5 + K_CINC];
	R1 = [P5 + K_CSTEP];
	R0 = R0 + R1;
	[P5 + K_CINC] = R0;
	R1 = [P5 + K_CPHASE];
	R1 = R1 + R0;
	[P5 + K_CPHASE] = R1;           /* cphase */
	R5 = [P5 + K_PREV];             /* R5 = prev (the old one, to the end) */
	R1 = [P4 + Q_PHASEFB];
	MUL(R2, R5, R1)
	R2 <<= 1;                       /* x */
	R0 = [P5 + K_MINC];
	R1 = [P4 + Q_MSTEP];
	R0 = R0 + R1;                   /* minc */
	[P5 + K_MINC] = R0;
	R1 = R0 >> 1;
	MUL(R1, R1, R2)
	R1 <<= 1;
	R0 = R0 + R1;
	R1 = [P5 + K_MPHASE];
	R1 = R1 + R0;
	[P5 + K_MPHASE] = R1;           /* mphase */

	/* The modulator's phase: R6 its fraction, P0 its index (the table read waits for the FM amount below). */
	R0 = [P5 + K_FBV];
	R2 = [P4 + Q_FBSTEP];
	R0 = R0 + R2;
	[P5 + K_FBV] = R0;
	MULX(R2, R0, R0)
	CC = R0 <= 0;
	R0 = 0;
	IF CC R2 = R0;                  /* mfb */
	MUL(R2, R2, R5)
	R2 <<= 2;
	R6 = R1 + R2;
	R0 = R6 >> 20;
	P0 = R0;
	R6 <<= 12;
	R6 >>= 1;

	/* The FM amount (R7). */
	R3 = [P5 + K_BR];
	R1 = [P5 + K_BRSTEP];
	R3 = R3 + R1;
	[P5 + K_BR] = R3;
	MUL(R3, R3, R3)                 /* b2 */
	P0 = P3 + (P0 << 2);
	R1 = [P4 + Q_HALF];
	R4 = MIN (R3, R1);              /* fm_max */
	R3 = R3 - R4;                   /* fm_min */
	R0 = [P5 + K_BE];
	R0 = R0 - R1;
	R2 = [P5 + K_ENV];
	MUL(R0, R2, R0)
	R0 = R0 + R1;                   /* fm_env */
	MUL(R0, R4, R0)
	R0 >>>= 1;
	R3 >>>= 1;
	R3 = R3 + R0;                   /* target */
	R1 = [P4 + Q_K0_015];
	MUL(R0, R4, R1)
	R0 >>>= 2;
	R1 = [P4 + Q_K0_005Q28];
	R0 = R0 + R1;                   /* slew */
	R7 = [P5 + K_FM];
	R3 = R3 - R7;                   /* error */
	R3 = MIN (R3, R0);
	R0 = -R0;
	R3 = MAX (R3, R0);
	R7 = R7 + R3;                   /* R7 = fm_amount */
	[P5 + K_FM] = R7;

	/* The modulator (R1). */
	R2 = [P0];
	R3 = [P0 + 4];
	R3 = R3 - R2;
	MUL(R3, R3, R6)
	R1 = R2 + R3;

	/* The carrier's phase: R0 its fraction, P1 its index. */
	MUL(R0, R7, R1)
	R0 <<= 4;
	R2 = [P5 + K_CPHASE];
	R0 = R0 + R2;
	R2 = R0 >> 20;
	P1 = R2;
	R0 <<= 12;
	R0 >>= 1;

	/* The gain (R6), while P1 settles. */
	R2 = [P5 + K_AE];
	R3 = [P4 + Q_ONE];
	R2 = R2 - R3;
	R4 = [P5 + K_ENV];
	MUL(R2, R4, R2)
	P1 = P3 + (P1 << 2);
	R2 = R2 + R3;                   /* gain target */
	R3 = [P4 + Q_K0_045];
	MUL(R4, R7, R3)
	R4 <<= 3;
	R3 = [P4 + Q_K0_005];
	R4 = R4 + R3;                   /* coefficient */
	R6 = [P5 + K_GAIN];
	R2 = R2 - R6;
	MUL(R2, R2, R4)
	R6 = R6 + R2;
	[P5 + K_GAIN] = R6;

	/* The carrier (R0), the previous sample, the output. */
	R2 = [P1];
	R3 = [P1 + 4];
	R3 = R3 - R2;
	MUL(R3, R3, R0)
	R0 = R2 + R3;
	R0 >>>= 1;                      /* car >> 1 */
	R2 = R0 - R5;
	R3 = [P4 + Q_K0_1];
	MUL(R2, R2, R3)
	R5 = R5 + R2;
	[P5 + K_PREV] = R5;
	R1 >>>= 1;
	R0 = R0 + R1;
	MUL(R0, R0, R6)
	R0 >>>= 3;
.Lfm_end:
	[I0++] = R0;

	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	RTS;
	.size _FmLoopAsm, .-_FmLoopAsm
