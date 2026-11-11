// FOLD: a sine wavefolder with first-order antiderivative anti-aliasing
// (ADAA), for the OP-1's dirt category (te-op1-cfw docs/fx-briefs/fold.md,
// docs/fold-port.md). Fixed point, at the effect rate (44.1 kHz, in place,
// n even 2..64, normally 64).
//
// coop copy (plugins/wavefolder): te-op1-cfw ports/fx/src/fold at 5aa314b,
// the DSP unchanged. Changed for coop: Process says whether it wrote the
// output (false = mix 0 bypass, the input untouched), ChannelsEqual /
// MirrorChannels serve the wrapper's L == R shortcut, the test hooks
// (set_use_asm) are gone, and the Blackfin build carries no C kernels (the
// assembly runs every even n; the wrapper only passes even n).
//
// Per sample and channel:
//   u    = x G + c                   Q27 turns: G = fold gain (0.25 .. 4
//                                    turns per full scale), c = symmetry
//                                    offset (+-1/8 turn)
//   w    = (F(u) - F(u')) / (u - u') with f(u) = sin(2 pi u), F its
//          antiderivative, u' the previous u. By cos a - cos b =
//          2 sin((a+b)/2) sin((b-a)/2) this is exactly
//            sin(pi (u + u')) sinc(pi (u - u'))
//          so there is no division and no ill-conditioning at u = u'
//          (sinc(0) = 1): two table lookups and a multiply. Q14.
//   -> minus sin(2 pi c) (the offset's DC, as the folder makes it for
//      silence) -> two one-pole low-passes (tone, int16 states) -> DC
//      blocker (10 Hz, 32-bit state) -> x level compensation -> mix.
//
// Kernels: Shape (x -> w, per channel, into a scratch block; stride 2
// interleaves a stereo pair) and Tone (w -> filters, DC, level, mix, back
// in place; a mono kernel and a stereo one that runs both channels in the
// two halves of each register). Each has a C reference and a Blackfin
// version (fold_bfin.S), bit-exact (tests/test_fold.cc). Parameters are
// smoothed per call (1/4 of the way, about 5 ms) and ramped per sample in
// the kernels.

#ifndef OP1FX_SRC_FOLD_FOLD_H_
#define OP1FX_SRC_FOLD_FOLD_H_

#include <cstddef>
#include <cstdint>

#include "src/fold_layout.h"

namespace op1fx {

struct FoldParams {
  int32_t fold;       // 0..32767: gain into the folder, 0.25 -> 4 turns per full scale
  int32_t symmetry;   // 0..32767, 16384 = symmetric: offset -1/8 .. +1/8 turn
  int32_t tone;       // 0..32767: dark (300 Hz, 2 poles) -> open
  int32_t mix;        // 0..32767: dry -> wet
  // An input gain into the folder, Q14 (16384 = 0 dB; the kennel plugin's
  // page-2 word, -6..+6 dB). It scales the folder's gain only, kept within
  // the fold knob's own range (0.25..4 turns), so at either end of the knob
  // the gain adds nothing; the level make-up follows the knob alone, and the
  // dry side of the mix never sees it.
  int32_t gain = 16384;
};

// Kernel blocks (offsets in fold_layout.h). The ramps: each sample first
// adds the step, then uses the value.
struct FoldShapeBlock {
  int32_t g, g_step;          // gain, Q12 in the high half
  int32_t c, c_step;          // offset, Q27 turns
  const uint32_t* sin_tab;    // kFoldSin
  const uint32_t* sinc_tab;   // kFoldSinc
};

struct FoldToneBlock {
  int32_t sc, sc_step;        // DC to remove, Q14 in the high half
  int32_t mix, mix_step;      // level x mix (high half) | 32767 - mix (low half), Q15;
                              // one 32-bit ramp (the low half stays in 0..32767)
  int32_t coef;               // tone coefficient (low half) | DC blocker's << 16, Q15
};

struct FoldChannel {
  int32_t u_prev;             // Q27 turns
  int16_t t1, t2;             // tone stages, Q14
  int32_t z;                  // DC blocker's low-pass, Q30
};

static_assert(offsetof(FoldShapeBlock, g) == FSB_G, "layout");
static_assert(offsetof(FoldShapeBlock, g_step) == FSB_GSTEP, "layout");
static_assert(offsetof(FoldShapeBlock, c) == FSB_C, "layout");
static_assert(offsetof(FoldShapeBlock, c_step) == FSB_CSTEP, "layout");
static_assert(offsetof(FoldShapeBlock, sin_tab) == FSB_SIN, "layout");
static_assert(sizeof(void*) != 4 || offsetof(FoldShapeBlock, sinc_tab) == FSB_SINC, "layout");
static_assert(offsetof(FoldToneBlock, sc) == FTB_SC, "layout");
static_assert(offsetof(FoldToneBlock, sc_step) == FTB_SCSTEP, "layout");
static_assert(offsetof(FoldToneBlock, mix) == FTB_MIX, "layout");
static_assert(offsetof(FoldToneBlock, mix_step) == FTB_MIXSTEP, "layout");
static_assert(offsetof(FoldToneBlock, coef) == FTB_COEF, "layout");
static_assert(offsetof(FoldChannel, u_prev) == FCH_UPREV, "layout");
static_assert(offsetof(FoldChannel, t1) == FCH_T1, "layout");
static_assert(offsetof(FoldChannel, t2) == FCH_T2, "layout");
static_assert(offsetof(FoldChannel, z) == FCH_Z, "layout");

// C references (fold.cc). n > 0; the assembly needs n even.
// Shape: x[0..n) -> w[0], w[stride], ... (stride 1 or 2).
void FoldShapeRunC(const FoldShapeBlock* k, FoldChannel* ch, const int16_t* x, int16_t* w,
                   int32_t stride, int32_t n);
// Tone, mono: w[0..n) -> io in place.
void FoldToneRunC(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w, int16_t* io,
                  int32_t n);
// Tone, stereo: w interleaved (l, r, l, r ...) -> l, r in place.
void FoldToneStereoRunC(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w, int16_t* l,
                        int16_t* r, int32_t n);

extern "C" void FoldShapeRunAsm(const FoldShapeBlock* k, FoldChannel* ch, const int16_t* x,
                                int16_t* w, int32_t stride, int32_t n);
extern "C" void FoldToneRunAsm(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w,
                               int16_t* io, int32_t n);
// ch: both channels (ch[0], ch[1]).
extern "C" void FoldToneStereoRunAsm(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w,
                                     int16_t* l, int16_t* r, int32_t n);

// The Blackfin's signed fractional 16 x 16 multiply into a half register
// (Rd.L = Rs.L * Rt.L): (a b) << 1, rounded to 16 bits with the default
// unbiased (ties-to-even) rounding, saturated.
inline int32_t FoldMulFrac(int32_t a, int32_t b) {
  const int32_t p = a * b;                    // int16 operands: |p| <= 2^30
  if (p == 0x40000000) return 32767;          // -1 x -1 saturates
  const int32_t x = p * 2;
  int32_t r = x >> 16;
  const int32_t rem = x & 0xffff;
  if (rem > 0x8000 || (rem == 0x8000 && (r & 1))) ++r;
  return r;
}

// A packed table segment (value | delta << 16) interpolated at fr (Q15).
inline int32_t FoldInterp(uint32_t e, int32_t fr) {
  const int32_t v = static_cast<int16_t>(e & 0xffff);
  const int32_t d = static_cast<int16_t>(e >> 16);
  return static_cast<int16_t>(v + FoldMulFrac(d, fr));
}

// sin(2 pi phase), phase in Q32 turns, Q15.
int32_t FoldSinQ15(uint32_t phase);

class Fold {
 public:
  void Init();
  // r = nullptr: mono (channel 0 only). Returns false when the mix sits at
  // 0: l / r untouched (the kernels ran into the sink).
  bool Process(int16_t* l, int16_t* r, int32_t n, const FoldParams& p);

  // The two channels' states are equal: a mono run on channel 0 then gives
  // exactly what the stereo run would on both, when L == R.
  bool ChannelsEqual() const {
    return ch_[0].u_prev == ch_[1].u_prev && ch_[0].t1 == ch_[1].t1 && ch_[0].t2 == ch_[1].t2 &&
           ch_[0].z == ch_[1].z;
  }
  // After such a mono run: channel 1 takes channel 0's state.
  void MirrorChannels() { ch_[1] = ch_[0]; }
  const FoldChannel& channel(int32_t i) const { return ch_[i]; }

 private:
  void Control(int32_t n, const FoldParams& p);

  FoldShapeBlock shape_;
  FoldToneBlock tone_;
  FoldChannel ch_[2];
  int32_t knob_[5];             // smoothed fold, symmetry, tone, mix, gain
  int32_t last_[5];             // ... as of the last full control update
  int32_t g_, c_, sc_, lm_, mm_;  // the ramps' values at the end of the last call
  bool first_;
  alignas(4) int16_t w_[128];   // the folder's output (stereo: interleaved; 32-bit reads)
  int16_t sink_[128];           // where the output goes while the mix is 0
};

}  // namespace op1fx

#endif  // OP1FX_SRC_FOLD_FOLD_H_
