/*
 * petri's voice kernel (src/petri_engine.cc VoiceLoopC is its C reference, bit for bit; tests/host/petri checks the
 * two against each other in the Blackfin simulator).
 *
 * void PetriVoiceAsm(const uint32_t* rec, uint32_t pos, int32_t rate, int32_t* wl, int32_t* wr,
 *                    uint32_t gains, uint32_t dgains);
 *
 * 32 samples of one voice added into wl[], wr[]. rec: the record, L in the low half of each word, R in the high half;
 * pos: Q17 frames (the index in the top 15 bits); rate: Q17 frames a sample. gains: the voice's gains, R in the high
 * half, L in the low (Q15); dgains: what they move by each sample (the same halves). Per sample, both channels in the
 * two halves of one register:
 *   d = (b >>> 1) - (a >>> 1)            the step to the next frame, halved to fit 16 bits
 *   s = a + 2 floor(d frac / 2^15)       frac = pos bits 2..16 (Q15); 16-bit wrap (the result lies between a and b)
 *   w += floor(s g / 2^15)               integer products in the accumulators
 *   pos += rate; g += dg
 *
 * Single-issue instructions only (no parallel packets: nothing for the device to refuse, docs/plugin-performance.md).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */

	.text
	.align 4
	.global _PetriVoiceAsm
	.type _PetriVoiceAsm, STT_FUNC
_PetriVoiceAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P0 = R0;                        /* rec */
	P1 = [FP + 20];                 /* wl */
	P2 = [FP + 24];                 /* wr */
	R5 = [FP + 28];                 /* gains */
	R3 = [FP + 32];                 /* dgains */
	P5 = 32;
	LSETUP (.Lv_start, .Lv_end) LC0 = P5;
.Lv_start:
	R0 = R1 >> 17;                  /* the frame */
	P3 = R0;
	P3 = P0 + (P3 << 2);
	R4 = R1 >> 2;
	BITCLR (R4, 15);                /* R4.L: the fraction, Q15 */
	R6 = [P3];                      /* a */
	R7 = [P3 + 4];                  /* b */
	R0 = R6 >>> 1 (V);
	R7 = R7 >>> 1 (V);
	R7 = R7 -|- R0;
	R7.H = R7.H * R4.L, R7.L = R7.L * R4.L (T);    /* truncated: no dependence on ASTAT RND_MOD */
	R7 = R7 +|+ R7;
	R6 = R6 +|+ R7;                 /* the interpolated frame: R high, L low */
	A1 = R6.H * R5.H, A0 = R6.L * R5.L (IS);
	A1 = A1 >>> 15;
	A0 = A0 >>> 15;
	R7 = A1, R6 = A0;
	R0 = [P1];
	R0 = R0 + R6;
	[P1++] = R0;
	R0 = [P2];
	R0 = R0 + R7;
	[P2++] = R0;
	R1 = R1 + R2;
.Lv_end:
	R5 = R5 +|+ R3;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _PetriVoiceAsm, .-_PetriVoiceAsm
