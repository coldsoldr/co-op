// PLATE: Dattorro's figure-of-eight plate reverb ("Effect Design Part 1",
// JAES 1997) in fixed point, at 22050 Hz (half the OP-1's rate: the glue
// decimates its input 2:1 and interpolates the wet signal back, half_rate.h).
// Wet only. te-op1-cfw ports/fx/src/plate (9ced415), docs/plate-port.md
// there has the design, the formats and the measurements.
//
// coop port (plugins/cs_plate, 2026-11-11), the DSP unchanged; what moved:
//   - namespace csplate (the host test links the upstream op1fx beside it);
//   - the parameter and size tables are integer literals (plate.cc), not
//     constexpr double: coop has no float or double in a plugin at all
//     (guide/plugin-guidelines.md §2.1); tests/host/cs_plate checks them
//     against the upstream's;
//   - Init() is split: the buffers are cleared by the glue a chunk a block
//     (cs_plate.cc, the slow-init rule §2.3), Reset() does the rest (cheap);
//   - the Blackfin always runs the assembly; the C references are host only
//     (they are the reference the assembly matches bit for bit).
//
// Signal path per sample (mono sum in, stereo out):
//   pre-delay (0..200 ms, its own ring, gliding read tap)
//   -> low cut (one-pole high-pass, 20..500 Hz) -> input gain (0.25; the
//      tank's headroom, made up at the output as Clouds' reverb does;
//      ramps to 0 on freeze) -> 4 input allpasses (diffusion)
//   -> tank, two halves in a figure of eight; each half: + decay x the
//      other half's end -> modulated allpass (-dd1) -> delay -> damping
//      low-pass -> x decay -> allpass (dd2) -> delay
//   -> 7 + 7 output taps (Dattorro's table), x 0.6, x 2 (the makeup).
//
// Arithmetic: the Blackfin's 16-bit MACs: int16 buffers; a 40-bit
// accumulator between stages; everything written to the buffers is the
// accumulator's high half, rounded (unbiased) and saturated. Coefficients
// Q15.
//
// Changing `size` (a step index 0..7): the wet fades out (4 calls); then,
// the output muted, the tank's reads crossfade from the old lengths to the
// new ones over 256 samples (8 calls); then the wet fades back in (8 calls).

#ifndef CS_PLATE_PLATE_H_
#define CS_PLATE_PLATE_H_

#include <stdint.h>

#include "plate_layout.h"

namespace csplate {

struct PlateParams {
  int32_t decay;       // 0..32767 -> tank gain 0.2..0.98
  int32_t damp;        // 0..32767 bright -> dark
  int32_t predelay;    // 0..32767 -> 0..200 ms
  int32_t size;        // 0..7, tank length 0.6..1.0 (geometric steps)
  int32_t diffusion;   // 0..32767 -> input allpass gains 0..Dattorro's
  int32_t motion;      // 0..32767 -> excursion 0..2x Dattorro's
  int32_t lowcut;      // 0..32767 -> 20..500 Hz (log)
  bool freeze;         // decay 1, damping off, input off: the tail holds
};

const int32_t kPlateMaxBlock = 64;
const int32_t kPlateNumSizes = 8;

// Run kernel blocks (plate_layout.h's PTK_ / PXK_ / PIK_ offsets).
struct PlateTankKernel {
  int32_t decay_klp;
  int32_t dd1;
  int32_t dd2;
  int32_t gout;
  int32_t lp;
  int32_t pos1, step1;
  int32_t pos2, step2;
  int32_t off[PT_NUM_OFF];
};

struct PlateXfadeKernel {
  int32_t xf, xf_step;              // Q31
  int32_t pos1, step1;              // the new size's modulated delays, Q16
  int32_t pos2, step2;
  int32_t off[6];                   // the new size's tails
  int32_t w, tmp;                   // the assembly's scratch
};

struct PlateInputKernel {
  int32_t g1;
  int32_t g2;
  int32_t khp;
  int32_t gain, gstep;
  int32_t hp;
};

// plate_bfin.S. p: the buffer at the write pointer; each sample first
// moves it down one. Every write also goes alias_bytes further on. n > 0.
extern "C" void PlateInputRunAsm(PlateInputKernel* k, const int16_t* x, int16_t* out, int16_t* p,
                                 int32_t n, int32_t alias_bytes);
extern "C" void PlateTankRunAsm(PlateTankKernel* k, const int16_t* x, int16_t* l, int16_t* r,
                                int16_t* p, int32_t n, int32_t alias_bytes);
extern "C" void PlateXfadeRunAsm(PlateTankKernel* k, PlateXfadeKernel* xk, const int16_t* x,
                                 int16_t* p, int32_t n, int32_t alias_bytes);
extern "C" void PlatePreRunAsm(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x,
                               int32_t w, int32_t d, int32_t step, int32_t n);

#if !defined(__bfin__)
// The C references (host only; plate_bfin.S matches them bit for bit,
// te-op1-cfw ports/fx/tests/test_plate.cc).
void PlatePreRunC(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x, int32_t w,
                  int32_t d, int32_t step, int32_t n);
void PlateInputRunC(PlateInputKernel* k, const int16_t* x, int16_t* out, int16_t* p, int32_t n,
                    int32_t alias_bytes);
void PlateTankRunC(PlateTankKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p,
                   int32_t n, int32_t alias_bytes);
void PlateXfadeRunC(PlateTankKernel* k, PlateXfadeKernel* xk, const int16_t* x, int16_t* p,
                    int32_t n, int32_t alias_bytes);
#endif

struct PlateCoefs {
  int32_t decay, dd1, dd2, klp;     // Q15
  int32_t g1, g2, khp;              // Q15
  int32_t gain0, gain_step;         // input gain, Q31
  int32_t pre0, pre_step;           // pre-delay, Q16 samples
  int32_t mod0[2], mod_step[2];     // modulated allpass delays, Q16 samples
  int32_t fade0, fade_step;         // wet fade, Q31 (kFadeOne and 0: none)
  int32_t size;                     // 0..7
  bool xfade;
  int32_t size_new;
  int32_t xmod0[2], xmod_step[2];
  int32_t xf0, xf_step;
};

struct PlateSizeTable {
  int32_t a1, a3;                   // the modulated allpasses' delays
  int32_t tail[6];                  // D4, D1, A2, D2, D3, A4 (PT_ order)
  int32_t tap_l[7], tap_r[7];       // Dattorro's taps, in his order
};
extern const PlateSizeTable kPlateSizes[kPlateNumSizes];
extern const int8_t kPlateTapSign[7];

class PlateControl {
 public:
  static const int32_t kFadeOne = 0x7fff0000;
  static const int32_t kXfadeSamples = 256;
  void Init();
  // n even, 2..kPlateMaxBlock.
  void Update(const PlateParams& p, int32_t n, PlateCoefs* c);

 private:
  int32_t ModTarget(int32_t size, int32_t j) const;
  void UpdateMod(int32_t n, PlateCoefs* c);

  int32_t decay_, damp_, diffusion_, motion_, lowcut_;   // smoothed, Q15 << 8
  int32_t gain_;                    // Q31
  int32_t pre_;                     // Q16
  int32_t mod_[2], xmod_[2];        // Q16
  uint32_t lfo_phase_;
  int32_t size_, size_new_, fade_, xf_;
  bool xfading_;
  bool started_;
  bool steady_;
  PlateParams last_;
  PlateCoefs cached_;
};

class PlateReverb {
 public:
  // Everything but the buffers, which must already be zero (the glue
  // clears them a chunk a block; ClearBuffers() at once, for tests). Cheap.
  void Reset();
  void ClearBuffers();
  // n even, 2..64. Stereo in, stereo out (the input is summed into the
  // pre-delay, as Dattorro's plate). Wet at the input's scale, saturated.
  void Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l, int16_t* wet_r,
               int32_t n, const PlateParams& p);

 private:
  void Pre(const int16_t* in_l, const int16_t* in_r, int32_t n);

  PlateControl control_;
  PlateCoefs coefs_;
  PlateInputKernel ik_;
  PlateTankKernel tk_;
  PlateXfadeKernel xk_;
  int32_t tank_size_;               // the size tk_'s offsets are for
  int32_t in_ptr_;                  // [1, PL_IN_SIZE]: the next sample is at in_ptr - 1
  int32_t tank_ptr_;                // [1, PL_TANK_SIZE]
  int32_t pre_w_;                   // the pre-delay ring's next write
  int16_t x_[kPlateMaxBlock];       // the pre-delayed input, then the diffused input
  int16_t pre_[PL_PRE_WORDS];
  int16_t in_buf_[PL_IN_WORDS];
  int16_t tank_[PL_TANK_WORDS];
};

}  // namespace csplate

#endif  // CS_PLATE_PLATE_H_
