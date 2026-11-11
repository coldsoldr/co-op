/*
 * coop: copied from te-op1-cfw ports/fx/src/tape; only the include path changed.
 *
 * Blackfin kernels for TAPE (src/tape/tape.h): the same arithmetic as the
 * C references in tape.cc, bit for bit (tests/test_tape.cc checks it).
 * RND_MOD is cleared on entry (unbiased rounding) and ASTAT restored on
 * exit. L registers stay 0. n >= 1.
 *
 * Learnt here (bfin-elf-run): two half-register loads into the same
 * register in one instruction keep only one of them.
 */

#include "src/tape_layout.h"

#define ROUNDING_ENTER                                                     \
	R3 = ASTAT;                                                        \
	[--SP] = R3;                                                       \
	BITCLR (R3, 8);                                                    \
	ASTAT = R3;

#define ROUNDING_EXIT                                                      \
	R3 = [SP++];                                                       \
	ASTAT = R3;

#define SAVE                                                               \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;

#define RESTORE                                                            \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

	.text

/*
 * void TapeDelayRunAsm(TapeDelayKernel* k, int32_t* ring, const int16_t* l,
 *                      const int16_t* r, int16_t* wl, int16_t* wr, int32_t n);
 *
 * The run must not wrap the write index: widx >= n (TapeSat::Process
 * splits). P4 -> k, P5 = ring, P1 the next write (moving down), P2 its
 * mirror, P0 the read; I0 -> l, I1 -> r, I2 -> wl, I3 -> wr (each output
 * stored during the next sample: the streams start at wl[-1], wr[-1],
 * which get their own values back); R6 = pos, R5 = step - 65536, R7.L =
 * the EXTRACT pattern of pos's bits 16..23 (the read index).
 */
	.align 4
	.global _TapeDelayRunAsm
	.type _TapeDelayRunAsm, STT_FUNC
_TapeDelayRunAsm:
	SAVE
	P4 = R0;
	P5 = R1;
	I0 = R2;
	ROUNDING_ENTER
	R0 = [FP + 20];
	I1 = R0;
	R0 = [FP + 24];
	R0 += -2;
	I2 = R0;
	R0 = [FP + 28];
	R0 += -2;
	I3 = R0;
	P3 = [FP + 32];                 /* n */
	R0 = [P4 + TDK_WIDX];
	R1 = P3;
	P1 = R0;
	R0 = R0 - R1;
	[P4 + TDK_WIDX] = R0;
	P1 = P5 + (P1 << 2);
	P1 += -4;
	P2 = 4 * TAPE_RING (X);
	P2 = P1 + P2;
	R6 = [P4 + TDK_POS];
	R5 = [P4 + TDK_STEP];
	R0.L = 0;
	R0.H = 1;                       /* 65536 */
	R5 = R5 - R0;
	R7 = 0x1008 (Z);                /* bits 16.., 8 of them */
	R2.L = W[I2];
	R2.H = W[I3];

	LSETUP (.Ld_start, .Ld_end) LC0 = P3;
.Ld_start:
	R6 = R6 + R5 (NS) || R0.L = W[I0++] || W[I2++] = R2.L;
	R1 = EXTRACT (R6, R7.L) (Z) || R0.H = W[I1++] || W[I3++] = R2.H;
	R4 = ~R6;
	P0 = R1;
	R4 = PACK (R6.L, R4.L) || [P1--] = R0 || NOP;
	R4 = R4 >> 2 (V) || [P2--] = R0 || NOP;
	P0 = P5 + (P0 << 2);
	R2 = [P0];
	A1 = R2.H * R4.L, A0 = R2.L * R4.L || R3 = [P0 + 4] || NOP;
.Ld_end:
	R2.H = (A1 += R3.H * R4.H), R2.L = (A0 += R3.L * R4.H);
	W[I2++] = R2.L;
	W[I3++] = R2.H;

	[P4 + TDK_POS] = R6;
	ROUNDING_EXIT
	RESTORE
	.size _TapeDelayRunAsm, .-_TapeDelayRunAsm

/*
 * void TapeDelayRunMonoAsm(TapeDelayKernel* k, int32_t* ring,
 *                          const int16_t* l, int16_t* wl, int32_t n);
 *
 * As above (wl[-1] gets its own value back); the ring words get l in the
 * low half, 0 in the high one.
 */
	.align 4
	.global _TapeDelayRunMonoAsm
	.type _TapeDelayRunMonoAsm, STT_FUNC
_TapeDelayRunMonoAsm:
	SAVE
	P4 = R0;
	P5 = R1;
	I0 = R2;
	ROUNDING_ENTER
	R0 = [FP + 20];
	R0 += -2;
	I2 = R0;
	P3 = [FP + 24];                 /* n */
	R0 = [P4 + TDK_WIDX];
	R1 = P3;
	P1 = R0;
	R0 = R0 - R1;
	[P4 + TDK_WIDX] = R0;
	P1 = P5 + (P1 << 2);
	P1 += -4;
	P2 = 4 * TAPE_RING (X);
	P2 = P1 + P2;
	R6 = [P4 + TDK_POS];
	R5 = [P4 + TDK_STEP];
	R0.L = 0;
	R0.H = 1;
	R5 = R5 - R0;
	R7 = 0x1008 (Z);
	R0 = 0;
	R2.L = W[I2];

	LSETUP (.Ldm_start, .Ldm_end) LC0 = P3;
.Ldm_start:
	R6 = R6 + R5 (NS) || R0.L = W[I0++] || W[I2++] = R2.L;
	R1 = EXTRACT (R6, R7.L) (Z);
	R4 = ~R6;
	P0 = R1;
	R4 = PACK (R6.L, R4.L) || [P1--] = R0 || NOP;
	R4 = R4 >> 2 (V) || [P2--] = R0 || NOP;
	P0 = P5 + (P0 << 2);
	R2 = W[P0] (X);
	A0 = R2.L * R4.L || R3 = W[P0 + 4] (X) || NOP;
.Ldm_end:
	R2.L = (A0 += R3.L * R4.H);
	W[I2++] = R2.L;

	[P4 + TDK_POS] = R6;
	ROUNDING_EXIT
	RESTORE
	.size _TapeDelayRunMonoAsm, .-_TapeDelayRunMonoAsm

/*
 * The halfband's two allpass sections, both paths packed in a register's
 * halves: states X (the previous input), E1, E2; the new input XN; Y1, Y2
 * out (the next E1, E2); R6, R7 the coefficient pairs. The second section
 * rounds towards zero: d = Y1 - E2, acc = 2 c2 d, f = d < 0 ? -1 : 0,
 * Y2 = trunc(acc + 2 (f - E1) x -1.0); -1.0 comes from [I3] into E2's
 * register once E2 is spent. NEXTLOAD: the next input's load (into a
 * register free once Y1 is made). HB_LAST is the last instruction, apart
 * so that a loop's end label can sit on it.
 */
#define HB_REST(X, Y1)                                                     \
	Y1.H = Y1.H * R6.H, Y1.L = Y1.L * R6.L;                            \
	Y1 = X +|+ Y1 (S);

#define HB_SECOND(E1, E2, Y1, Y2, NEXTLOAD)                                \
	Y2 = Y1 -|- E2 (S) || NEXTLOAD || NOP;                             \
	A1 = Y2.H * R7.H, A0 = Y2.L * R7.L || E2 = [I3] || NOP;            \
	Y2 = Y2 >>> 15 (V);                                                \
	Y2 = Y2 -|- E1 (S);

#define HB_LAST(E2, Y2)                                                    \
	Y2.H = (A1 += Y2.H * E2.L), Y2.L = (A0 += Y2.L * E2.L) (T);

/* Up: x | x from the loaded low half; the previous output's halves stored
 * on the way (E2.L, then E2.H). */
#define UP_HALF(X, E1, E2, XN, Y1, Y2, NEXT)                               \
	XN = PACK (XN.L, XN.L) || W[I1++] = E2.L || NOP;                   \
	Y1 = XN -|- E1 (S) || W[I1++] = E2.H || NOP;                       \
	HB_REST(X, Y1)                                                     \
	HB_SECOND(E1, E2, Y1, Y2, NEXT.L = W[I0++])

/* Down: the input pair whole; the previous output (the sum of E2's
 * halves, into Y2's register) stored on the way. */
#define DOWN_HALF(X, E1, E2, XN, Y1, Y2, NEXT)                             \
	Y2.L = E2.L + E2.H (S);                                            \
	Y1 = XN -|- E1 (S) || W[I1++] = Y2.L || NOP;                       \
	HB_REST(X, Y1)                                                     \
	HB_SECOND(E1, E2, Y1, Y2, NEXT = [I0++])

#define HB_LOAD(OFF)                                                       \
	R0 = [P5 + (OFF) + THK_X];                                         \
	R1 = [P5 + (OFF) + THK_S1];                                        \
	R2 = [P5 + (OFF) + THK_S2];                                        \
	R6 = [P5 + (OFF) + THK_C1];                                        \
	R7 = [P5 + (OFF) + THK_C2];

#define HB_STORE(OFF)                                                      \
	[P5 + (OFF) + THK_X] = R0;                                         \
	[P5 + (OFF) + THK_S1] = R1;                                        \
	[P5 + (OFF) + THK_S2] = R2;

/*
 * void TapeLaneAsm(TapeLaneKernel* k, const TapeSatKernel* sat, int16_t* w,
 *                  int32_t* u, int16_t* io, int32_t n);
 *
 * TapeLaneRunC: up (w -> u), sat (u), down (u -> w), the tone (w), the mix
 * (w, io -> io). n even. P5 -> k, P4 -> sat, P3 = w, P2 = n / 2; u and io
 * stay on the stack; I3 -> -32768 (pushed). The scratch words: u[-1],
 * w[-1]; w[n] and u[n] are read.
 */
	.align 4
	.global _TapeLaneAsm
	.type _TapeLaneAsm, STT_FUNC
_TapeLaneAsm:
	SAVE
	P5 = R0;
	P4 = R1;
	P3 = R2;
	ROUNDING_ENTER
	R3 = -32768 (X);
	[--SP] = R3;
	I3 = SP;
	P2 = [FP + 28];                 /* n */
	P2 = P2 >> 1;                   /* pairs */

	/* Up: w -> u. The halfword stores run from u[-1]'s high half on:
	 * odd, even, odd, even...; states in R0-R2 (A) and R3-R5 (B). */
	I0 = P3;
	R0 = [FP + 20];
	R0 += -2;
	I1 = R0;
	HB_LOAD(TLK_UP)
	R3.L = W[I0++];
	LSETUP (.Lu_start, .Lu_end) LC0 = P2;
.Lu_start:
	UP_HALF(R0, R1, R2, R3, R4, R5, R0)
	HB_LAST(R2, R5)
	UP_HALF(R3, R4, R5, R0, R1, R2, R3)
.Lu_end:
	HB_LAST(R5, R2)
	W[I1++] = R2.L;
	HB_STORE(TLK_UP)

	/* Saturation, u in place: z = sat((u h) << shift); X = z z;
	 * T = K1 X + K2 X^2 + K0 (rounded); out = sat((z T) << j). Each result
	 * is stored during the next one (u[-1] gets 0). R5 = h | shift,
	 * R6 = K1 | K2, R7 = K0 | 32767, R4 = j. */
	R0 = [FP + 20];
	I0 = R0;
	R0 += -4;
	I1 = R0;
	R5 = [P4 + TSK_HSH];
	R6 = [P4 + TSK_K12];
	R7 = [P4 + TSK_K0];
	R4 = [P4 + TSK_J];
	R3 = 0;
	R0 = [I0++];
	P1 = P2 << 1;
	LSETUP (.Ls_start, .Ls_end) LC0 = P1;
.Ls_start:
	R1.H = R0.H * R5.H, R1.L = R0.L * R5.H || [I1++] = R3 || R0 = [I0++];
	R1 = ASHIFT R1 BY R5.L (V, S);
	R2.H = R1.H * R1.H, R2.L = R1.L * R1.L;
	R3.H = R2.H * R2.H, R3.L = R2.L * R2.L;
	A1 = R2.H * R6.H, A0 = R2.L * R6.H;
	A1 += R3.H * R6.L, A0 += R3.L * R6.L;
	R3.H = (A1 += R7.H * R7.L), R3.L = (A0 += R7.H * R7.L);
	R3.H = R1.H * R3.H, R3.L = R1.L * R3.L;
.Ls_end:
	R3 = ASHIFT R3 BY R4.L (V, S);
	[I1++] = R3;

	/* Down: u -> w. w[-1] gets the state's sum (scratch). */
	R0 = [FP + 20];
	I0 = R0;
	R0 = P3;
	R0 += -2;
	I1 = R0;
	HB_LOAD(TLK_DOWN)
	R3 = [I0++];
	LSETUP (.Ldn_start, .Ldn_end) LC0 = P2;
.Ldn_start:
	DOWN_HALF(R0, R1, R2, R3, R4, R5, R0)
	HB_LAST(R2, R5)
	DOWN_HALF(R3, R4, R5, R0, R1, R2, R3)
.Ldn_end:
	HB_LAST(R5, R2)
	R3.L = R2.L + R2.H (S);
	W[I1++] = R3.L;
	HB_STORE(TLK_DOWN)

	/* The tone, w in place (the stores lag one: w[-1] gets the old s2).
	 * R0 = x, R2 temporary, R3 = lpb | lpa, R4 = s1, R5 = s2 (Q16 +
	 * 0x8000: the high halves are the rounded outputs), R6 = gb | k,
	 * R7 = kb | ka; A0, A1 the bump's low-passes. */
	I0 = P3;
	R0 = P3;
	R0 += -2;
	I2 = R0;
	R0 = [P5 + TLK_TONE + TTK_A0];
	A0 = R0;
	R1 = [P5 + TLK_TONE + TTK_A1];
	A1 = R1;
	R3 = [P5 + TLK_TONE + TTK_LP];
	R4 = [P5 + TLK_TONE + TTK_S1];
	R5 = [P5 + TLK_TONE + TTK_S2];
	R6 = [P5 + TLK_TONE + TTK_GK];
	R7 = [P5 + TLK_TONE + TTK_KAB];
	R0.L = W[I0++];
	P1 = P2 << 1;
	LSETUP (.Lt_start, .Lt_end) LC0 = P1;
.Lt_start:
	/* The bump: y = x + gb (lpa - lpb). */
	R2 = PACK (R0.L, R0.L) || W[I2++] = R5.H || NOP;
	R2 = R2 -|- R3 (S);
	R3.H = (A1 += R2.H * R7.H), R3.L = (A0 += R2.L * R7.L);
	R2.L = R3.L - R3.H (S);
	R2.L = R2.L * R6.H;
	R0.L = R0.L + R2.L (S);
	/* HF loss: s += 2 k (in - round(s)), twice. */
	R2.L = R0.L - R4.H (S);
	R2 = R2.L * R6.L || R0.L = W[I0++] || NOP;
	R4 = R4 + R2 (S);
	R2.L = R4.H - R5.H (S);
	R2 = R2.L * R6.L;
.Lt_end:
	R5 = R5 + R2 (S);
	W[I2++] = R5.H;
	R0 = A0;
	[P5 + TLK_TONE + TTK_A0] = R0;
	R1 = A1;
	[P5 + TLK_TONE + TTK_A1] = R1;
	[P5 + TLK_TONE + TTK_LP] = R3;
	[P5 + TLK_TONE + TTK_S1] = R4;
	[P5 + TLK_TONE + TTK_S2] = R5;

	/* The mix, two samples at a time: io = dry - dry m + wet m. R0 = the
	 * wet pair, R1 = the dry pair, R7 = the pair's mixes (m0 + 2 step |
	 * m0 + step, then + 2 step a pair), R6 = 2 step | 2 step; R2, R3
	 * temporaries (R3 the result: low half stored at once, high half
	 * during the next pair). Loads run one pair ahead. */
	R0 = [P5 + TLK_TONE + TTK_MIX];
	R1 = R0 >>> 16;                 /* step */
	R2 = R0.L (X);                  /* m0 */
	R2 = R2 + R1;
	R3 = R2 + R1;
	R7 = PACK (R3.L, R2.L);
	R6 = R1 + R1;
	R6 = PACK (R6.L, R6.L);
	[--SP] = R1;
	I0 = P3;
	R0 = [FP + 24];
	I1 = R0;
	I2 = R0;
	R0 = [I0++];
	R1.L = W[I1++];
	R1.H = W[I1++];
	P2 += -1;
	CC = P2 == 0;
	IF CC JUMP .Lm_one;
	R2.H = R1.H * R7.H, R2.L = R1.L * R7.L;
	R3.H = R0.H * R7.H, R3.L = R0.L * R7.L;
	R2 = R1 -|- R2 (S) || R0 = [I0++] || NOP;
	R3 = R2 +|+ R3 (S) || R1.L = W[I1++] || NOP;
	R7 = R7 +|+ R6 (S) || W[I2++] = R3.L || R1.H = W[I1++];
	P2 += -1;
	CC = P2 == 0;
	IF CC JUMP .Lm_last;
	LSETUP (.Lm_start, .Lm_end) LC0 = P2;
.Lm_start:
	R2.H = R1.H * R7.H, R2.L = R1.L * R7.L || W[I2++] = R3.H || NOP;
	R3.H = R0.H * R7.H, R3.L = R0.L * R7.L;
	R2 = R1 -|- R2 (S) || R0 = [I0++] || NOP;
	R3 = R2 +|+ R3 (S) || R1.L = W[I1++] || NOP;
.Lm_end:
	R7 = R7 +|+ R6 (S) || W[I2++] = R3.L || R1.H = W[I1++];
.Lm_last:
	R2.H = R1.H * R7.H, R2.L = R1.L * R7.L || W[I2++] = R3.H || NOP;
	JUMP .Lm_tail;
.Lm_one:
	R2.H = R1.H * R7.H, R2.L = R1.L * R7.L;
.Lm_tail:
	R3.H = R0.H * R7.H, R3.L = R0.L * R7.L;
	R2 = R1 -|- R2 (S);
	R3 = R2 +|+ R3 (S);
	W[I2++] = R3.L;
	W[I2++] = R3.H;
	R1 = [SP++];                    /* step */
	R0 = PACK (R1.L, R7.H);         /* step | the last sample's mix */
	[P5 + TLK_TONE + TTK_MIX] = R0;

	SP += 4;
	ROUNDING_EXIT
	RESTORE
	.size _TapeLaneAsm, .-_TapeLaneAsm
