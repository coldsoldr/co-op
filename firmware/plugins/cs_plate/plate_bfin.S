/*
 * Blackfin kernels for the plate's runs: the same arithmetic as
 * PlateInputRunC and PlateTankRunC (plate.cc; bit-exact, checked by
 * tests/test_plate.cc). The 16-bit MACs' default rounding (unbiased) is
 * what the C references model; RND_MOD is cleared on entry and restored
 * on exit, so the caller's ASTAT doesn't matter.
 *
 * p points at a buffer's write pointer; each sample first moves it down
 * one. Fixed accesses are immediate offsets from it (plate_layout.h); the
 * size-dependent ones come from the kernel block. Every write also goes
 * alias_bytes further on (P3 = p + alias). n > 0. L registers stay 0.
 */

#include "plate_layout.h"

#define OFF(j) (PTK_OFF + 4 * (j))

/* Read the line's tail at prev_off; acc += prev rs; write the rounded acc
 * at `base` (and its mirror); acc = prev + written ws. */
#define ALLPASS(base, length, rs, ws)                                      \
	R1 = W[P2 + 2 * ((base) + (length))] (X);                          \
	R2.L = (A0 += R1.L * rs);                                          \
	W[P2 + 2 * (base)] = R2;                                           \
	W[P3 + 2 * (base)] = R2;                                           \
	R1 <<= 16;                                                         \
	A0 = R1;                                                           \
	A0 += R2.L * ws;

/* Save RND_MOD (ASTAT bit 8) in R0's ... : ASTAT is saved on the stack
 * and RND_MOD cleared (unbiased rounding). */
#define ROUNDING_ENTER                                                     \
	R0 = ASTAT;                                                        \
	[--SP] = R0;                                                       \
	BITCLR (R0, 8);                                                    \
	ASTAT = R0;

#define ROUNDING_EXIT                                                      \
	R0 = [SP++];                                                       \
	ASTAT = R0;

	.text

/*
 * void PlateInputRunAsm(PlateInputKernel* k, const int16_t* x,
 *                       int16_t* out, int16_t* p, int32_t n,
 *                       int32_t alias_bytes);
 *
 * P2 = p, P3 = p + alias, P4 -> x, P5 -> k, I0 -> out, I1 = gain (Q31),
 * M0 = its step; R4 = -g1 | g1, R5 = -g2 | g2, R6 = the low cut's state,
 * R7 = its coefficient; A0 the accumulator; R0-R3 temporaries.
 */
	.align 4
	.global _PlateInputRunAsm
	.type _PlateInputRunAsm, STT_FUNC
_PlateInputRunAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;
	P4 = R1;
	I0 = R2;
	ROUNDING_ENTER
	P2 = [FP + 20];                 /* p */
	P1 = [FP + 24];                 /* n */
	P0 = [FP + 28];                 /* alias, bytes */
	P3 = P2 + P0;
	R4 = [P5 + PIK_G1];
	R5 = [P5 + PIK_G2];
	R6 = [P5 + PIK_HP];
	R7 = [P5 + PIK_KHP];
	R0 = [P5 + PIK_GAIN];
	I1 = R0;
	R0 = [P5 + PIK_GSTEP];
	M0 = R0;

	LSETUP (.Li_start, .Li_end) LC0 = P1;
.Li_start:
	P2 += -2;
	P3 += -2;
	/* Low cut: y = sat(x - (hp >> 16)); hp = sat32(hp + 2 k y). */
	R0 = W[P4++] (X);
	R1 = R6 >>> 16;
	R0.L = R0.L - R1.L (S);
	A0 = R6;
	A0 += R0.L * R7.L;
	R6 = A0;
	/* x gain, then the four input allpasses. */
	R1 = I1;
	I1 += M0;
	A0 = R0.L * R1.H;
	ALLPASS(PL_I1, PL_I1_LEN, R4.L, R4.H)
	ALLPASS(PL_I2, PL_I2_LEN, R4.L, R4.H)
	ALLPASS(PL_I3, PL_I3_LEN, R5.L, R5.H)
	ALLPASS(PL_I4, PL_I4_LEN, R5.L, R5.H)
	R0.L = A0;
.Li_end:
	W[I0++] = R0.L;

	R0 = I1;
	[P5 + PIK_GAIN] = R0;
	[P5 + PIK_HP] = R6;
	ROUNDING_EXIT
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _PlateInputRunAsm, .-_PlateInputRunAsm

/*
 * The tank, per sample (both kernels):
 *
 * P2 = p, P3 = p + alias, P4 -> x, P5 -> k, P0/P1 addresses; I0 -> l,
 * I1 -> r (stereo), I3 = pos1, M3 = step1, I2 = pos2, M2 = step2 (Q16);
 * R4 = -dd2 | dd2, R5 = dd1 | -dd1, R6 = decay | klp, R7 = lp_1 | lp_2;
 * A1 = x << 16 (both halves' input), A0 the accumulator; R0-R3
 * temporaries.
 */

/* R3.L = the modulated tap of the allpass at `base`, its delay (Q16) in
 * R0: two samples, weights 32767 - f and f. Uses A0, R0-R2, P0. */
#define MODTAP_R(base)                                                     \
	R1 = R0.L (Z);                                                     \
	R0 >>>= 16;                    /* ip */                            \
	R1 >>= 1;                      /* f */                             \
	P0 = R0;                                                           \
	R2 = 32767 (X);                                                    \
	P0 = P2 + (P0 << 1);                                               \
	R2 = R2 - R1;                                                      \
	R0 = W[P0 + 2 * (base)] (X);                                       \
	R2 = PACK (R1.L, R2.L);        /* f | 32767 - f */                 \
	R1 = W[P0 + 2 * (base) + 2] (X);                                   \
	A0 = R0.L * R2.L;                                                  \
	R3.L = (A0 += R1.L * R2.H);

/* The same, the delay from Ipos (then Ipos += Mstep). */
#define MODTAP(base, Ipos, Mstep)                                          \
	R0 = Ipos;                                                         \
	Ipos += Mstep;                                                     \
	MODTAP_R(base)

/* One half: acc = in + decay x (the other half's end at tail_in); the
 * modulated allpass (-dd1) at abase; the delay at dbase, its tail at
 * tail_d; damping (lp in R7.half); x decay; the allpass (dd2) at apbase,
 * its tail at tail_ap; the delay at d2base. */
/* (The tails are read before this sample's writes: no tail is a written
 * slot, so the order doesn't matter, and the loads pair with the MACs.) */
#define HALF(tail_in, abase, Ipos, Mstep, dbase, tail_d, LPH, apbase, tail_ap, d2base) \
	MODTAP(abase, Ipos, Mstep)                                         \
	P1 = [P5 + OFF(tail_in)];                                          \
	P0 = [P5 + OFF(tail_d)];                                           \
	P1 = P1 + P2;                                                      \
	P0 = P0 + P2;                                                      \
	A0 = A1 || R1 = W[P1] (X) || NOP;                                  \
	P1 = [P5 + OFF(tail_ap)];                                          \
	A0 += R1.L * R6.L || R2 = W[P0] (X) || NOP;   /* D's tail */       \
	P1 = P1 + P2;                                                      \
	A0 += R3.L * R5.L || R1 = W[P1] (X) || NOP;   /* the AP's tail */  \
	R0.L = A0;                                                         \
	W[P2 + 2 * (abase)] = R0;                                          \
	W[P3 + 2 * (abase)] = R0;                                          \
	R3 <<= 16;                                                         \
	A0 = R3;                                                           \
	R0.L = (A0 += R0.L * R5.H);                                        \
	W[P2 + 2 * (dbase)] = R0;                                          \
	W[P3 + 2 * (dbase)] = R0;                                          \
	R2.L = R2.L - LPH (S);                                             \
	R2.L = R2.L * R6.H;                                                \
	LPH = LPH + R2.L (S);                                              \
	A0 = LPH * R6.L;                                                   \
	R2.L = (A0 += R1.L * R4.L);                                        \
	W[P2 + 2 * (apbase)] = R2;                                         \
	W[P3 + 2 * (apbase)] = R2;                                         \
	R1 <<= 16;                                                         \
	A0 = R1;                                                           \
	R2.L = (A0 += R2.L * R4.H);                                        \
	W[P2 + 2 * (d2base)] = R2;                                         \
	W[P3 + 2 * (d2base)] = R2;

#define TANK                                                               \
	P2 += -2;                                                          \
	P3 += -2;                                                          \
	R0 = W[P4++] (X);                                                  \
	R0 <<= 16;                                                         \
	A1 = R0;                                                           \
	HALF(PT_D4T, PL_A1, I3, M3, PL_D1, PT_D1T, R7.L, PL_A2, PT_A2T, PL_D2) \
	HALF(PT_D2T, PL_A3, I2, M2, PL_D3, PT_D3T, R7.H, PL_A4, PT_A4T, PL_D4)

/* Dattorro's signs: + + - + - - - (R3 = gout | -gout); each load pairs
 * with the previous tap's MAC. */
#define TAPS(acc, base)                                                    \
	P0 = [P5 + OFF(base)];                                             \
	P1 = [P5 + OFF(base + 1)];                                         \
	P0 = P0 + P2;                                                      \
	P1 = P1 + P2;                                                      \
	R0 = W[P0] (X);                                                    \
	P0 = [P5 + OFF(base + 2)];                                         \
	acc = R0.L * R3.L || R1 = W[P1] (X) || NOP;                        \
	P1 = [P5 + OFF(base + 3)];                                         \
	P0 = P0 + P2;                                                      \
	P1 = P1 + P2;                                                      \
	acc += R1.L * R3.L || R0 = W[P0] (X) || NOP;                       \
	acc += R0.L * R3.H || R1 = W[P1] (X) || NOP;                       \
	P0 = [P5 + OFF(base + 4)];                                         \
	P1 = [P5 + OFF(base + 5)];                                         \
	P0 = P0 + P2;                                                      \
	P1 = P1 + P2;                                                      \
	acc += R1.L * R3.L || R0 = W[P0] (X) || NOP;                       \
	acc += R0.L * R3.H || R1 = W[P1] (X) || NOP;                       \
	P0 = [P5 + OFF(base + 6)];                                         \
	P0 = P0 + P2;                                                      \
	acc += R1.L * R3.H || R0 = W[P0] (X) || NOP;                       \
	acc += R0.L * R3.H;

#define TANK_ENTER                                                         \
	LINK 0;                                                            \
	[--SP] = (R7:4, P5:3);                                             \
	[--SP] = LC0;                                                      \
	[--SP] = LT0;                                                      \
	[--SP] = LB0;                                                      \
	P5 = R0;                                                           \
	P4 = R1;                                                           \
	I0 = R2;                                                           \
	ROUNDING_ENTER

#define TANK_LOAD                                                          \
	R4 = [P5 + PTK_DD2];                                               \
	R5 = [P5 + PTK_DD1];                                               \
	R6 = [P5 + PTK_DECAY_KLP];                                         \
	R7 = [P5 + PTK_LP];                                                \
	R0 = [P5 + PTK_POS1];                                              \
	I3 = R0;                                                           \
	R0 = [P5 + PTK_STEP1];                                             \
	M3 = R0;                                                           \
	R0 = [P5 + PTK_POS2];                                              \
	I2 = R0;                                                           \
	R0 = [P5 + PTK_STEP2];                                             \
	M2 = R0;

#define TANK_EXIT                                                          \
	[P5 + PTK_LP] = R7;                                                \
	R0 = I3;                                                           \
	[P5 + PTK_POS1] = R0;                                              \
	R0 = I2;                                                           \
	[P5 + PTK_POS2] = R0;                                              \
	ROUNDING_EXIT                                                      \
	LB0 = [SP++];                                                      \
	LT0 = [SP++];                                                      \
	LC0 = [SP++];                                                      \
	(R7:4, P5:3) = [SP++];                                             \
	UNLINK;                                                            \
	RTS;

/*
 * void PlateTankRunAsm(PlateTankKernel* k, const int16_t* x, int16_t* l,
 *                      int16_t* r, int16_t* p, int32_t n,
 *                      int32_t alias_bytes);
 */
	.align 4
	.global _PlateTankRunAsm
	.type _PlateTankRunAsm, STT_FUNC
_PlateTankRunAsm:
	TANK_ENTER
	R0 = [FP + 20];                 /* r */
	I1 = R0;
	P2 = [FP + 24];                 /* p */
	P1 = [FP + 28];                 /* n */
	P0 = [FP + 32];                 /* alias, bytes */
	P3 = P2 + P0;
	TANK_LOAD

	LSETUP (.Lt_start, .Lt_end) LC0 = P1;
.Lt_start:
	TANK
	R3 = [P5 + PTK_GOUT];
	TAPS(A0, PT_TL)
	TAPS(A1, PT_TR)
	A0 = A0 << 1;
	A1 = A1 << 1;
	R0.L = A0, R0.H = A1;
	W[I0++] = R0.L;
.Lt_end:
	W[I1++] = R0.H;

	TANK_EXIT
	.size _PlateTankRunAsm, .-_PlateTankRunAsm

/*
 * void PlateTankRunMonoAsm(PlateTankKernel* k, const int16_t* x,
 *                          int16_t* l, int16_t* p, int32_t n,
 *                          int32_t alias_bytes);
 */
	.align 4
	.global _PlateTankRunMonoAsm
	.type _PlateTankRunMonoAsm, STT_FUNC
_PlateTankRunMonoAsm:
	TANK_ENTER
	P2 = [FP + 20];                 /* p */
	P1 = [FP + 24];                 /* n */
	P0 = [FP + 28];                 /* alias, bytes */
	P3 = P2 + P0;
	TANK_LOAD

	LSETUP (.Lm_start, .Lm_end) LC0 = P1;
.Lm_start:
	TANK
	R3 = [P5 + PTK_GOUT];
	TAPS(A0, PT_TL)
	A0 = A0 << 1;
	R0.L = A0;
.Lm_end:
	W[I0++] = R0.L;

	TANK_EXIT
	.size _PlateTankRunMonoAsm, .-_PlateTankRunMonoAsm

/*
 * void PlateXfadeRunAsm(PlateTankKernel* k, PlateXfadeKernel* xk,
 *                       const int16_t* x, int16_t* p, int32_t n,
 *                       int32_t alias_bytes);
 *
 * The tank with every size-dependent read crossfaded (PlateXfadeRunC): as
 * the tank kernels, but P4 -> xk, I1 -> x, I0 = xf (Q31), M0 = its step;
 * [xk + PXK_W] = 32767 - b | b, this sample's weights; no taps.
 */

/* R1.L = rnd(old (32767 - b) + new b): tail j at k's size and xk's. Uses
 * A0, R0-R2, P0, P1. */
#define XTAIL(j)                                                           \
	P0 = [P5 + OFF(j)];                                                \
	P1 = [P4 + (PXK_OFF + 4 * (j))];                                   \
	P0 = P0 + P2;                                                      \
	P1 = P1 + P2;                                                      \
	R0 = W[P0] (X);                                                    \
	R1 = W[P1] (X);                                                    \
	R2 = [P4 + PXK_W];                                                 \
	A0 = R0.L * R2.L;                                                  \
	R1.L = (A0 += R1.L * R2.H);

/* R3.L = the crossfaded modulated tap: the old delay from Ipos (+= Mstep),
 * the new one from xk (+= its step). */
#define XMODTAP(base, Ipos, Mstep, xpos, xstep)                            \
	MODTAP(base, Ipos, Mstep)                                          \
	W[P4 + PXK_TMP] = R3;                                              \
	R0 = [P4 + xpos];                                                  \
	R1 = [P4 + xstep];                                                 \
	R1 = R0 + R1;                                                      \
	[P4 + xpos] = R1;                                                  \
	MODTAP_R(base)                                                     \
	R0 = W[P4 + PXK_TMP] (X);                                          \
	R2 = [P4 + PXK_W];                                                 \
	A0 = R0.L * R2.L;                                                  \
	R3.L = (A0 += R3.L * R2.H);

#define XHALF(tail_in, abase, Ipos, Mstep, xpos, xstep, dbase, tail_d, LPH, apbase, tail_ap, d2base) \
	XMODTAP(abase, Ipos, Mstep, xpos, xstep)                           \
	XTAIL(tail_in)                                                     \
	A0 = A1;                                                           \
	A0 += R1.L * R6.L;                                                 \
	A0 += R3.L * R5.L;                                                 \
	R0.L = A0;                                                         \
	W[P2 + 2 * (abase)] = R0;                                          \
	W[P3 + 2 * (abase)] = R0;                                          \
	R3 <<= 16;                                                         \
	A0 = R3;                                                           \
	R0.L = (A0 += R0.L * R5.H);                                        \
	W[P2 + 2 * (dbase)] = R0;                                          \
	W[P3 + 2 * (dbase)] = R0;                                          \
	XTAIL(tail_d)                                                      \
	R1.L = R1.L - LPH (S);                                             \
	R1.L = R1.L * R6.H;                                                \
	LPH = LPH + R1.L (S);                                              \
	XTAIL(tail_ap)                                                     \
	A0 = LPH * R6.L;                                                   \
	R2.L = (A0 += R1.L * R4.L);                                        \
	W[P2 + 2 * (apbase)] = R2;                                         \
	W[P3 + 2 * (apbase)] = R2;                                         \
	R1 <<= 16;                                                         \
	A0 = R1;                                                           \
	R2.L = (A0 += R2.L * R4.H);                                        \
	W[P2 + 2 * (d2base)] = R2;                                         \
	W[P3 + 2 * (d2base)] = R2;

	.align 4
	.global _PlateXfadeRunAsm
	.type _PlateXfadeRunAsm, STT_FUNC
_PlateXfadeRunAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P5 = R0;
	P4 = R1;
	I1 = R2;
	ROUNDING_ENTER
	P2 = [FP + 20];                 /* p */
	P1 = [FP + 24];                 /* n */
	P0 = [FP + 28];                 /* alias, bytes */
	P3 = P2 + P0;
	TANK_LOAD
	R0 = [P4 + PXK_XF];
	I0 = R0;
	R0 = [P4 + PXK_XSTEP];
	M0 = R0;

	LSETUP (.Lx_start, .Lx_end) LC0 = P1;
.Lx_start:
	P2 += -2;
	P3 += -2;
	/* This sample's weights: b = xf >> 16, 32767 - b. */
	R0 = I0;
	I0 += M0;
	R0 >>= 16;
	R1 = 32767 (X);
	R1 = R1 - R0;
	R1 = PACK (R0.L, R1.L);
	[P4 + PXK_W] = R1;
	R0.L = W[I1++];
	R0 <<= 16;
	A1 = R0;
	XHALF(PT_D4T, PL_A1, I3, M3, PXK_POS1, PXK_STEP1, PL_D1, PT_D1T, R7.L, PL_A2, PT_A2T, PL_D2)
	XHALF(PT_D2T, PL_A3, I2, M2, PXK_POS2, PXK_STEP2, PL_D3, PT_D3T, R7.H, PL_A4, PT_A4T, PL_D4)
.Lx_end:
	NOP;

	R0 = I0;
	[P4 + PXK_XF] = R0;
	TANK_EXIT
	.size _PlateXfadeRunAsm, .-_PlateXfadeRunAsm

/*
 * void PlatePreRunAsm(const int16_t* l, const int16_t* r, int16_t* ring,
 *                     int16_t* x, int32_t w, int32_t d, int32_t step,
 *                     int32_t n);
 *
 * PlatePreRunC. P4 -> l, P3 -> r, P5 = ring, I0 -> x; R4 = PL_PRE_WORDS,
 * R5 = step, R6 = d, R7 = w; R0-R3, P0, P1 temporaries.
 */
	.align 4
	.global _PlatePreRunAsm
	.type _PlatePreRunAsm, STT_FUNC
_PlatePreRunAsm:
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
	R4 = PL_PRE_WORDS (X);

	LSETUP (.Lp_start, .Lp_end) LC0 = P1;
.Lp_start:
	R0 = W[P4++] (X);
	R1 = W[P3++] (X);
	R0 = R0 + R1;
	R0 >>>= 1;
	P0 = R7;
	R1 = R6 >>> 16;                 /* ip */
	P0 = P5 + (P0 << 1);
	R1 = R7 - R1;                   /* r0 = w - ip, wrapped */
	W[P0] = R0;
	R2 = R1 >>> 31;
	R2 = R2 & R4;
	R1 = R1 + R2;
	R2 = R1;                        /* r1 = r0 - 1, wrapped */
	R2 += -1;
	R3 = R2 >>> 31;
	R3 = R3 & R4;
	R2 = R2 + R3;
	P0 = R1;
	P1 = R2;
	R1 = R6.L (Z);
	R1 >>= 1;                       /* f */
	R0 = 32767 (X);
	R0 = R0 - R1;
	R1 = PACK (R1.L, R0.L);         /* f | 32767 - f */
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
	.size _PlatePreRunAsm, .-_PlatePreRunAsm
