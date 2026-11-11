/* The ensemble's delay lines and kernel block, shared by ensemble.cc and
 * ensemble_bfin.S (plain #defines: the assembly includes it too).
 *
 * Two lines (the left and right inputs), each a no-wrap int16 array as the
 * plate's (plate_layout.h): a write pointer p moves down one sample per
 * sample, p in [0, EN_SIZE); the newest sample is at p, the one d samples
 * older at p + d. A write made while p < EN_REACH is mirrored EN_SIZE
 * further on, where it's read after p wraps. So every read is a plain
 * positive offset from p, and the L registers stay 0.
 *
 * Delays at 22050 Hz: EN_BASE (6.8 ms) +- the slow sweep (up to
 * EN_SLOW_MAX samples, 3.6 ms) +- the vibrato (EN_FAST_MAX); one more
 * sample for the interpolation. */

#ifndef COOP_ENSEMBLE_ENSEMBLE_LAYOUT_H_
#define COOP_ENSEMBLE_ENSEMBLE_LAYOUT_H_

#define EN_BASE 150                    /* the delay at rest, samples */
#define EN_SLOW_MAX 80                 /* the slow sweep's peak, samples */
#define EN_FAST_MAX 7                  /* the vibrato's peak, samples */
#define EN_REACH 240                   /* > the longest read (EN_BASE + 87 + 1) */
#define EN_SIZE 2048
#define EN_WORDS (EN_SIZE + EN_REACH)  /* 2288 words per line */

/* The side kernel's block (byte offsets): one output side, three voices.
 * Voices 1 and 2 read line_a (this side's input), voice 3 line_b (the
 * other side's; the same line for a mono input). */
#define ESK_LINE_A 0      /* -> the run's first sample (its newest) */
#define ESK_LINE_B 4
#define ESK_POS 8         /* 3 delays, Q16 samples */
#define ESK_STEP 20       /* their steps per sample */
#define ESK_G12 32        /* g1 (low) | g2 (high), Q15 */
#define ESK_G12_STEP 36   /* their steps per sample (16-bit lanes) */
#define ESK_G3 40         /* g3 (low) | its step (high) */

#endif  /* COOP_ENSEMBLE_ENSEMBLE_LAYOUT_H_ */
