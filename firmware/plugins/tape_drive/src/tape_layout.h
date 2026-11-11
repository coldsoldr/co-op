// TAPE's kernel blocks: byte offsets shared by tape.h (static_asserts) and
// tape_bfin.S. Every field is an int32; a "pair" is two int16 halves,
// high << 16 | low.

#ifndef OP1FX_SRC_TAPE_TAPE_LAYOUT_H_
#define OP1FX_SRC_TAPE_TAPE_LAYOUT_H_

// The wobble delay: a ring of TAPE_RING words (one L | R pair per sample),
// every write mirrored TAPE_RING words further on, so a read pair at
// idx, idx + 1 never wraps.
#define TAPE_RING 256
#define TAPE_RING_WORDS (2 * TAPE_RING)

// TapeDelayKernel
#define TDK_POS 0        // the read position, Q16: widx << 16 plus the delay
#define TDK_STEP 4       // the delay's change, Q16 per sample
#define TDK_WIDX 8       // 0..TAPE_RING-1: the last write

// TapeHbKernel (the halfband up- and downsamplers)
#define THK_X 0          // pair: the previous input (up: x | x; down: odd | even)
                         // (up: s2's high half is the last odd output)
#define THK_S1 4         // pair: the first allpasses' previous outputs
#define THK_S2 8         // pair: the second allpasses' previous outputs
#define THK_C1 12        // pair: c1 | c0
#define THK_C2 16        // pair: c3 | c2

// TapeSatKernel
#define TSK_HSH 0        // pair: h | shift
#define TSK_K12 4        // pair: K1 | K2
#define TSK_K0 8         // pair: K0 | 32767
#define TSK_J 12         // the output shift

// TapeToneKernel
#define TTK_A0 0         // the bump's 180 Hz low-pass (A0), lp << 16
#define TTK_A1 4         // its 45 Hz low-pass (A1), lp << 16
#define TTK_LP 8         // pair: their rounded outputs, lpb | lpa
#define TTK_S1 12        // the HF loss's first one-pole, Q16 + 0x8000 (the
                         // high half is the rounded value)
#define TTK_S2 16        // its second
#define TTK_KAB 20       // pair: kb | ka
#define TTK_GK 24        // pair: gb (bump gain) | k (HF loss)
#define TTK_MIX 28       // pair: step | mix

// TapeLaneKernel: one lane's kernels, in a row.
#define TLK_UP 0
#define TLK_DOWN 20
#define TLK_TONE 40

#endif  // OP1FX_SRC_TAPE_TAPE_LAYOUT_H_
