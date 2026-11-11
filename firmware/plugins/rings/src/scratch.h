// Per-call scratch buffers, shared by every voice (the OP-1 renders one
// voice at a time; nothing in them outlives a render call).
//
// On the OP-1 they sit in the L1 scratchpad (single-cycle): the mode bank
// reads and writes its output buffer once per mode and sample, and the
// string kernels their mono sum once per string and sample, so in SDRAM
// every one of those accesses risks a bus wait.
//
// The block is what one OP-1 render call (128 samples at 88.2 kHz) needs
// at the Rings rate: 64 samples at 44.1 kHz (upsampled 2x), 32 at 22.05 kHz
// (4x). src/rate_config.h.

#ifndef OP1RINGS_SCRATCH_H_
#define OP1RINGS_SCRATCH_H_

#include <cstdint>

#ifndef OP1RINGS_UPSAMPLE
#define OP1RINGS_UPSAMPLE 2
#endif

namespace op1rings {

const int32_t kScratchBlock = 128 / OP1RINGS_UPSAMPLE;   // samples per render at the Rings rate
const int32_t kScratchSubblock = 32;                     // the modal sub-block
const int32_t kScratchHalfbandHistory = 24;              // the 4x upsampler's first stage (op1/engine.cc)

struct Scratch {
  int32_t render_out[kScratchBlock];     // the voice's block, before upsampling
  int32_t wet_share[kScratchBlock];      // its share of the shared reverb
  union {
    struct {                             // the modal model's sub-block
      int32_t excitation[kScratchSubblock];
      int32_t in_scaled[kScratchSubblock];
      int32_t half_x[kScratchSubblock];
      int32_t weights_end[64];           // ModalResonator::Process: the block's end weights (kMaxModalModes)
    } modal;
    struct {                             // the string models' block
      int32_t excitation[kScratchBlock];
      int32_t sympathetic[kScratchBlock];
      int32_t mono[kScratchBlock];
    } strings;
#if OP1RINGS_UPSAMPLE == 4
    struct {                             // after the voice has rendered
      int16_t x[kScratchHalfbandHistory + kScratchBlock];   // history + the block, 22.05 kHz (16-bit: clamped)
      int16_t xs[kScratchHalfbandHistory + kScratchBlock];  // x shifted by one sample, for the kernel's aligned reads
      int32_t mid[3 + 2 * kScratchBlock];                   // history + the block at 44.1 kHz
    } upsampler;
#else
    int32_t upsampler[kScratchBlock + 3];   // after the voice has rendered
#endif
  };
};

// coop: the core lends the L1 scratchpad per call (coop_l1_scratch, sdk/
// coop_plugin.h: 1312 bytes at the window kennel rings used, valid only
// inside the engine's own note_on and render). rings.cc points g_scratch at
// it at the top of every note_on and render, or at an SDRAM fallback when
// the core lends less than a Scratch; nothing here names an address.
extern Scratch* g_scratch;
inline Scratch* scratch() { return g_scratch; }

}  // namespace op1rings

#endif  // OP1RINGS_SCRATCH_H_
