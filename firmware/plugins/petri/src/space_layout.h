/* petri's space: the lines' layout in its buffer (byte offsets from the moving pointer), shared by the kernel
 * (src/space_bfin.S) and its C reference (src/petri_engine.cc PetriSpaceC). At 11.025 kHz: four input allpasses
 * (37, 53, 79, 127 samples), then two tank halves, each an allpass and a delay (531 + 1069, 653 + 1389). Each line
 * takes its length + 1 entries; a line's head is written at its base, its tail read at base + length.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_PETRI_SPACE_LAYOUT_H_
#define COOP_PETRI_SPACE_LAYOUT_H_

#define SP_AP1_HEAD 0
#define SP_AP1_TAIL 74
#define SP_AP2_HEAD 76
#define SP_AP2_TAIL 182
#define SP_AP3_HEAD 184
#define SP_AP3_TAIL 342
#define SP_AP4_HEAD 344
#define SP_AP4_TAIL 598
#define SP_AP5_HEAD 600
#define SP_AP5_TAIL 1662
#define SP_DL1_HEAD 1664
#define SP_DL1_TAIL 3802
#define SP_AP6_HEAD 3804
#define SP_AP6_TAIL 5110
#define SP_DL2_HEAD 5112
#define SP_DL2_TAIL 7890
#define SP_USED_BYTES 7892          /* 3946 entries */

#define SP_G      20480             /* the input allpasses' gain, 0.625 (Q15) */
#define SP_ONE    32767             /* 1 - 2^-15: the MACs' "x 1" */
#define SP_T      16384             /* the tank allpasses' gain, 0.5 */
#define SP_DAMP   28300             /* the damping low-pass, ~3.5 kHz at 11.025 kHz */

#endif
