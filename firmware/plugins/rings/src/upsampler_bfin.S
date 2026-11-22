/*
 * The 4x upsampler's whole render (rings.cc Upsampler::Process, the 44.1 kHz voice: OP1RINGS_VOICE_44K) in one
 * Blackfin kernel (coop rings 0.14): what rings 0.2-0.13 did in C around HalfbandAsm (src/halfband_bfin.S), and the
 * screen's peak. Bit-exact with the C (tests/host/rings: the simulator against the host).
 *
 * int32_t UpsampleAsm(const int32_t* in, int16_t* history, int32_t* mid_history, int16_t* out, int16_t* work);
 *   in: 32 Q27 samples (22.05 kHz); history: the last 24 inputs (16-bit), read and written; mid_history: the last
 *   three 44.1 kHz samples, read and written; out: 64 samples (44.1 kHz); work: the scratch's upsampler block
 *   (src/scratch.h: x[56], xs[56] int16, then mid[67] int32 at byte 224; 4-byte aligned).
 *   Returns max |out[k]| (rings.cc ScreenRendered's peak).
 *
 * The steps, as the C:
 *   x[0..23] = history; x[24 + i] = Saturate16(in[i] >> 12), here (in << 4, saturated) >> 16, the same integer;
 *   xs[i] = x[i + 1] (i < 55); mid[0..2] = mid_history;
 *   the half-band: mid[3 + 2m] = x[m + 12], mid[4 + 2m] = (8192 + sum_j c_j (x[m + 12 - j] + x[m + 13 + j])) >> 14,
 *     for m = 0 .. 31: HalfbandAsm's two passes (odd m from x, even m from xs) as one loop body run twice;
 *   out[k] = Saturate16(mid[k + 1]), k < 64; history = x[32..55]; mid_history = mid[64..66].
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#define W_XS 112
#define W_MID 224

	.section .rodata
	.align 4
.Lup_taps:
	.short 10380, -3328, 1846, -1169, 772, -510, 330, -205, 120, -64, 29, -9   /* src/halfband_taps.h, c0..c11 */
	.text
	.align 8
	.global _UpsampleAsm
	.type _UpsampleAsm, STT_FUNC
_UpsampleAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	[--SP] = LC1;
	[--SP] = LT1;
	[--SP] = LB1;
	I1 = R0;                        /* in */
	P5 = R1;                        /* history */
	I2 = R1;
	P3 = R2;                        /* mid_history */
	P4 = [FP + 24];                 /* work: x */
	/* x[0..23] = history */
	P0 = P5;
	P1 = P4;
	P2 = 12;
	LSETUP (.Lup_h_start, .Lup_h_end) LC0 = P2;
.Lup_h_start:
	R0 = [P0++];
.Lup_h_end:
	[P1++] = R0;
	/* x[24..55] = Saturate16(in >> 12) */
	P2 = 32;
	LSETUP (.Lup_in_start, .Lup_in_end) LC0 = P2;
.Lup_in_start:
	R0 = [I1++];
	R0 = R0 << 4 (S);
	R0 >>>= 16;
.Lup_in_end:
	W[P1++] = R0;
	/* xs[i] = x[i + 1], i < 55 */
	P0 = P4;
	P0 += 2;
	P1 = W_XS;
	P1 = P4 + P1;
	P2 = 55;
	LSETUP (.Lup_xs_start, .Lup_xs_end) LC0 = P2;
.Lup_xs_start:
	R0 = W[P0++] (X);
.Lup_xs_end:
	W[P1++] = R0;
	/* mid[0..2] = mid_history */
	P1 = W_MID;
	P1 = P4 + P1;                   /* mid */
	R0 = [P3];
	[P1] = R0;
	R0 = [P3 + 4];
	[P1 + 4] = R0;
	R0 = [P3 + 8];
	[P1 + 8] = R0;

	/* The half-band: the odd outputs m = 1, 3 .. 31 from x (base x + 2, mid3 + 2 m), then the even ones
	 * m = 0, 2 .. 30 from xs (base xs, mid3). */
	P2.H = .Lup_taps;
	P2.L = .Lup_taps;
	R2 = [P2];
	R3 = [P2 + 4];
	R4 = [P2 + 8];
	R5 = [P2 + 12];
	R6 = [P2 + 16];
	R7 = [P2 + 20];
	P5 = P1;
	P5 += 12;                       /* mid3 = &mid[3] */
	P0 = P4;
	P0 += 4;                        /* x + 2 = x[m + 1] for m = 1 */
	P1 = P5;
	P1 += 8;                        /* mid3 + 2 m, m = 1 */
	P2 = 16;                        /* outputs a pass */
	R1 = 2;
	LC1 = R1;
	LSETUP (.Lup_pass_start, .Lup_pass_end) LC1;
.Lup_pass_start:
	LSETUP (.Lup_hb_start, .Lup_hb_end) LC0 = P2;
.Lup_hb_start:
	I0 = P0;
	A1 = A0 = 0;
	R0 = [I0++];
	A0 += R0.L * R7.H (IS);
	R0 = (A0 += R0.H * R7.L) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R6.H (IS);
	R1 = (A1 += R1.H * R6.L) (IS) || R0 = [I0++] || NOP;
	A0 += R0.L * R5.H (IS);
	R0 = (A0 += R0.H * R5.L) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R4.H (IS);
	R1 = (A1 += R1.H * R4.L) (IS) || R0 = [I0++] || NOP;
	A0 += R0.L * R3.H (IS);
	R0 = (A0 += R0.H * R3.L) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R2.H (IS);
	R1 = (A1 += R1.H * R2.L) (IS) || R0 = [I0++] || NOP;
	A0 += R0.L * R2.L (IS);
	R0 = (A0 += R0.H * R2.H) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R3.L (IS);
	R1 = (A1 += R1.H * R3.H) (IS) || R0 = [I0++] || NOP;
	A0 += R0.L * R4.L (IS);
	R0 = (A0 += R0.H * R4.H) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R5.L (IS);
	R1 = (A1 += R1.H * R5.H) (IS) || R0 = [I0++] || NOP;
	A0 += R0.L * R6.L (IS);
	R0 = (A0 += R0.H * R6.H) (IS) || R1 = [I0++] || NOP;
	A1 += R1.L * R7.L (IS);
	R1 = (A1 += R1.H * R7.H) (IS);  /* the odd pairs' sum */
	R0 = A0 (IS);                   /* the even pairs' sum */
	R1 = R1 + R0;
	R0 = 8192 (X);
	R1 = R1 + R0;
	R1 >>>= 14;                     /* mid[4 + 2m] */
	R0 = W[P0 + 22] (X);            /* x[m + 12]: mid[3 + 2m] */
	[P1] = R0;
	[P1 + 4] = R1;
	P0 += 4;                        /* the pass's next output: two samples on */
	P1 += 16;
.Lup_hb_end:
	NOP;
	P0 = W_XS;
	P0 = P4 + P0;                   /* the even pass: xs */
	P1 = P5;                        /* mid3 */
.Lup_pass_end:
	NOP;

	/* out[k] = Saturate16(mid[k + 1]), and the peak; mid_history = mid[64..66] */
	P0 = P5;
	P0 += -8;                       /* mid[1] */
	P1 = [FP + 20];                 /* out */
	R6 = 0;
	P2 = 64;
	LSETUP (.Lup_out_start, .Lup_out_end) LC0 = P2;
.Lup_out_start:
	R0 = [P0++];
	R0 = R0 << 16 (S);
	R0 >>>= 16;
	W[P1++] = R0;
	R1 = ABS R0;
.Lup_out_end:
	R6 = MAX (R6, R1);
	R0 = [P0];                      /* P0 = mid[65] */
	R1 = [P0 + 4];
	R2 = [P0 + -4];
	[P3] = R2;
	[P3 + 4] = R0;
	[P3 + 8] = R1;
	/* history = x[32..55] */
	P0 = 64;
	P0 = P4 + P0;
	P1 = I2;
	P2 = 12;
	LSETUP (.Lup_t_start, .Lup_t_end) LC0 = P2;
.Lup_t_start:
	R0 = [P0++];
.Lup_t_end:
	[P1++] = R0;
	R0 = R6;
	LB1 = [SP++];
	LT1 = [SP++];
	LC1 = [SP++];
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _UpsampleAsm, .-_UpsampleAsm
