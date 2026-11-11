/*
 * coop port (plugins/cs_hall): te-op1-cfw ports/fx/src/hall/hall_bfin.S, unchanged but
 * for the include path and the mono kernels left out (HALL_MONO_KERNELS).
 *
 * Blackfin kernels for the hall's runs: the same arithmetic as HallRunC,
 * HallXfadeRunC and HallPreRunC (hall.cc; bit-exact, checked by
 * tests/test_hall.cc). The 16-bit MACs' default rounding (unbiased) is
 * what the C references model; RND_MOD is cleared on entry and restored
 * on exit, so the caller's ASTAT doesn't matter.
 *
 * p points at the buffer's write pointer; each sample first moves it down
 * one. Fixed accesses are immediate offsets from it (hall_layout.h); the
 * size-dependent ones come from the kernel block. In the mirrored region
 * every write also goes alias_bytes further on (P3 = p + alias); outside
 * it the ...Nm kernels write once (alias_bytes must be 0 for them). n > 0.
 * L registers stay 0.
 *
 * Registers in the sample loop:
 *   P2 = p, P3 = p + alias, P4 -> x, P5 -> the block, P1 = the current
 *   modulation group's pointer (then the early taps' offsets), P0 a line's
 *   address;
 *   I0 -> l, I1 -> r, I2 = position A (M2 its step), I3 = position B (M3);
 *   R4 = this group's weights (f | 32767 - f), then m | m, then c0 | c1;
 *   R5 = the even lines' halved sums (run kernels) or the crossfade's
 *   weights (b | 32767 - b); R6 = d | d (the input to the lines), then
 *   c2 | c3; R7 = the odd lines' halved sums (stereo), the dither's gain
 *   pair (mono) or a pair (crossfade); R0-R3 temporaries; A0, A1.
 *
 * The stack frame (below the saved registers): [SP] = this sample's dither
 * pair, [SP + 4] = its gain pair (block[HK_DK]), [SP + 8] = the generator's
 * state.
 *
 * Parallel issue: the block's per-sample words that are loaded alongside
 * a MAC (the line offsets, b, z) sit below byte 64, the reach of the
 * 16-bit load form.
 */

#include "hall_layout.h"

#define ROUNDING_ENTER                                                     \
	R0 = ASTAT;                                                        \
	[--SP] = R0;                                                       \
	BITCLR (R0, 8);                                                    \
	ASTAT = R0;

#define ROUNDING_EXIT                                                      \
	R0 = [SP++];                                                       \
	ASTAT = R0;

#define ENTER                                                              \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	P5 = R0;                                                           \
	P4 = R1;                                                           \
	I0 = R2;                                                           \
	ROUNDING_ENTER                                                     \
	SP += -12;                                                         \
	R0 = [P5 + HK_RNG];                                                \
	[SP + 8] = R0;                                                     \
	R0 = [P5 + HK_DK];                                                 \
	[SP + 4] = R0;

#define LOAD_MOD                                                           \
	R0 = [P5 + HK_POSA];                                               \
	I2 = R0;                                                           \
	R0 = [P5 + HK_STEPA];                                              \
	M2 = R0;                                                           \
	R0 = [P5 + HK_POSB];                                               \
	I3 = R0;                                                           \
	R0 = [P5 + HK_STEPB];                                              \
	M3 = R0;

#define EXIT                                                               \
	R0 = I2;                                                           \
	[P5 + HK_POSA] = R0;                                               \
	R0 = I3;                                                           \
	[P5 + HK_POSB] = R0;                                               \
	R0 = [SP + 8];                                                     \
	[P5 + HK_RNG] = R0;                                                \
	SP += 12;                                                          \
	ROUNDING_EXIT                                                      \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

/* A buffer write at word `index` from p, and its mirror. MIRROR (defined
 * per kernel) is the mirror's store or nothing. */
#define WRITE(index, R)                                                    \
	W[P2 + 2 * (index)] = R;                                           \
	MIRROR(index, R)
#define MIRROR_ON(index, R) W[P3 + 2 * (index)] = R;
#define MIRROR_OFF(index, R)

/* Read the allpass's tail; acc += prev rs; write the rounded acc at base;
 * acc = prev + written ws. Uses R1, R2. */
#define ALLPASS(base, length, rs, ws)                                      \
	R1 = W[P2 + 2 * ((base) + (length))] (X);                          \
	R2.L = (A0 += R1.L * rs);                                          \
	WRITE(base, R2)                                                    \
	R1 <<= 16;                                                         \
	A0 = R1;                                                           \
	A0 += R2.L * ws;

/* The dither: xorshift32 of [SP + 8]; each half >> 1 into [SP]. */
#define DITHER                                                             \
	R5 = [SP + 8];                                                     \
	R4 = R5 << 13;                                                     \
	R5 = R5 ^ R4;                                                      \
	R4 = R5 >> 17;                                                     \
	R5 = R5 ^ R4;                                                      \
	R4 = R5 << 5;                                                      \
	R5 = R5 ^ R4;                                                      \
	[SP + 8] = R5;                                                     \
	R5 = R5 >>> 1 (V);                                                 \
	[SP] = R5;

/* The input: x gain (ramped), the early line, three allpasses, x 1/2;
 * R6 = d | d. */
#define INPUT                                                              \
	DITHER                                                             \
	P2 += -2;                                                          \
	P3 += -2;                                                          \
	R0 = W[P4++] (X);                                                  \
	R1 = [P5 + HK_GAIN];                                               \
	R2 = [P5 + HK_GSTEP];                                              \
	A0 = R0.L * R1.H;                                                  \
	R1 = R1 + R2;                                                      \
	[P5 + HK_GAIN] = R1;                                               \
	R0.L = A0;                                                         \
	WRITE(HL_ER, R0)                                                   \
	R0 <<= 16;                                                         \
	A0 = R0;                                                           \
	R3 = [P5 + HK_DG];                                                 \
	ALLPASS(HL_AP1, HL_AP1_LEN, R3.L, R3.H)                            \
	ALLPASS(HL_AP2, HL_AP2_LEN, R3.L, R3.H)                            \
	ALLPASS(HL_AP3, HL_AP3_LEN, R3.L, R3.H)                            \
	A0 = A0 >>> 1;                                                     \
	R0.L = A0;                                                         \
	R6 = PACK (R0.L, R0.L);

/* A modulation group: R4 = f | 32767 - f; P1 = p + 2 ip (the + group);
 * P0 = block[OFFB] (the group's first line's offset). */
#define LFO(Ipos, Mstep, OFFB)                                             \
	R0 = Ipos;                                                         \
	Ipos += Mstep;                                                     \
	R1 = R0.L (Z);                                                     \
	R0 >>>= 16;                                                        \
	R1 >>= 1;                                                          \
	R2 = 32767 (X);                                                    \
	R2 = R2 - R1;                                                      \
	R4 = PACK (R1.L, R2.L) || P0 = [P5 + (OFFB)] || NOP;               \
	P1 = R0;                                                           \
	P1 = P2 + (P1 << 1);

/* The - group: P1 = p - 2 ip - 2; P0 = block[OFFB]. */
#define NEGATE(OFFB)                                                       \
	P0 = P2;                                                           \
	P0 -= P1;                                                          \
	P1 = P0 + P2;                                                      \
	P0 = [P5 + (OFFB)];                                                \
	P1 += -2;

/* Pair i's damping, y in Y: R0 = rnd(z a + y b + dither x DK) (not
 * stored). KLOAD loads the dither's gain pair into DK (or nothing: it's in
 * a register). Uses R0, R1, R3. */
#define DAMP(i, Y, KLOAD, DK)                                              \
	R1 = [P5 + HK_CA + 4 * (i)];                                       \
	A1 = R0.H * R1.H, A0 = R0.L * R1.L || R3 = [P5 + HK_CB + 4 * (i)] || NOP; \
	A1 += Y.H * R3.H, A0 += Y.L * R3.L || R1 = [SP] || NOP;            \
	KLOAD                                                              \
	A1 += R1.H * DK.H, A0 += R1.L * DK.L;                              \
	R0.H = A1, R0.L = A0;

#define K_STACK R3 = [SP + 4];

/* Pair i (P0 = its first line's offset, loaded): R2 = y_j | y_k (reads
 * rnd(s0 W0 + s1 W1)), then R0 = the damped z pair (not stored). Uses
 * R0-R3, P0. */
#define PAIR(i, W0, W1, KLOAD, DK)                                         \
	P0 = P0 + P1;                                                      \
	R0 = W[P0++] (X);                                                  \
	R1 = W[P0] (X);                                                    \
	A0 = R0.L * W0 || P0 = [P5 + HK_OFF + 8 * (i) + 4] || NOP;         \
	P0 = P0 + P1;                                                      \
	R2.L = (A0 += R1.L * W1) || R0 = W[P0++] (X) || NOP;               \
	A1 = R0.L * W0 || R1 = W[P0] (X) || NOP;                           \
	R2.H = (A1 += R1.L * W1) || R0 = [P5 + HK_Z + 4 * (i)] || NOP;     \
	DAMP(i, R2, KLOAD, DK)

/* A line's read at the offset in P0 (loaded): DST = rnd(s0 W0 + s1 W1)
 * through ACC. Uses R0, R1, P0. */
#define XREAD(ACC, DST, W0, W1)                                            \
	P0 = P0 + P1;                                                      \
	R0 = W[P0++] (X);                                                  \
	ACC = R0.L * W0 || R1 = W[P0] (X) || NOP;                          \
	DST = (ACC += R1.L * W1);

/* Pair i crossfaded (P0 = its first line's offset, loaded): each line read
 * at the old size (block[HK_OFF]) and the new (block[HK_XOFF]), R7 =
 * rnd(old (32767 - b) + new b) per line (R5 = b | 32767 - b); then R0 =
 * the damped z pair. */
#define XPAIR(i, W0, W1)                                                   \
	XREAD(A0, R2.L, W0, W1)                                            \
	P0 = [P5 + HK_XOFF + 8 * (i)];                                     \
	XREAD(A1, R2.H, W0, W1)                                            \
	A0 = R2.L * R5.L || P0 = [P5 + HK_OFF + 8 * (i) + 4] || NOP;       \
	R7.L = (A0 += R2.H * R5.H);                                        \
	XREAD(A0, R2.L, W0, W1)                                            \
	P0 = [P5 + HK_XOFF + 8 * (i) + 4];                                 \
	XREAD(A1, R2.H, W0, W1)                                            \
	A1 = R2.L * R5.L;                                                  \
	R7.H = (A1 += R2.H * R5.H) || R0 = [P5 + HK_Z + 4 * (i)] || NOP;   \
	DAMP(i, R7, K_STACK, R3)

/* Householder, m | m = rnd(sum z / 4) into R4; then each pair's writes:
 * sat(sat(z - m) +- d) at its two lines (OP: the first sign is the high
 * half's). */
#define WRITE_PAIR(R, OP, cs, bk)                                          \
	R = R -|- R4 (S);                                                  \
	R = R OP R6 (S);                                                   \
	WRITE(cs, R)                                                       \
	R >>= 16;                                                          \
	WRITE(bk, R)

#define WRITES                                                             \
	R3 = 8192 (X);                                                     \
	R0 = [P5 + HK_Z];                                                  \
	R1 = [P5 + HK_Z + 4];                                              \
	A1 = R0.H * R3.L, A0 = R0.L * R3.L || R2 = [P5 + HK_Z + 8] || NOP; \
	A1 += R1.H * R3.L, A0 += R1.L * R3.L || R0 = [P5 + HK_Z + 12] || NOP; \
	A1 += R2.H * R3.L, A0 += R2.L * R3.L || R1 = [P5 + HK_Z] || NOP;   \
	A1 += R0.H * R3.L, A0 += R0.L * R3.L || R2 = [P5 + HK_Z + 4] || NOP; \
	A0 += A1;                                                          \
	R4.L = A0;                                                         \
	R4 = PACK (R4.L, R4.L) || R3 = [P5 + HK_Z + 8] || NOP;             \
	WRITE_PAIR(R1, +|+, HL_L0, HL_L4)                                  \
	WRITE_PAIR(R2, -|+, HL_L2, HL_L6)                                  \
	WRITE_PAIR(R3, +|-, HL_L1, HL_L5)                                  \
	R0 = [P5 + HK_Z + 12];                                             \
	WRITE_PAIR(R0, -|-, HL_L3, HL_L7)

/* The early taps' coefficients: R4 = c0 | c1, R6 = c2 | c3, R3 = gl | c4;
 * P1 = the block's early offsets. */
#define LOAD_COEFS                                                         \
	R3 = [P5 + HK_GLC4];                                               \
	R4 = [P5 + HK_C01];                                                \
	R6 = [P5 + HK_C23];                                                \
	P0 = HK_EROFF (X);                                                 \
	P1 = P5 + P0;

/* Early taps, pipelined: each MACs the tap loaded before and loads the
 * next (offset o from P1). */
#define TAP_FIRST(o)                                                       \
	P0 = [P1 + (o)];                                                   \
	P0 = P0 + P2;                                                      \
	R0 = W[P0] (X);
#define TAP_NEXT(ACC, C, o)                                                \
	ACC += R0.L * C || P0 = [P1 + (o)] || NOP;                         \
	P0 = P0 + P2;                                                      \
	R0 = W[P0] (X);
#define TAP_LAST(ACC, C)                                                   \
	ACC += R0.L * C;

/* Both sides' taps: left into A0, right into A1 (offsets from P1: 5 left,
 * then 5 right). */
#define TAPS_LR                                                            \
	TAP_FIRST(0)                                                       \
	TAP_NEXT(A0, R4.L, 20)                                             \
	TAP_NEXT(A1, R4.L, 4)                                              \
	TAP_NEXT(A0, R4.H, 24)                                             \
	TAP_NEXT(A1, R4.H, 8)                                              \
	TAP_NEXT(A0, R6.L, 28)                                             \
	TAP_NEXT(A1, R6.L, 12)                                             \
	TAP_NEXT(A0, R6.H, 32)                                             \
	TAP_NEXT(A1, R6.H, 16)                                             \
	TAP_NEXT(A0, R3.H, 36)                                             \
	TAP_LAST(A1, R3.H)

/* The left side's taps into A0. */
#define TAPS_L                                                             \
	TAP_FIRST(0)                                                       \
	TAP_NEXT(A0, R4.L, 4)                                              \
	TAP_NEXT(A0, R4.H, 8)                                              \
	TAP_NEXT(A0, R6.L, 12)                                             \
	TAP_NEXT(A0, R6.H, 16)                                             \
	TAP_LAST(A0, R3.H)

/* l, r from A0 = left, A1 = right: the width matrix, x 2 (S2RND). The
 * last store is the caller's (the loop's end). */
#define WIDTH_OUT                                                          \
	R0.H = A1, R0.L = A0;                                              \
	R1 = [P5 + HK_WIDTH];                                              \
	A1 = R0.L * R1.H, A0 = R0.L * R1.L;                                \
	A1 += R0.H * R1.L, A0 += R0.H * R1.H;                              \
	R0.H = A1, R0.L = A0 (S2RND);                                      \
	W[I0++] = R0.L;

/* The stereo run kernel's sample. */
#define STEREO_SAMPLE                                                      \
	INPUT                                                              \
	LFO(I2, M2, HK_OFF)                                                \
	PAIR(0, R4.L, R4.H, K_STACK, R3)                                   \
	R5 = PACK (R2.H, R2.L) || [P5 + HK_Z] = R0 || NOP;                 \
	NEGATE(HK_OFF + 8)                                                 \
	PAIR(1, R4.H, R4.L, K_STACK, R3)                                   \
	R5 = R5 +|+ R2, R1 = R5 -|- R2 (ASR) || [P5 + HK_Z + 4] = R0 || NOP; \
	LFO(I3, M3, HK_OFF + 16)                                           \
	PAIR(2, R4.L, R4.H, K_STACK, R3)                                   \
	R7 = PACK (R2.H, R2.L) || [P5 + HK_Z + 8] = R0 || NOP;             \
	NEGATE(HK_OFF + 24)                                                \
	PAIR(3, R4.H, R4.L, K_STACK, R3)                                   \
	R7 = R7 +|+ R2, R1 = R7 -|- R2 (ASR) || [P5 + HK_Z + 12] = R0 || NOP; \
	WRITES                                                             \
	LOAD_COEFS                                                         \
	A0 = R5.L * R3.L;                                                  \
	A0 += R5.H * R3.L;                                                 \
	A1 = R7.L * R3.L;                                                  \
	A1 += R7.H * R3.L;                                                 \
	TAPS_LR                                                            \
	WIDTH_OUT

/* The mono run kernel's sample (the dither's gain pair in R7). */
#define MONO_SAMPLE                                                        \
	INPUT                                                              \
	LFO(I2, M2, HK_OFF)                                                \
	PAIR(0, R4.L, R4.H, , R7)                                          \
	R5 = PACK (R2.H, R2.L) || [P5 + HK_Z] = R0 || NOP;                 \
	NEGATE(HK_OFF + 8)                                                 \
	PAIR(1, R4.H, R4.L, , R7)                                          \
	R5 = R5 +|+ R2, R1 = R5 -|- R2 (ASR) || [P5 + HK_Z + 4] = R0 || NOP; \
	LFO(I3, M3, HK_OFF + 16)                                           \
	PAIR(2, R4.L, R4.H, , R7)                                          \
	[P5 + HK_Z + 8] = R0;                                              \
	NEGATE(HK_OFF + 24)                                                \
	PAIR(3, R4.H, R4.L, , R7)                                          \
	[P5 + HK_Z + 12] = R0;                                             \
	WRITES                                                             \
	LOAD_COEFS                                                         \
	A0 = R5.L * R3.L;                                                  \
	A0 += R5.H * R3.L;                                                 \
	TAPS_L                                                             \
	R0.L = A0 (S2RND);

/*
 * void HallRunAsm(HallKernel* k, const int16_t* x, int16_t* l,
 *                 int16_t* r, int16_t* p, int32_t n, int32_t alias_bytes);
 * void HallRunNmAsm(...): the same with alias_bytes = 0 (no mirror).
 */
#define STEREO_KERNEL(NAME, START, END)                                    \
	.align 4;                                                          \
	.global NAME;                                                      \
	.type NAME, STT_FUNC;                                              \
NAME:                                                                      \
	ENTER                                                              \
	R0 = [FP + 20];                                                    \
	I1 = R0;                                                           \
	P2 = [FP + 24];                                                    \
	P1 = [FP + 28];                                                    \
	P0 = [FP + 32];                                                    \
	P3 = P2 + P0;                                                      \
	LOAD_MOD                                                           \
	LSETUP (START, END) LC0 = P1;                                      \
START:                                                                     \
	STEREO_SAMPLE                                                      \
END:                                                                       \
	W[I1++] = R0.H;                                                    \
	EXIT                                                               \
	.size NAME, .-NAME

/*
 * void HallRunMonoAsm(HallKernel* k, const int16_t* x, int16_t* l,
 *                     int16_t* p, int32_t n, int32_t alias_bytes);
 * void HallRunMonoNmAsm(...): the same with alias_bytes = 0.
 */
#define MONO_KERNEL(NAME, START, END)                                      \
	.align 4;                                                          \
	.global NAME;                                                      \
	.type NAME, STT_FUNC;                                              \
NAME:                                                                      \
	ENTER                                                              \
	R7 = [P5 + HK_DK];                                                 \
	P2 = [FP + 20];                                                    \
	P1 = [FP + 24];                                                    \
	P0 = [FP + 28];                                                    \
	P3 = P2 + P0;                                                      \
	LOAD_MOD                                                           \
	LSETUP (START, END) LC0 = P1;                                      \
START:                                                                     \
	MONO_SAMPLE                                                        \
END:                                                                       \
	W[I0++] = R0.L;                                                    \
	EXIT                                                               \
	.size NAME, .-NAME

	.text

#define MIRROR MIRROR_ON
STEREO_KERNEL(_HallRunAsm, .Ls_start, .Ls_end)
MONO_KERNEL(_HallRunMonoAsm, .Lm_start, .Lm_end)
#undef MIRROR
#define MIRROR MIRROR_OFF
STEREO_KERNEL(_HallRunNmAsm, .Lsn_start, .Lsn_end)
MONO_KERNEL(_HallRunMonoNmAsm, .Lmn_start, .Lmn_end)
#undef MIRROR
#define MIRROR MIRROR_ON

/*
 * void HallXfadeRunAsm(HallKernel* k, const int16_t* x, int16_t* l,
 *                      int16_t* r, int16_t* p, int32_t n,
 *                      int32_t alias_bytes);
 *
 * As HallRunAsm, every size-dependent read crossfaded (HallXfadeRunC):
 * R5 = b | 32767 - b this sample, the halved sums go to block[HK_E],
 * block[HK_O].
 */
	.align 4
	.global _HallXfadeRunAsm
	.type _HallXfadeRunAsm, STT_FUNC
_HallXfadeRunAsm:
	ENTER
	R0 = [FP + 20];                 /* r */
	I1 = R0;
	P2 = [FP + 24];                 /* p */
	P1 = [FP + 28];                 /* n */
	P0 = [FP + 32];                 /* alias, bytes */
	P3 = P2 + P0;
	LOAD_MOD

	LSETUP (.Lx_start, .Lx_end) LC0 = P1;
.Lx_start:
	INPUT
	/* The crossfade's weights: b = xf >> 16. */
	R0 = [P5 + HK_XF];
	R1 = [P5 + HK_XFSTEP];
	R1 = R0 + R1;
	[P5 + HK_XF] = R1;
	R0 >>= 16;
	R1 = 32767 (X);
	R1 = R1 - R0;
	R5 = PACK (R0.L, R1.L);
	LFO(I2, M2, HK_OFF)
	XPAIR(0, R4.L, R4.H)
	[P5 + HK_Z] = R0;
	[P5 + HK_E] = R7;
	NEGATE(HK_OFF + 8)
	XPAIR(1, R4.H, R4.L)
	[P5 + HK_Z + 4] = R0;
	R0 = [P5 + HK_E];
	R0 = R0 +|+ R7, R1 = R0 -|- R7 (ASR);
	[P5 + HK_E] = R0;
	LFO(I3, M3, HK_OFF + 16)
	XPAIR(2, R4.L, R4.H)
	[P5 + HK_Z + 8] = R0;
	[P5 + HK_O] = R7;
	NEGATE(HK_OFF + 24)
	XPAIR(3, R4.H, R4.L)
	[P5 + HK_Z + 12] = R0;
	R0 = [P5 + HK_O];
	R0 = R0 +|+ R7, R1 = R0 -|- R7 (ASR);
	[P5 + HK_O] = R0;
	WRITES
	/* The early taps at the old size (rounded into R7), at the new (R0),
	 * blended into R7 = right | left. */
	LOAD_COEFS
	A1 = A0 = 0;
	TAPS_LR
	R7.H = A1, R7.L = A0;
	P0 = HK_XEROFF (X);
	P1 = P5 + P0;
	A1 = A0 = 0;
	TAPS_LR
	R0.H = A1, R0.L = A0;
	A1 = R7.H * R5.L, A0 = R7.L * R5.L;
	A1 += R0.H * R5.H, A0 += R0.L * R5.H;
	R7.H = A1, R7.L = A0;
	/* acc = early << 16 + the lines' sums x gl. */
	R0 = R7 << 16;
	A0 = R0;
	R0 = R7 >> 16;
	R0 <<= 16;
	A1 = R0;
	R0 = [P5 + HK_E];
	A0 += R0.L * R3.L;
	A0 += R0.H * R3.L;
	R0 = [P5 + HK_O];
	A1 += R0.L * R3.L;
	A1 += R0.H * R3.L;
	WIDTH_OUT
.Lx_end:
	W[I1++] = R0.H;

	EXIT
	.size _HallXfadeRunAsm, .-_HallXfadeRunAsm

/*
 * void HallPreRunAsm(const int16_t* l, const int16_t* r, int16_t* ring,
 *                    int16_t* x, int32_t w, int32_t d, int32_t step,
 *                    int32_t n);
 *
 * HallPreRunC (the plate's pre-delay kernel). P4 -> l, P3 -> r,
 * P5 = ring, I0 -> x; R4 = HL_PRE_WORDS, R5 = step, R6 = d, R7 = w.
 */
	.align 4
	.global _HallPreRunAsm
	.type _HallPreRunAsm, STT_FUNC
_HallPreRunAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P4 = R0;
	P3 = R1;
	P5 = R2;
	ROUNDING_ENTER
	R0 = [FP + 20];                 /* x */
	I0 = R0;
	R7 = [FP + 24];                 /* w */
	R6 = [FP + 28];                 /* d */
	R5 = [FP + 32];                 /* step */
	P1 = [FP + 36];                 /* n */
	R4 = HL_PRE_WORDS (X);

	LSETUP (.Lp_start, .Lp_end) LC0 = P1;
.Lp_start:
	R0 = W[P4++] (X);
	R1 = W[P3++] (X);
	R0 = R0 + R1;
	R0 >>>= 1;
	P0 = R7;
	R1 = R6 >>> 16;
	P0 = P5 + (P0 << 1);
	R1 = R7 - R1;
	W[P0] = R0;
	R2 = R1 >>> 31;
	R2 = R2 & R4;
	R1 = R1 + R2;
	R2 = R1;
	R2 += -1;
	R3 = R2 >>> 31;
	R3 = R3 & R4;
	R2 = R2 + R3;
	P0 = R1;
	P1 = R2;
	R1 = R6.L (Z);
	R1 >>= 1;
	R0 = 32767 (X);
	R0 = R0 - R1;
	R1 = PACK (R1.L, R0.L);
	P0 = P5 + (P0 << 1);
	P1 = P5 + (P1 << 1);
	R6 = R6 + R5;
	R0 = W[P0] (X);
	R2 = W[P1] (X);
	A0 = R0.L * R1.L;
	R0.L = (A0 += R2.L * R1.H);
	R7 += 1;
.Lp_end:
	W[I0++] = R0.L;

	ROUNDING_EXIT
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _HallPreRunAsm, .-_HallPreRunAsm
