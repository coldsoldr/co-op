// TAPE ECHO: the tape buffer's size and the run kernel's block offsets,
// shared by tape_echo.cc and tape_echo_bfin.S (plain #defines: the
// assembler includes this file).

#ifndef OP1FX_TAPE_ECHO_TAPE_ECHO_LAYOUT_H_
#define OP1FX_TAPE_ECHO_TAPE_ECHO_LAYOUT_H_

// The tape: TE_TAPE_SIZE stereo pairs (L in the low half, R in the high
// half of each 32-bit word), one ring, plus one mirror pair at
// [TE_TAPE_SIZE] that repeats [0] (for the interpolation's second tap).
// 1.5 s at 22050 Hz is 33075 samples; + 112 of wow and flutter, + 1 for
// the interpolation, + a margin.
#define TE_TAPE_SIZE 33216
#define TE_TAPE_PAIRS (TE_TAPE_SIZE + 1)

// The run kernel's block (TapeEchoKernel, all int32), byte offsets.
#define TEK_POS0 0       // head delays, Q15 samples
#define TEK_POS1 4
#define TEK_POS2 8
#define TEK_STEP0 12     // per sample
#define TEK_STEP1 16
#define TEK_STEP2 20
#define TEK_W0 24        // head weights: out (low, Q15) | feedback (high, Q15)
#define TEK_W1 28
#define TEK_W2 32
#define TEK_WS0 36       // their per-sample steps (per half)
#define TEK_WS1 40
#define TEK_WS2 44
#define TEK_GAIN 48      // input gain, Q31 (the high half is used)
#define TEK_GSTEP 52
#define TEK_HPL 56       // low cut states, 32 bits (the low-pass's value << 16)
#define TEK_HPR 60
#define TEK_LP 64        // tape loss states: L (low) | R (high)
#define TEK_K 68         // coefficients: low cut (low, Q15) | tape loss (high, Q15)
#define TEK_ADJ0 72      // per run: added to each head's read address (0 or -4 N: the wrap)
#define TEK_ADJ1 76
#define TEK_ADJ2 80
#define TEK_SAT 84       // the assembly's scratch: the saturation table's centre
#define TEK_SIZE 88

#endif  // OP1FX_TAPE_ECHO_TAPE_ECHO_LAYOUT_H_
