#include "src/rate_config.h"
/*
 * Blackfin kernel for ReverbFixed's sample loop, settled case (no reads
 * cut off by a Clear): same arithmetic as ReverbFixed::ProcessInternal
 * (bit-exact; tests/bench_string.cc compares checksums with the host).
 *
 * void ReverbKernelAsm(ReverbKernel* k, int32_t* in_out, int16_t* p,
 *                      int32_t size, int32_t alias_bytes);
 *
 * p points at buffer[write_ptr]; each sample first moves it down one. Every
 * access is a fixed offset from it; each write also goes alias_bytes
 * further on (0: the same place again; kSize * 2: the copy read after the
 * write pointer wraps). The LFOs don't step within a call (the caller
 * splits the block where they do), so the modulated taps' offsets and
 * fractions are constant.
 *
 * Registers: P2 = p, P3 = p + alias, P4 = alias, P5 -> k, I0 -> in_out;
 * R0 accumulator, R1 previous read, R4 = diffusion, R5 = -diffusion, R7
 * allpass output.
 */

#define K_GAIN 0
#define K_KRT 4
#define K_KLP 8
#define K_AMOUNT 12
#define K_LP1 16
#define K_LP2 20
#define K_D2OFF 24
#define K_D2FRAC 28
#define K_D1OFF 32
#define K_D1FRAC 36
#define K_WET1 40
#define K_KAP 44

/* Upstream's delay lines (reverb_fixed.cc): bases and lengths, packed with
 * one sample between. DIV halves them at 22.05 kHz (src/rate_config.h); at
 * 44.1 kHz the bases are 0, 151, 366, 686, 1214, 3397, 6088, 10590, 13116,
 * 15314 as before. */
#define DIV OP1RINGS_REVERB_DIV
#define AP1 0
#define AP1_LEN (150 / DIV)
#define AP2 (AP1 + AP1_LEN + 1)
#define AP2_LEN (214 / DIV)
#define AP3 (AP2 + AP2_LEN + 1)
#define AP3_LEN (319 / DIV)
#define AP4 (AP3 + AP3_LEN + 1)
#define AP4_LEN (527 / DIV)
#define DAP1A (AP4 + AP4_LEN + 1)
#define DAP1A_LEN (2182 / DIV)
#define DAP1B (DAP1A + DAP1A_LEN + 1)
#define DAP1B_LEN (2690 / DIV)
#define DEL1 (DAP1B + DAP1B_LEN + 1)
#define DEL1_LEN (4460 / DIV + 40 / DIV + 1)
#define DAP2A (DEL1 + DEL1_LEN + 1)
#define DAP2A_LEN (2525 / DIV)
#define DAP2B (DAP2A + DAP2A_LEN + 1)
#define DAP2B_LEN (2197 / DIV)
#define DEL2 (DAP2B + DAP2B_LEN + 1)

#define MUL(d, a, b)                                                       \
	A0 = a.H * b.H, A1 = a.H * b.L (M);                                \
	A1 += b.H * a.L (M);                                               \
	A1 = A1 >>> 15;                                                    \
	d = (A0 += A1);

/* Read(line TAIL, rs); WriteAllPass(line, ws). */
#define ALLPASS(base, length, rs, ws)                                      \
	R1 = W[P2 + 2 * ((base) + (length) - 1)] (X);                      \
	R1 <<= 12;                                                         \
	MUL(R2, R1, rs)                                                    \
	R0 = R0 + R2 (NS);                                                 \
	R2 = R0 << 4 (S);                                                  \
	R2 >>>= 16;                                                        \
	W[P2 + 2 * (base)] = R2;                                           \
	W[P3 + 2 * (base)] = R2;                                           \
	MUL(R0, R0, ws)                                                    \
	R0 = R0 + R1 (NS);

/* Write(line, 2.0): the store; the caller doubles R0. */
#define WRITE(base)                                                        \
	R2 = R0 << 4 (S);                                                  \
	R2 >>>= 16;                                                        \
	W[P2 + 2 * (base)] = R2;                                           \
	W[P3 + 2 * (base)] = R2;

/* R0 = R7 + krt * (interpolated read at [off] + p, fraction [frac]). */
#define TAP(off, frac)                                                     \
	P1 = [P5 + off];                                                   \
	R3 = [P5 + frac];                                                  \
	P1 = P1 + P2;                                                      \
	R1 = W[P1] (X);                                                    \
	R2 = W[P1 + 2] (X);                                                \
	R1 <<= 12;                                                         \
	R2 <<= 12;                                                         \
	R2 = R2 - R1 (NS);                                                 \
	MUL(R2, R2, R3)                                                    \
	R1 = R1 + R2 (NS);                                                 \
	R3 = [P5 + K_KRT];                                                 \
	MUL(R1, R1, R3)                                                    \
	R0 = R7 + R1 (NS);

/* state += klp * (R0 - state); R0 = state. */
#define LP(state)                                                          \
	R1 = [P5 + state];                                                 \
	R3 = [P5 + K_KLP];                                                 \
	R0 = R0 - R1 (NS);                                                 \
	MUL(R0, R3, R0)                                                    \
	R0 = R1 + R0 (NS);                                                 \
	[P5 + state] = R0;

	.text
	.align 4
	.global _ReverbKernelAsm
	.type _ReverbKernelAsm, STT_FUNC
_ReverbKernelAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;

	P5 = R0;
	I0 = R1;
	P2 = R2;
	R3 = [FP + 20];
	P1 = R3;                        /* size */
	P4 = [FP + 24];                 /* alias, bytes */
	R4 = [P5 + K_KAP];
	R5 = -R4;

	LSETUP (.Lstart, .Lend) LC0 = P1;
.Lstart:
	P2 += -2;
	P3 = P2 + P4;
	R6 = [I0];                      /* dry */
	R3 = [P5 + K_GAIN];
	MUL(R0, R6, R3)

	/* Diffuse through 4 allpasses. */
	ALLPASS(AP1, AP1_LEN, R4, R5)
	ALLPASS(AP2, AP2_LEN, R4, R5)
	ALLPASS(AP3, AP3_LEN, R4, R5)
	ALLPASS(AP4, AP4_LEN, R4, R5)
	R7 = R0;                        /* apout */

	/* First branch: del2's modulated tap, lp_1, dap1a, dap1b, del1. */
	TAP(K_D2OFF, K_D2FRAC)
	LP(K_LP1)
	ALLPASS(DAP1A, DAP1A_LEN, R5, R4)
	ALLPASS(DAP1B, DAP1B_LEN, R4, R5)
	WRITE(DEL1)
	R0 <<= 1;
	[P5 + K_WET1] = R0;

	/* Second branch: del1's modulated tap, lp_2, dap2a, dap2b, del2. */
	TAP(K_D1OFF, K_D1FRAC)
	LP(K_LP2)
	ALLPASS(DAP2A, DAP2A_LEN, R4, R5)
	ALLPASS(DAP2B, DAP2B_LEN, R5, R4)
	WRITE(DEL2)
	R0 <<= 1;

	/* dry + amount (wet_1 + wet_2 - dry). */
	R1 = [P5 + K_WET1];
	R0 = R0 + R1 (NS);
	R0 = R0 - R6 (NS);
	R3 = [P5 + K_AMOUNT];
	MUL(R0, R3, R0)
	R0 = R0 + R6 (NS);
.Lend:
	[I0++] = R0;

	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ReverbKernelAsm, .-_ReverbKernelAsm
