/*
 * The half-rate path's Blackfin kernels, one source for every plugin that runs Clouds' half-band pair (tape_echo,
 * cs_hall, cs_plate, ensemble, petri: each lists ../shared/halfband_bfin.S in plugin.json "sources"). The arithmetic
 * is that of te-op1-cfw ports/clouds/src/core/halfband.cc (Mutable Instruments Clouds, MIT: the plugins'
 * LICENSE-MIT) as each plugin's C reference has it: HalfbandDecimateC, HalfbandInterpolateC, MixSoftLimitC.
 *
 * void HalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
 * void HalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
 * void MixSoftLimitAsm(int16_t* io, const int16_t* wet, int32_t n,
 *                      int32_t gains, const SoftLimitTable* t);
 *
 * 2026-11-22 (efficiency audit finding 15): the decimator and the interpolator make two outputs a pass, one in each
 * MAC: every input word feeds both outputs' products in one dual MAC (the two taps it meets, from a circular stream of
 * tap pairs in I3), so a tap costs half a packet, with the loads in the parallel slots. Before: four instructions a
 * tap, one output at a time, no parallel loads (27 packets an output; now 7.5).
 *
 * Contract (the plugins' HalfbandDecimator / HalfbandInterpolator meet it: alignas(4) buffers, n = 32):
 *   - x and out 4-byte aligned, n even and > 0 (the kernels read words and write words);
 *   - the decimator reads x[0 .. 2n + 17] (and its last pass's look-ahead word, still inside), writes out[0 .. n-1];
 *   - the interpolator reads x[0 .. n + 9] (x[n + 9] one halfword past its C reference: the buffer's padding),
 *     writes out[0 .. 2n - 1].
 *
 * Exactness: the products are 16 x 16 fractional ((a b) << 1; no operand pair is -1 x -1, every tap is under 2^15),
 * exact in the 40-bit accumulators. The decimator's centre and rounding (x[c] x 16384 + 16384, then >> 15) and the
 * interpolator's (8192, >> 14; its taps doubled, so 4 h x) put y << 16 in the accumulator, and the truncating
 * extraction (T) takes floor(y / 2^16) saturated to 16 bits: Sat16(y >> 15) and Sat16(y >> 14), as the C.
 * Checked over random and edge input against the C references by tests/host/halfband (simulator), and in each
 * plugin's own simulator comparison.
 *
 * Output stores ride one pass late, through a circular I1 over out (the first pass's stores land on the last word(s)
 * and are rewritten after the loop: docs/plugin-performance.md "the output store rotated without a peeled pass").
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
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

/*
 * The tap-pair streams: (low half -> A0, the pass's first output; high half -> A1, its second), in packet order.
 * g = h4 h3 h2 h1 h0 h0 h1 h2 h3 h4 with h = 10019 -2397 710 -151 11 (Q15): output k of the decimator is
 * sum g[j] x[2(k + j) + 1] + 16384 x[2k + 10]; even output 2m of the interpolator is sum g[j] x[m + j].
 */
	.section .rodata._HalfbandTaps, "a", @progbits
	.align 4
.Ldec_taps:                             /* the decimator: word k + j's odd sample (its centre: the even one) */
	.short 11, 0                    /* j 0 */
	.short -151, 11
	.short 710, -151
	.short -2397, 710
	.short 10019, -2397
	.short 10019, 10019             /* j 5 */
	.short 16384, 0                 /* output k's centre: word k + 5's even sample */
	.short -2397, 10019             /* j 6 */
	.short 0, 16384                 /* output k + 1's centre: word k + 6's even sample */
	.short 710, -2397
	.short -151, 710
	.short 11, -151
	.short 0, 11                    /* j 10 */
.Lint_taps:                             /* the interpolator: x[m + j], 2 g (the extraction's extra bit) */
	.short 22, 0                    /* j 0 */
	.short -302, 22
	.short 1420, -302
	.short -4794, 1420
	.short 20038, -4794
	.short 20038, 20038
	.short -4794, 20038
	.short 1420, -4794
	.short -302, 1420
	.short 22, -302
	.short 0, 22                    /* j 10 */

/*
 * The decimator. Output pair k, k + 1 (k even) reads words W[k .. k + 10] (W[j] = x[2j] | x[2j + 1] << 16).
 * I0 -> the words (rewound 8 words by M0 at the pass's last load), I1 circular over out, I3 circular over the 13
 * tap pairs; R0 / R1 the words, R5 the tap pair, R7 = 1 | 16384 << 16 (the rounding: 1 x 16384, doubled), R2 the
 * pair of outputs.
 */
	.section .text._HalfbandDecimateAsm, "ax", @progbits
	.align 4
	.global _HalfbandDecimateAsm
	.type _HalfbandDecimateAsm, STT_FUNC
_HalfbandDecimateAsm:
	PROLOGUE
	I0 = R0;                        /* x */
	B1 = R1;                        /* out: n halfwords, n / 2 words */
	R3 = R2 << 1;
	L1 = R3;
	R3 += -4;
	R1 = R1 + R3;
	I1 = R1;                        /* the last word: the first pass's store */
	P1 = R2;
	P1 = P1 >> 1;                   /* passes */
	R0.L = .Ldec_taps;
	R0.H = .Ldec_taps;
	B3 = R0;
	I3 = R0;
	L3 = 52 (X);
	M0 = -32 (X);
	R7.L = 1;
	R7.H = 16384;
	R0 = [I0++] || R5 = [I3++];
	LSETUP (.Ld_start, .Ld_end) LC0 = P1;
.Ld_start:
	A1 = R7.L * R7.H, A0 = R7.L * R7.H || [I1++] = R2 || NOP;
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 0 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 1 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 2 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 3 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 4 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R1 = [I0++] || R5 = [I3++];     /* j 5 */
	A0 += R0.L * R5.L || R0 = [I0++] || R5 = [I3++];                        /* k's centre */
	A1 += R1.H * R5.H, A0 += R1.H * R5.L || NOP || R5 = [I3++];             /* j 6 */
	A1 += R1.L * R5.H || NOP || R5 = [I3++];                                /* k + 1's centre */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 7 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 8 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0 ++ M0] || R5 = [I3++]; /* j 9 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 10; the next pass's first word */
.Ld_end:
	R2.H = A1, R2.L = A0 (T);
	[I1++] = R2;                    /* the last pass's pair (I1 wrapped to the last word) */
	L1 = 0;
	L3 = 0;
	EPILOGUE
	.size _HalfbandDecimateAsm, .-_HalfbandDecimateAsm

/*
 * The interpolator. Pass q makes out[4q .. 4q + 3]: y(2q), x[2q + 5], y(2q + 1), x[2q + 6], from words
 * V[q .. q + 5] (V[i] = x[2i] | x[2i + 1] << 16). I0 -> the words (rewound 4 words by M0), I1 circular over out
 * (two words a pass), I3 circular over the 11 tap pairs; R0 / R1 the words, R2 = V[q + 2] and R3 = V[q + 3] kept for
 * the odd outputs, R5 the tap pair, R7 = 1 | 16384 << 16, R6 the outputs, R4 / R6 the two output words.
 */
	.section .text._HalfbandInterpolateAsm, "ax", @progbits
	.align 4
	.global _HalfbandInterpolateAsm
	.type _HalfbandInterpolateAsm, STT_FUNC
_HalfbandInterpolateAsm:
	PROLOGUE
	I0 = R0;                        /* x */
	B1 = R1;                        /* out: 2n halfwords, n words */
	R3 = R2 << 2;
	L1 = R3;
	R3 += -8;
	R1 = R1 + R3;
	I1 = R1;                        /* the last two words: the first pass's stores */
	P1 = R2;
	P1 = P1 >> 1;                   /* passes */
	R0.L = .Lint_taps;
	R0.H = .Lint_taps;
	B3 = R0;
	I3 = R0;
	L3 = 44 (X);
	M0 = -16 (X);
	R7.L = 1;
	R7.H = 16384;
	R0 = [I0++] || R5 = [I3++];
	LSETUP (.Li_start, .Li_end) LC0 = P1;
.Li_start:
	A1 = R7.L * R7.H, A0 = R7.L * R7.H || [I1++] = R4 || NOP;
	A1 += R0.L * R5.H, A0 += R0.L * R5.L || [I1++] = R6 || R5 = [I3++];     /* j 0 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 1 */
	A1 += R0.L * R5.H, A0 += R0.L * R5.L || R2 = [I0++] || R5 = [I3++];     /* j 2 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R3 = [I0++] || R5 = [I3++];     /* j 3 */
	A1 += R2.L * R5.H, A0 += R2.L * R5.L || NOP || R5 = [I3++];             /* j 4 */
	A1 += R2.H * R5.H, A0 += R2.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 5 */
	A1 += R3.L * R5.H, A0 += R3.L * R5.L || R1 = [I0 ++ M0] || R5 = [I3++]; /* j 6 */
	A1 += R3.H * R5.H, A0 += R3.H * R5.L || NOP || R5 = [I3++];             /* j 7 */
	A1 += R0.L * R5.H, A0 += R0.L * R5.L || NOP || R5 = [I3++];             /* j 8 */
	A1 += R0.H * R5.H, A0 += R0.H * R5.L || R0 = [I0++] || R5 = [I3++];     /* j 9; the next pass's first word */
	A1 += R1.L * R5.H || NOP || R5 = [I3++];                                /* j 10 */
	R6.H = A1, R6.L = A0 (T);
	R4 = PACK (R2.H, R6.L);         /* y(2q) | x[2q + 5] << 16 */
.Li_end:
	R6 = PACK (R3.L, R6.H);         /* y(2q + 1) | x[2q + 6] << 16 */
	[I1++] = R4;                    /* the last pass's words (I1 wrapped to the last two) */
	[I1++] = R6;
	L1 = 0;
	L3 = 0;
	EPILOGUE
	.size _HalfbandInterpolateAsm, .-_HalfbandInterpolateAsm

/*
 * The mix, as before (unchanged from te-op1-cfw ports/clouds/src/core/halfband_bfin.S 5aa314b).
 * Registers: P0 -> io, P1 -> wet, P3 -> t->value (t->step at +514), R7 = gains, R6 = 2^29 - 1 (the table's end),
 * R5 = 0x7fff.
 */
	.section .text._MixSoftLimitAsm, "ax", @progbits
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
