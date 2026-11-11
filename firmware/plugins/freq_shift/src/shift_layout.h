// SHIFT's kernel blocks: byte offsets shared by shift.h (static_asserts)
// and shift_bfin.S. Plain #defines so the assembler can include it.

#ifndef OP1FX_SRC_SHIFT_SHIFT_LAYOUT_H_
#define OP1FX_SRC_SHIFT_SHIFT_LAYOUT_H_

#define SH_MAX_BLOCK 64

/* ShiftHilbertKernel */
#define SHK_XH      0     /* int32 xh[3]: x[n-1], x[n-2], x[n-3] */
#define SHK_Y       12    /* int32 y[8][2]: per section y[n-1], y[n-2] */
#define SHK_M       76    /* int32: Q gain (ring 0 <-> shift 32768), Q15 */
#define SHK_DM      80    /* int32: its step per sample */
#define SHK_COEF    84    /* int16 coef[8] */
#define SHK_SIZE    100

/* The Hilbert kernel's scratch: five int32 runs of SH_RUN words, each with
 * room for history before (3 words for X, 2 for the others) and 2 words
 * of read-ahead after: X (the input), A/B (the I path), C/D (the Q path). */
#define SH_RUN          72
#define SH_SCRATCH      (5 * SH_RUN)
#define SH_X            3                 /* X[0] */
#define SH_A            (SH_RUN + 2)
#define SH_B            (2 * SH_RUN + 2)
#define SH_C            (3 * SH_RUN + 2)
#define SH_D            (4 * SH_RUN + 2)

/* ShiftChannelKernel */
#define SCK_PHASE   0     /* uint32 */
#define SCK_INC     4     /* int32: phase step */
#define SCK_DINC    8     /* int32: its step per sample */
#define SCK_G       12    /* int32 feedback, Q15 */
#define SCK_DG      16    /* its step per sample, Q15 */
#define SCK_MIX     20    /* int32 mix, Q15 */
#define SCK_DMIX    24    /* its step per sample, Q15 */
#define SCK_LP      28    /* uint32 packed: loop low-pass state, re low / im high */
#define SCK_POS     32    /* int32: delay index in bytes, 0..1020 */
#define SCK_OSC     36    /* const uint32_t*: kShiftOsc */
#define SCK_DELAY   40    /* uint32 delay[256] packed re low / im high */
#define SCK_SIZE    1064

#define SH_DELAY    256   /* feedback loop length, samples */
#define SH_LP_K     19661 /* loop low-pass: lp += 0.6 (w - lp) */
#define SH_LP_K2    13107 /* 32768 - SH_LP_K */

#endif  /* OP1FX_SRC_SHIFT_SHIFT_LAYOUT_H_ */
