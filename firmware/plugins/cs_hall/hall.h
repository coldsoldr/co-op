// coop port (plugins/cs_hall): te-op1-cfw ports/fx/src/hall/hall.h with the
// include path changed and InitKeepBuffers / ClearBase / ClearWords added (the
// tank's clear moved out of init). pre_ and buf_ must stay adjacent (a static
// assert in cs_hall.cc checks it).
//
// HALL: an 8-line feedback delay network hall reverb (Jot's FDN) in fixed
// point, at 22050 Hz (half the OP-1's rate: the glue decimates its input
// 2:1 and interpolates the wet signal back, as for the plate). Wet only.
// docs/hall-port.md has the design, the formats and the measurements.
//
// Signal path per sample (mono sum in, stereo out):
//   pre-delay (0..200 ms, its own ring, gliding read tap)
//   -> input gain (0.25: headroom, made up x 2 at the output; ramps to 0
//      on freeze)
//   -> the early reflections' line (5 + 5 taps, scaled with size)
//   -> 3 input allpasses (diffusion) -> x 1/2 -> into the 8 lines
//      (signs + + + - - + - -)
//   -> 8 delay lines (lengths scaled with size, primes, 10..140 ms), each
//      read with a modulated, interpolated tap (two LFOs, each + and -)
//   -> per line a one-pole damping filter that is also the line's loss
//      (Jot: DC gain g = 10^(-3 L / (fs RT60)), Nyquist gain g^ratio)
//   -> Householder feedback (x - (2/8) sum x: adds and one rounding)
//   -> even lines -> left, odd -> right, + early taps, width matrix, x 2.
//
// Arithmetic: the Blackfin's 16-bit MACs, as the plate: int16 buffers; a
// 40-bit accumulator; everything written to the buffers is rounded with the
// MAC's unbiased (ties-to-even) rounding and saturated, so a frozen network
// doesn't creep. The damping filters (the lines' loss) add a +-1/2 LSB
// dither before rounding (stochastic rounding), so a decaying tail has no
// dead band (limit cycles that would never die); frozen or at infinite
// decay the dither is off and the loop is exactly lossless. The run kernels have C references (HallRunC, ...) and
// Blackfin versions (hall_bfin.S, bit-exact; tests/test_hall.cc).
//
// Size: 16 steps (0.2 .. 1.0 of the largest lengths, geometric). A change
// crossfades every size-dependent read (the eight lines and the early taps)
// from the old length to the new over 256 samples (8 calls). The output is
// made from the lines' reads, so it is crossfaded too: no mute, no click.

#ifndef OP1FX_HALL_HALL_H_
#define OP1FX_HALL_HALL_H_

#include <cstdint>

#include "hall_layout.h"

namespace op1fx {

struct HallParams {     // 0..32767 each
  int32_t size;         // room -> hall: 16 steps, line scale 0.2 .. 1.0
  int32_t decay;        // RT60 0.3 .. 20 s (log); >= 32512: infinite
  int32_t damp;         // HF (Nyquist) RT60 = RT60 / ratio, ratio 1 .. 10 (log)
  int32_t predelay;     // 0 .. 200 ms
  int32_t early;        // early taps x EARLY; the late part full to 16384, then to 0
  int32_t motion;       // line modulation 0 .. 16 samples peak
  int32_t width;        // stereo width: 0 mono .. 32767 full (stereo out only)
  bool freeze;          // loss 0, damping off, input gated off: the tail holds
};

const int32_t kHallMaxBlock = 64;
const int32_t kHallNumSizes = 16;
const int32_t kHallInfinite = 32512;   // decay at and above: no loss

// The run kernel's block (hall_layout.h's HK_ offsets).
struct HallKernel {
  int32_t off[8];         // pair order: lines 0 4 2 6 1 5 3 7
  int32_t cb[4];          // packed pairs
  int32_t z[4];
  int32_t eroff[10];
  int32_t ca[4];
  int32_t gain, gstep;
  int32_t dg;
  int32_t c01, c23, glc4;
  int32_t width;
  int32_t posa, stepa, posb, stepb;
  int32_t xf, xfstep;
  int32_t e, o;           // scratch
  int32_t xoff[8];
  int32_t xeroff[10];
  uint32_t rng;           // the dither's xorshift32 state
  int32_t dk;             // the dither's gain pair: 1 | 1, or 0
};

// p: the buffer at the write pointer; each sample first moves it down one.
// Every write also goes alias_bytes further on (0: the same place again).
// n > 0. x: the pre-delayed mono input.
extern "C" void HallRunAsm(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p,
                           int32_t n, int32_t alias_bytes);
extern "C" void HallRunMonoAsm(HallKernel* k, const int16_t* x, int16_t* l, int16_t* p, int32_t n,
                               int32_t alias_bytes);
// The same outside the mirrored region (alias_bytes must be 0): each write
// once.
extern "C" void HallRunNmAsm(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p,
                             int32_t n, int32_t alias_bytes);
extern "C" void HallRunMonoNmAsm(HallKernel* k, const int16_t* x, int16_t* l, int16_t* p,
                                 int32_t n, int32_t alias_bytes);
extern "C" void HallXfadeRunAsm(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r,
                                int16_t* p, int32_t n, int32_t alias_bytes);
// The pre-delay, as the plate's: (l + r) >> 1 into ring[w..w+n) (no wrap:
// the caller splits), and x[i] = the ring read d_i samples back (Q16,
// d_i = d + i step), two taps, weights 32767 - f and f, rounded as the MAC.
extern "C" void HallPreRunAsm(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x,
                              int32_t w, int32_t d, int32_t step, int32_t n);
void HallPreRunC(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x, int32_t w,
                 int32_t d, int32_t step, int32_t n);
// r == nullptr: mono (the left output only).
void HallRunC(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p, int32_t n,
              int32_t alias_bytes);
// Every size-dependent read crossfaded from off/eroff to xoff/xeroff by xf;
// stereo out.
void HallXfadeRunC(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p, int32_t n,
                   int32_t alias_bytes);

// One call's control, from the parameters. The fixed-point hall and the
// float reference (tests/hall_float_ref.cc) run from the same values.
struct HallCoefs {
  int32_t a[8], b[8];               // damping per line (line order), Q15
  int32_t c[5];                     // early taps (signed, incl. EARLY), Q15
  int32_t gl;                       // late gain, Q15
  int32_t wa, wb;                   // width matrix, Q15
  int32_t dg;                       // diffusers' gain, Q15
  int32_t dither;                   // 1: the damping rounds stochastically; 0 (freeze, infinite)
  int32_t gain0, gain_step;         // input gain, Q31
  int32_t pre0, pre_step;           // pre-delay, Q16 samples
  int32_t mod0[2], mod_step[2];     // excursions A, B, Q16 samples
  int32_t size;                     // 0..15
  bool xfade;                       // a crossfade to size_new
  int32_t size_new;
  int32_t xf0, xf_step;             // its weight, Q31
};

struct HallSizeTable {
  int32_t len[8];                   // the lines (line order)
  int32_t er_l[5], er_r[5];         // the early taps
};
extern const HallSizeTable kHallSizes[kHallNumSizes];
// Early tap coefficients at full EARLY (the same for both sides), Q15.
extern const int16_t kHallEarlyCoef[5];
// Line index of each kernel slot (pair order), and the input's sign.
extern const int8_t kHallSlotLine[8];
extern const int8_t kHallSlotSign[8];

class HallControl {
 public:
  static const int32_t kOne = 0x7fff0000;
  static const int32_t kXfadeSamples = 256;
  void Init();
  // n even, 2..kHallMaxBlock.
  void Update(const HallParams& p, int32_t n, HallCoefs* c);
  // Tests: false switches sizes at once (no crossfade).
  void set_size_fade(bool fade) {
    size_fade_ = fade;
    steady_ = false;
  }

 private:
  void Coefficients(HallCoefs* c) const;
  void LineCoefs(int32_t size, int32_t* a, int32_t* b) const;
  void UpdateMod(int32_t n, HallCoefs* c);

  int32_t decay_, damp_, early_, motion_, width_;   // smoothed knobs, Q15 << 8
  int32_t freeze_;                  // 0..32768, ramped
  int32_t gain_;                    // Q31
  int32_t pre_;                     // Q16
  int32_t mod_[2];                  // Q16
  uint32_t phase_[2];
  int32_t size_, size_new_, xf_, xs_;   // xs_: the crossfade's samples so far
  bool xfading_;
  bool started_;
  bool size_fade_;
  bool steady_;
  HallParams last_;
  HallCoefs cached_;
};

class HallReverb {
 public:
  void Init();
  // coop: Init without clearing the two buffers (the pre-delay ring and the
  // tank, 84 KB: too slow for an init on the audio thread, guidelines 2.3).
  // The caller then zeroes ClearWords() int16 words from ClearBase() before
  // the next Process (plugins/cs_hall/cs_hall.cc spreads it over blocks).
  void InitKeepBuffers();
  int16_t* ClearBase() { return pre_; }
  static int32_t ClearWords() { return HL_PRE_WORDS + HL_WORDS; }
  // n even, 2..64. in_r null: mono input; wet_r null: mono output (the
  // left side, cheaper). Wet at the input's scale, saturated.
  void Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l, int16_t* wet_r,
               int32_t n, const HallParams& p);

  // Tests.
  void set_use_asm(bool use_asm) { use_asm_ = use_asm; }
  void set_size_fade(bool fade) { control_.set_size_fade(fade); }
  const HallCoefs& coefs() const { return coefs_; }
  const int16_t* buffer() const { return buf_; }
  int32_t write_ptr() const { return ptr_; }

 private:
  void Pre(const int16_t* in_l, const int16_t* in_r, int32_t n);

  HallControl control_;
  HallCoefs coefs_;
  HallKernel k_;
  int32_t k_size_;                  // the size k_'s offsets are for
  int32_t ptr_;                     // [1, HL_SIZE]: the next sample is at ptr - 1
  int32_t pre_w_;
  bool use_asm_;
  int16_t x_[kHallMaxBlock];        // the pre-delayed input
  int16_t r_[kHallMaxBlock];        // the right side of a mono output's crossfade
  int16_t pre_[HL_PRE_WORDS];
  int16_t buf_[HL_WORDS];
};

}  // namespace op1fx

#endif  // OP1FX_HALL_HALL_H_
