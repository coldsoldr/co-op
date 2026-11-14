/*
 * petri's space kernel (src/petri_engine.cc PetriSpaceC is its C reference, bit for bit with the Blackfin's default
 * rounding; tests/host/petri checks the two against each other in the Blackfin simulator).
 *
 * void PetriSpaceAsm(int16_t* z, const int16_t* x, uint32_t* y, uint32_t* lp, uint32_t decay);
 *
 * 16 samples at 11.025 kHz. z: the buffer at the moving pointer (src/space_layout.h: every line at a constant byte
 * offset; z steps down one entry a sample, never wrapping); x[16]: the input (int16); y[16]: the output, the left
 * tank half in the low half of each word, the right in the high half; *lp: the two damping states (the same halves),
 * read and written back; decay: the tank's gain in both halves (Q15).
 *
 * Every product is a fractional 16 x 16 MAC; a result leaves the accumulator rounded and saturated to 16 bits (the
 * MAC's extraction). "x 1" is x 32767 (1 - 2^-15). Per sample:
 *   four allpasses:  d = tail; w = x + g d; head = w; x = d - g w            (g 0.625)
 *   the tank, both halves at once (left in .L, right in .H):
 *     t  = x + decay (dl2 tail : dl1 tail)
 *     lp = lp + damp (t - lp)                                                (t - lp saturated)
 *     d  = (ap6 tail : ap5 tail); w = lp - T d; heads = w; y = d + T w       (T 0.5)
 *     dl1 head = y.L, dl2 head = y.H; out = y
 *
 * Single-issue instructions only (no parallel packets: nothing for the device to refuse, docs/plugin-performance.md).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include "space_layout.h"

	.text
	.align 4
	.global _PetriSpaceAsm
	.type _PetriSpaceAsm, STT_FUNC
_PetriSpaceAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P0 = R0;                        /* z */
	P1 = R1;                        /* x */
	P2 = R2;                        /* y */
	P3 = [FP + 20];                 /* lp */
	R6 = [FP + 24];                 /* decay : decay */
	R3 = [P3];                      /* lp right : lp left */
	R7.L = SP_G;
	R7.H = SP_ONE;
	R5.L = SP_DAMP;
	R5.H = SP_DAMP;
	R4.L = SP_T;
	R4.H = SP_T;
	P5 = 16;
	LSETUP (.Ls_start, .Ls_end) LC0 = P5;
.Ls_start:
	R0 = W[P1++] (X);               /* x */
	/* allpass 1 */
	R1 = W[P0 + SP_AP1_TAIL] (X);
	A0 = R0.L * R7.H;
	R2.L = (A0 += R1.L * R7.L);
	W[P0 + SP_AP1_HEAD] = R2;
	A0 = R1.L * R7.H;
	R0.L = (A0 -= R2.L * R7.L);
	/* allpass 2 */
	R1 = W[P0 + SP_AP2_TAIL] (X);
	A0 = R0.L * R7.H;
	R2.L = (A0 += R1.L * R7.L);
	W[P0 + SP_AP2_HEAD] = R2;
	A0 = R1.L * R7.H;
	R0.L = (A0 -= R2.L * R7.L);
	/* allpass 3 */
	R1 = W[P0 + SP_AP3_TAIL] (X);
	A0 = R0.L * R7.H;
	R2.L = (A0 += R1.L * R7.L);
	W[P0 + SP_AP3_HEAD] = R2;
	A0 = R1.L * R7.H;
	R0.L = (A0 -= R2.L * R7.L);
	/* allpass 4 */
	R1 = W[P0 + SP_AP4_TAIL] (X);
	A0 = R0.L * R7.H;
	R2.L = (A0 += R1.L * R7.L);
	W[P0 + SP_AP4_HEAD] = R2;
	A0 = R1.L * R7.H;
	R0.L = (A0 -= R2.L * R7.L);
	/* the tank's inputs: x + decay x the other half's end */
	R1 = W[P0 + SP_DL2_TAIL] (X);
	R2 = W[P0 + SP_DL1_TAIL] (X);
	R1 = PACK (R2.L, R1.L);
	R0 = PACK (R0.L, R0.L);
	A1 = R0.H * R7.H, A0 = R0.L * R7.H;
	R2.H = (A1 += R1.H * R6.H), R2.L = (A0 += R1.L * R6.L);
	/* the damping */
	R2 = R2 -|- R3 (S);
	A1 = R3.H * R7.H, A0 = R3.L * R7.H;
	R3.H = (A1 += R2.H * R5.H), R3.L = (A0 += R2.L * R5.L);
	/* the tank's allpasses */
	R1 = W[P0 + SP_AP5_TAIL] (X);
	R2 = W[P0 + SP_AP6_TAIL] (X);
	R1 = PACK (R2.L, R1.L);
	A1 = R3.H * R7.H, A0 = R3.L * R7.H;
	R2.H = (A1 -= R1.H * R4.H), R2.L = (A0 -= R1.L * R4.L);
	W[P0 + SP_AP5_HEAD] = R2;
	R0 = R2 >> 16;
	W[P0 + SP_AP6_HEAD] = R0;
	A1 = R1.H * R7.H, A0 = R1.L * R7.H;
	R0.H = (A1 += R2.H * R4.H), R0.L = (A0 += R2.L * R4.L);
	/* into the delays, out */
	W[P0 + SP_DL1_HEAD] = R0;
	R1 = R0 >> 16;
	W[P0 + SP_DL2_HEAD] = R1;
	[P2++] = R0;
.Ls_end:
	P0 += -2;
	[P3] = R3;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _PetriSpaceAsm, .-_PetriSpaceAsm
