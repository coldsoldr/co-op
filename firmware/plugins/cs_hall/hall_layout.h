/* The hall's delay layout at 22050 Hz, shared by hall.cc and hall_bfin.S
 * (plain #defines: the assembly includes it too). hall.cc checks these
 * numbers against its size table (static_asserts).
 *
 * One no-wrap buffer, as the plate's tank (ports/fx/src/plate/
 * plate_layout.h): every access is a fixed offset from a write pointer p
 * that moves down one sample per sample, in [0, SIZE). A line at base b
 * with delay d is written at p + b and read at p + b + d. Lines are packed
 * with one sample between them; a write made while p < MAX_LINE (the
 * longest line's reach) is mirrored SIZE further on, where it's read after
 * p wraps. The longest line is placed last, so the buffer is SIZE + REACH
 * words.
 *
 * The eight FDN lines are laid out at the largest size (step 15); smaller
 * sizes only read nearer (the read offsets come from the kernel block, the
 * bases are immediates). Each line reserves HL_EXC_MAX samples beyond its
 * length for the modulation (reads span len - EXC .. len + EXC). */

#ifndef OP1FX_HALL_HALL_LAYOUT_H_
#define OP1FX_HALL_HALL_LAYOUT_H_

/* Input diffusers (fixed lengths). */
#define HL_AP1 0
#define HL_AP1_LEN 131
#define HL_AP2 132
#define HL_AP2_LEN 197
#define HL_AP3 330
#define HL_AP3_LEN 283

/* The early reflections' line: the gated, pre-delayed input; 5 + 5 taps
 * (their longest at the largest size: 1993). */
#define HL_ER 614
#define HL_ER_REACH 1993

/* The FDN lines (index 0..7: even -> left, odd -> right), longest last. */
#define HL_EXC_MAX 24
#define HL_L0 2608
#define HL_L0_LEN 1153
#define HL_L1 3786
#define HL_L1_LEN 1361
#define HL_L2 5172
#define HL_L2_LEN 1597
#define HL_L3 6794
#define HL_L3_LEN 1823
#define HL_L4 8642
#define HL_L4_LEN 2063
#define HL_L5 10730
#define HL_L5_LEN 2381
#define HL_L6 13136
#define HL_L6_LEN 2711
#define HL_L7 15872
#define HL_L7_LEN 3089
#define HL_REACH (HL_L7 + HL_L7_LEN + HL_EXC_MAX)   /* 18985 */
#define HL_MAX_LINE (HL_L7_LEN + HL_EXC_MAX)        /* 3113: mirror while p < this */
#define HL_SIZE (HL_REACH + 1)                      /* 18986 */
#define HL_WORDS (HL_SIZE + HL_REACH)               /* 37971 */

/* Pre-delay: its own plain ring (as the plate's). */
#define HL_PRE_MAX 4410                             /* 200 ms */
#define HL_PRE_WORDS 4416

/* The kernel block (byte offsets). The lines are held in pairs, packed
 * (low | high): pair 0 = lines 0 | 4 (modulation +A), pair 1 = 2 | 6 (-A),
 * pair 2 = 1 | 5 (+B), pair 3 = 3 | 7 (-B). What the sample loop loads in
 * parallel with its MACs sits below 64 (the 16-bit load form's reach). */
#define HK_OFF 0            /* 8 read offsets (bytes from p), pair order 0 4 2 6 1 5 3 7 */
#define HK_CB 32            /* 4 pairs: input coefficient b (Q15) */
#define HK_Z 48             /* 4 pairs: the damping filters' states (= their outputs) */
#define HK_EROFF 64         /* 10 early tap offsets: left 0..4, right 0..4 */
#define HK_CA 104           /* 4 pairs: feedback coefficient a (Q15) */
#define HK_GAIN 120         /* input gain, Q31 (its high half is used) */
#define HK_GSTEP 124
#define HK_DG 128           /* -g | g, the diffusers */
#define HK_C01 132          /* early taps' coefficients c0 | c1 */
#define HK_C23 136          /* c2 | c3 */
#define HK_GLC4 140         /* late gain | c4 */
#define HK_WIDTH 144        /* wa | wb */
#define HK_POSA 148         /* modulation A, Q16 samples */
#define HK_STEPA 152
#define HK_POSB 156         /* modulation B */
#define HK_STEPB 160
#define HK_XF 164           /* crossfade: the new size's weight, Q31 */
#define HK_XFSTEP 168
#define HK_E 172            /* scratch */
#define HK_O 176            /* scratch */
#define HK_XOFF 180         /* the line offsets for the size being crossfaded to */
#define HK_XEROFF 212       /* the same for the early taps */
#define HK_RNG 252          /* the dither's generator state (xorshift32) */
#define HK_DK 256           /* the dither's gain: 1 | 1, or 0 (freeze, infinite decay) */
#define HK_BYTES 260

#endif  /* OP1FX_HALL_HALL_LAYOUT_H_ */
