/*
 * Blackfin kernel for the modal resonator's mode bank. Same arithmetic as
 * ModeBankProcessC() in mode_bank.h (bit-exact; checked by
 * tests/bench_mode_bank.cc against the host build).
 *
 * void ModeBankProcessAsm(Mode* modes, int32_t num_modes, const int32_t* in,
 *                         int32_t* previous_input, int32_t* out,
 *                         int32_t* aux, int32_t size, int32_t* half_x);
 *
 * half_x: size words of scratch (the halved inputs, read once per mode and
 * sample; the OP-1 engine passes L1 memory, src/scratch.h).
 *
 * Loop order: for each mode, all samples of the block, so the mode's six
 * values stay in registers; the outputs are accumulated in out[] / aux[].
 * size must be <= MAX_BLOCK; num_modes must be even.
 *
 * Mode layout (24 bytes): lp, bp, half_f, half_q, weight, weight_increment.
 *
 * The device (Test 30-33): the modes live in SDRAM behind a 16 KB data
 * cache, and a cache line costs 100-190 cycles; the per-sample loop itself
 * has no pipeline latencies (ADI EE-197: the MAC/ALU/shifter chain is
 * single-cycle). So each mode pass prefetches the mode two ahead (its
 * 24 bytes start in the line at P5 + 48; its end line is the next mode's
 * start line, prefetched a pass later), and the loop top is 64-bit
 * aligned (a 64-bit first instruction off that alignment costs a cycle an
 * iteration). The probe (op1/cpu_probe.cc) benches this kernel warm and
 * cold against the previous one (op1/mode_bank_old_bfin.S).
 *
 * Register use in the sample loop:
 *   R0 bp    R1 lp    R2 half_f    R3 half_q    R4 weight    R5 increment
 *   R6, R7 temporaries; I0 -> half_x[], I1 -> out[] or aux[]
 *   P2 -> the mode two ahead (prefetch), I2 = out, I3 = aux
 */

#define MAX_BLOCK 32

#define M_LP 0
#define M_BP 4
#define M_HALF_F 8
#define M_HALF_Q 12
#define M_WEIGHT 16
#define M_WEIGHT_INCREMENT 20
#define M_SIZE 24

/* One mode, `size` samples, accumulating into the buffer at I1. Expects
 * P0 = half_x base, P1 = size, P5 -> mode, P2 -> the mode two ahead;
 * advances P5 and P2. */
#define PROCESS_MODE(label)                                                \
	PREFETCH [P2];                                                     \
	P2 += M_SIZE;                                                      \
	R1 = [P5 + M_LP];                                                  \
	R0 = [P5 + M_BP];                                                  \
	R2 = [P5 + M_HALF_F];                                              \
	R3 = [P5 + M_HALF_Q];                                              \
	R4 = [P5 + M_WEIGHT];                                              \
	R5 = [P5 + M_WEIGHT_INCREMENT];                                    \
	I0 = P0;                                                           \
	.align 8;                                                          \
	MNOP;                                                              \
	LSETUP (label##_start, label##_end) LC0 = P1;                      \
label##_start:                                                             \
	/* lp += (f/2) bp */                                               \
	A0 = R0.H * R2.H, A1 = R0.H * R2.L (M) || R7 = [I0++] || NOP;      \
	A1 += R2.H * R0.L (M);                                             \
	A1 = A1 >>> 15;                                                    \
	R6 = (A0 += A1);                                                   \
	R1 = R1 + R6 (S);                                                  \
	/* hp/2 = x/2 - lp - (q/2) bp */                                   \
	A0 = R0.H * R3.H, A1 = R0.H * R3.L (M);                            \
	A1 += R3.H * R0.L (M);                                             \
	A1 = A1 >>> 15;                                                    \
	R6 = (A0 += A1);                                                   \
	R7 = R7 - R1 (NS);                                                 \
	R7 = R7 - R6 (NS);                                                 \
	/* bp += 4 (f/2) (hp/2) */                                         \
	A0 = R7.H * R2.H, A1 = R7.H * R2.L (M);                            \
	A1 += R2.H * R7.L (M);                                             \
	A1 = A1 >>> 15;                                                    \
	R6 = (A0 += A1);                                                   \
	R6 = R6 << 2 (S);                                                  \
	R0 = R0 + R6 (S);                                                  \
	/* out += w bp */                                                  \
	R4 = R4 + R5 (NS);                                                 \
	A0 = R4.H * R0.H, A1 = R4.H * R0.L (M) || R7 = [I1] || NOP;        \
	A1 = A1 >>> 15;                                                    \
	R6 = (A0 += A1);                                                   \
	R7 = R7 + R6 (NS);                                                 \
label##_end:                                                               \
	[I1++] = R7;                                                       \
	[P5 + M_LP] = R1;                                                  \
	[P5 + M_BP] = R0;                                                  \
	[P5 + M_WEIGHT] = R4;                                              \
	P5 += M_SIZE;

	.text
	.align 4
	.global _ModeBankProcessAsm
	.type _ModeBankProcessAsm, STT_FUNC
_ModeBankProcessAsm:
	LINK 0;
	[--SP] = (R7:4, P5:3);
	[--SP] = LC0;
	[--SP] = LT0;
	[--SP] = LB0;
	[--SP] = LC1;
	[--SP] = LT1;
	[--SP] = LB1;
	P5 = R0;                        /* modes */
	R1 >>= 1;
	P4 = R1;                        /* number of mode pairs */
	P0 = R2;                        /* in */
	P3 = [FP + 20];                 /* previous_input */
	R3 = [FP + 32];                 /* size */
	P1 = R3;

	/* The next mode's line, while the prep loop runs (mode 0's lines are
	 * fetched by its own loads right after). */
	P2 = P5;
	P2 += M_SIZE;
	PREFETCH [P2];
	P2 += M_SIZE;                   /* -> mode 2, the pass loop's first prefetch */

	/* half_x[s] = (in[s] + x_prev) >> 1; out[s] = aux[s] = 0 */
	R1 = [P3];                      /* x_prev */
	R2 = 0;
	R0 = [FP + 36];                 /* half_x */
	I0 = R0;
	R0 = [FP + 24];
	I1 = R0;                        /* out */
	R0 = [FP + 28];
	I2 = R0;                        /* aux */
	LSETUP (.Lprep_start, .Lprep_end) LC0 = P1;
.Lprep_start:
	R0 = [P0++];
	R4 = R0 + R1 (NS);
	R4 >>>= 1;
	R1 = R0;
	[I0++] = R4;
	[I1++] = R2;
.Lprep_end:
	[I2++] = R2;
	[P3] = R1;

	P0 = [FP + 36];                 /* half_x base */
	R0 = [FP + 24];
	I2 = R0;                        /* out */
	R0 = [FP + 28];
	I3 = R0;                        /* aux */
	CC = P4 == 0;
	IF CC JUMP .Ldone;
	LSETUP (.Lpair_start, .Lpair_end) LC1 = P4;
.Lpair_start:
	I1 = I2;
	PROCESS_MODE(L_mode_out)
	I1 = I3;
	PROCESS_MODE(L_mode_aux)
.Lpair_end:
	NOP;

.Ldone:
	LB1 = [SP++];
	LT1 = [SP++];
	LC1 = [SP++];
	LB0 = [SP++];
	LT0 = [SP++];
	LC0 = [SP++];
	(R7:4, P5:3) = [SP++];
	UNLINK;
	RTS;
	.size _ModeBankProcessAsm, .-_ModeBankProcessAsm
