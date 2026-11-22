/*
 * fickle's reverb diffusers on the Blackfin (fickle.cc ApPass; 2026-11-22, efficiency audit finding 15): one allpass
 * over the block, both sides in one pass, the ring wrapped by the hardware (no run splits).
 *
 * void FickleApAsm(uint32_t* ring, int32_t ring_bytes, uint32_t ri, uint32_t wi, int32_t* vl, int32_t* vr,
 *                  int32_t g, int32_t ticks);
 *
 * For k = 0 .. ticks - 1, z = ring[ri + k] (L in the low half, R in the high half; indices wrap at ring_bytes / 4):
 *   u = Sat16(v[k] - ((z x g) >> 15))  for each side    -> ring[wi + k] = uL | uR << 16
 *   v[k] = z + ((u x g) >> 15)                          (v int32: a diffuser's output may pass 16 bits)
 * The reads never meet the pass's writes (ri = wi - delay, delay 113 .. 311 > ticks).
 *
 * The arithmetic, bit for bit: (z x g) >> 15 with z, g int16 is the truncating fractional product (T) (2 z g, floor of
 * the high half; g is 0 .. 23592, never -1 x -1). v - p saturated to 16 bits is the integer extraction (IS) of the
 * accumulator holding v - p x 1; z + q is a 32-bit (IS) extraction of z x 1 + q x 1. R7 = g | 1 << 16.
 *
 * Registers: I0 the ring's read head, I1 its write head (both circular over the ring), P0 / P1 -> vL / vR (the next
 * k's loads), I2 / I3 -> vL / vR circular over the block (the stores, one pass late: the first pass rewrites v[ticks-1]
 * with its own value, the last store goes there after the loop); R0 = z, R2 / R3 = vL / vR, R1 the products,
 * R6 = u, R4 / R5 the new v. 7 packets a sample, both sides.
 *
 * Checked against ApPass in C by tests/host/fickle (make ap: random and edge blocks in the simulator; make sim-eq: the
 * plugin's own digest with the C and with this kernel).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
	.section .text._FickleApAsm, "ax", @progbits
	.align 4
	.global _FickleApAsm
	.type _FickleApAsm, STT_FUNC
_FickleApAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	P2 = [FP + 36];                 /* ticks */
	CC = P2 <= 0;
	IF CC JUMP .Lap_done;
	B0 = R0;                        /* the ring, its read head */
	B1 = R0;                        /* and its write head */
	L0 = R1;
	L1 = R1;
	R2 <<= 2;
	R2 = R0 + R2;
	I0 = R2;
	R2 = [FP + 20];                 /* wi */
	R2 <<= 2;
	R2 = R0 + R2;
	I1 = R2;
	R1 = P2;
	R1 <<= 2;                       /* the block's bytes */
	R6 = R1;
	R6 += -4;
	R0 = [FP + 24];                 /* vL */
	P0 = R0;
	B2 = R0;
	L2 = R1;
	R0 = R0 + R6;
	I2 = R0;                        /* vL[ticks - 1] */
	R0 = [FP + 28];                 /* vR */
	P1 = R0;
	B3 = R0;
	L3 = R1;
	R0 = R0 + R6;
	I3 = R0;                        /* vR[ticks - 1] */
	R7 = [FP + 32];                 /* g */
	R7.H = 1;
	R4 = [I2];                      /* the first pass's stores write these back */
	R5 = [I3];
	R0 = [I0++];
	R2 = [P0++];
	R3 = [P1++];
	LSETUP (.Lap_start, .Lap_end) LC0 = P2;
.Lap_start:
	R1.H = R0.H * R7.L, R1.L = R0.L * R7.L (T) || [I2++] = R4 || NOP;     /* p = (z g) >> 15; the last v out */
	A0 = R2 || [I3++] = R5 || NOP;
	A1 = R3 || R2 = [P0++] || NOP;
	R6.H = (A1 -= R1.H * R7.H), R6.L = (A0 -= R1.L * R7.H) (IS) || R3 = [P1++] || NOP;   /* u = Sat16(v - p) */
	R1.H = R6.H * R7.L, R1.L = R6.L * R7.L (T) || [I1++] = R6 || NOP;     /* q = (u g) >> 15; u into the ring */
	A1 = R0.H * R7.H, A0 = R0.L * R7.H (IS) || R0 = [I0++] || NOP;        /* z; the next z */
.Lap_end:
	R5 = (A1 += R1.H * R7.H), R4 = (A0 += R1.L * R7.H) (IS);               /* v = z + q */
	[I2] = R4;
	[I3] = R5;
	L0 = 0;
	L1 = 0;
	L2 = 0;
	L3 = 0;
.Lap_done:
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _FickleApAsm, .-_FickleApAsm
