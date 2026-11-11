/* The plate's delay layout at 22050 Hz, shared by plate.cc and plate_bfin.S (te-op1-cfw ports/fx/src/plate, unchanged but for this guard)
 * (plain #defines: the assembly includes it too). plate.cc checks every
 * number here against Dattorro's lengths at 29761 Hz, scaled (Scaled()).
 *
 * Two no-wrap buffers, as Clouds' reverb (ports/clouds/src/core/
 * reverb_layout.h): every access is a fixed offset from a write pointer p
 * that moves down one sample per sample, in [0, SIZE). A line at base b
 * with delay d is written at p + b and read at p + b + d. Lines are packed
 * with one sample between them; a write made while p < MAX_LINE (the
 * longest line's reach) is mirrored SIZE further on, where it's read after
 * p wraps. The longest line is placed last, so the buffer is SIZE + REACH
 * words (the mirror of the last line ends where its reads do).
 *
 * Tank lines are laid out at the largest size (step 7, Dattorro's lengths);
 * smaller sizes only read nearer (the read offsets come from the kernel
 * block, the bases are immediates). The modulated allpasses reserve the
 * largest excursion (PL_EXC_MAX samples) and one sample for the
 * interpolation. */

#ifndef CS_PLATE_LAYOUT_H_
#define CS_PLATE_LAYOUT_H_

/* Input diffusers (fixed lengths): Dattorro's 142, 107, 379, 277. */
#define PL_I1 0
#define PL_I1_LEN 105
#define PL_I2 106
#define PL_I2_LEN 79
#define PL_I4 186
#define PL_I4_LEN 205
#define PL_I3 392
#define PL_I3_LEN 281
#define PL_IN_REACH (PL_I3 + PL_I3_LEN)          /* 673 */
#define PL_IN_MAX_LINE PL_I3_LEN                  /* mirror while p < this */
#define PL_IN_SIZE (PL_IN_REACH + 1)              /* 674 */
#define PL_IN_WORDS (PL_IN_SIZE + PL_IN_REACH)    /* 1347 */

/* Tank at the largest size: Dattorro's 672 (+ excursion), 4453, 1800,
 * 3720 (left) and 908 (+ excursion), 4217, 2656, 3163 (right). */
#define PL_EXC_MAX 24
#define PL_A1 0
#define PL_A1_LEN 498
#define PL_A1_REACH (PL_A1_LEN + PL_EXC_MAX + 1)  /* 523 */
#define PL_A2 524
#define PL_A2_LEN 1334
#define PL_D2 1859
#define PL_D2_LEN 2756
#define PL_A3 4616
#define PL_A3_LEN 673
#define PL_A3_REACH (PL_A3_LEN + PL_EXC_MAX + 1)  /* 698 */
#define PL_D3 5315
#define PL_D3_LEN 3124
#define PL_A4 8440
#define PL_A4_LEN 1968
#define PL_D4 10409
#define PL_D4_LEN 2343
#define PL_D1 12753
#define PL_D1_LEN 3299
#define PL_TANK_REACH (PL_D1 + PL_D1_LEN)         /* 16052 */
#define PL_TANK_MAX_LINE PL_D1_LEN
#define PL_TANK_SIZE (PL_TANK_REACH + 1)          /* 16053 */
#define PL_TANK_WORDS (PL_TANK_SIZE + PL_TANK_REACH) /* 32105 <= 32768 */

/* Pre-delay: its own plain ring (block-level, in C). */
#define PL_PRE_MAX 4410                           /* 200 ms */
#define PL_PRE_WORDS 4416

/* The tank kernel's block (byte offsets). */
#define PTK_DECAY_KLP 0     /* decay (low) | damping coefficient (high), Q15 */
#define PTK_DD1 4           /* dd1 | -dd1 */
#define PTK_DD2 8           /* -dd2 | dd2 */
#define PTK_GOUT 12         /* gout | -gout */
#define PTK_LP 16           /* lp_1 | lp_2, states */
#define PTK_POS1 20         /* A1's modulated delay, Q16 samples */
#define PTK_STEP1 24
#define PTK_POS2 28         /* A3's */
#define PTK_STEP2 32
#define PTK_OFF 36          /* 20 read offsets, bytes from p: */
#define PT_D4T 0            /*   the tails: D4, D1, A2, D2, D3, A4 */
#define PT_D1T 1
#define PT_A2T 2
#define PT_D2T 3
#define PT_D3T 4
#define PT_A4T 5
#define PT_TL 6             /*   7 left taps, then 7 right taps */
#define PT_TR 13
#define PT_NUM_OFF 20

/* The size crossfade's block. */
#define PXK_XF 0            /* the new size's weight, Q31 (high half used) */
#define PXK_XSTEP 4
#define PXK_POS1 8          /* the new size's modulated delays, Q16 */
#define PXK_STEP1 12
#define PXK_POS2 16
#define PXK_STEP2 20
#define PXK_OFF 24          /* the new size's 6 tails (PT_ order) */
#define PXK_W 48            /* scratch: 32767 - b | b, this sample's weights */
#define PXK_TMP 52          /* scratch */

/* The input kernel's block. */
#define PIK_G1 0            /* -g1 | g1 (I1, I2) */
#define PIK_G2 4            /* -g2 | g2 (I3, I4) */
#define PIK_KHP 8           /* low cut coefficient, Q15 (low half) */
#define PIK_GAIN 12         /* input gain, Q31 (its high half is used) */
#define PIK_GSTEP 16
#define PIK_HP 20           /* low cut state: the low-passed input << 16 */

#endif  /* CS_PLATE_LAYOUT_H_ */
