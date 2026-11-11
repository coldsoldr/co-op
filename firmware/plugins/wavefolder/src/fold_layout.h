/* Byte offsets of the FOLD kernel blocks, shared by fold.h (static_asserts)
 * and fold_bfin.S (plain #defines: the assembly includes this too). */

#ifndef OP1FX_SRC_FOLD_FOLD_LAYOUT_H_
#define OP1FX_SRC_FOLD_FOLD_LAYOUT_H_

/* FoldShapeBlock */
#define FSB_G 0           /* gain, Q12 in the high half */
#define FSB_GSTEP 4
#define FSB_C 8           /* symmetry offset, Q27 turns */
#define FSB_CSTEP 12
#define FSB_SIN 16        /* const uint32_t* kFoldSin */
#define FSB_SINC 20       /* const uint32_t* kFoldSinc */

/* FoldToneBlock */
#define FTB_SC 0          /* DC to remove, Q14 in the high half */
#define FTB_SCSTEP 4
#define FTB_MIX 8         /* level x mix (high half) | 32767 - mix (low half), Q15 */
#define FTB_MIXSTEP 12
#define FTB_COEF 16       /* tone coefficient (low half) | DC blocker's (high half), Q15 */

/* FoldChannel */
#define FCH_UPREV 0       /* the folder's previous input u, Q27 turns */
#define FCH_T1 4          /* tone stages, int16, Q14 */
#define FCH_T2 6
#define FCH_Z 8           /* DC blocker's low-pass, Q30 (Q14 in the high half) */

/* EXTRACT patterns (position << 8 | length) */
#define FOLD_PAT_FRAC 0x050f   /* bits 5..19: a segment's fraction, Q15 */
#define FOLD_PAT_SINIDX 0x1408 /* bits 20..27: the sine's segment */

#endif  /* OP1FX_SRC_FOLD_FOLD_LAYOUT_H_ */
