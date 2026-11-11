/*
 * wavefolder: the bus conversions (coop's int32 planar bus, the int16 bus widened by 16 bits <-> the int16 blocks
 * FOLD runs on), two samples a pass. The same results as the C loops in wavefolder.cc (the host build's, which the
 * host test runs against these in the simulator). n > 0 and even; the int16 blocks 4-byte aligned. L registers
 * stay 0.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */

	.text

/*
 * int32_t WfBusIn(const int32_t* l, const int32_t* r, int16_t* xl, int16_t* xr, int32_t n);
 *
 * xl[i] = l[i] >> 16, xr[i] = r[i] >> 16; returns 0 when xl == xr (the OR of the packed pairs' differences).
 * I0 -> l, I1 -> r, I2 -> xl, I3 -> xr; R7 the OR.
 */
	.align 4
	.global _WfBusIn
	.type _WfBusIn, STT_FUNC
_WfBusIn:
	LINK 0;
	[--SP] = (R7:4);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	I0 = R0;
	I1 = R1;
	I2 = R2;
	R0 = [FP + 20];                 /* xr */
	I3 = R0;
	P0 = [FP + 24];                 /* n */
	P0 = P0 >> 1;                   /* pairs */
	R7 = 0;

	LSETUP (.Li_start, .Li_end) LC0 = P0;
.Li_start:
	MNOP || R0 = [I0++] || R2 = [I1++];
	MNOP || R1 = [I0++] || R3 = [I1++];
	R4 = PACK (R1.H, R0.H);         /* l pair */
	R5 = PACK (R3.H, R2.H) || [I2++] = R4;   /* r pair */
	R6 = R4 -|- R5 || [I3++] = R5;  /* 0 when equal */
.Li_end:
	R7 = R7 | R6;

	R0 = R7;
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4) = [SP++];
	UNLINK;
	RTS;
	.size _WfBusIn, .-_WfBusIn

/*
 * void WfBusOut(const int16_t* xl, const int16_t* xr, int32_t* l, int32_t* r, int32_t n);
 *
 * l[i] = xl[i] << 16, r[i] = xr[i] << 16 (xr may be xl). I0 -> xl, I1 -> xr, I2 -> l, I3 -> r; R7 = 0.
 */
	.align 4
	.global _WfBusOut
	.type _WfBusOut, STT_FUNC
_WfBusOut:
	LINK 0;
	[--SP] = (R7:4);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	I0 = R0;
	I1 = R1;
	I2 = R2;
	R0 = [FP + 20];                 /* r */
	I3 = R0;
	P0 = [FP + 24];                 /* n */
	P0 = P0 >> 1;
	R7 = 0;

	LSETUP (.Lo_start, .Lo_end) LC0 = P0;
.Lo_start:
	MNOP || R0 = [I0++] || R1 = [I1++];
	R2 = PACK (R0.L, R7.L);         /* xl[2i] << 16 */
	R3 = PACK (R0.H, R7.L) || [I2++] = R2;
	R4 = PACK (R1.L, R7.L) || [I2++] = R3;
	R5 = PACK (R1.H, R7.L) || [I3++] = R4;
.Lo_end:
	[I3++] = R5;

	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4) = [SP++];
	UNLINK;
	RTS;
	.size _WfBusOut, .-_WfBusOut
