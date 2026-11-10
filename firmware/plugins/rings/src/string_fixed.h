// Fixed-point versions of Rings' string (rings/dsp/string.cc) and plucker
// (rings/dsp/plucker.h), for the OP-1 build. Upstream's float classes stay in
// src/string.* and src/plucker.h for the harness build (src/part.cc).
//
// Formats:
//   string signals Q25 (+-64). Values written to the delay line are clamped
//     to +-7, so the Hermite interpolator's terms (up to 9x a sample) can't
//     overflow; upstream's strings stay within about +-2;
//   delays in samples, Q19 (up to 4096);
//   coefficients Q31.
//
// The per-sample arithmetic follows upstream, in the same order. Two
// changes:
//   - the damping low-pass (stmlib::Svf, resonance 0.5) runs as two
//     identical TPT one-poles: at resonance 0.5 the SVF is critically
//     damped and its low-pass is exactly that cascade, which needs only
//     g / (1 + g) < 1 instead of g and h. g / (1 + g) comes from a table of
//     upstream's polynomial tan;
//   - upstream interpolates delay, damping compensation, pickup position,
//     damping and brightness linearly over the block and multiplies them per
//     sample. Here their products are interpolated instead: the loop delay
//     (delay x compensation - 1), the pickup delay (delay x position) and
//     the FIR's two taps (damping x h0, damping x h1). With dispersion, the
//     values derived from the dispersion amount (allpass gain, stretch,
//     noise and bridge amounts) are interpolated the same way. All exact
//     while the parameters hold still; while they move, the difference is
//     the product of two per-block steps;
//   - the first block after Init starts its interpolations at their
//     targets. Upstream's start from String::Init's defaults (compensation,
//     position and damping 0), harmless on Rings, whose strings are set up
//     long before a strum; but an OP-1 voice is reset and struck in the same
//     block, and a high note goes round its loop within it.
//
// Noise comes from upstream's generator (stmlib::Random: same LCG, same seed,
// drawn in the same order), so a host test can compare waveforms.

#ifndef OP1RINGS_STRING_FIXED_H_
#define OP1RINGS_STRING_FIXED_H_

#include <cstddef>
#include <cstdint>

#include "src/ffloat.h"

namespace op1rings {

// stmlib::Random's generator. Upstream's state is global and shared by
// every plucker and string, as here.
class RandomFixed {
 public:
  static void Seed(uint32_t seed) { state_ = seed; }
  static uint32_t GetWord() {
    state_ = state_ * 1664525u + 1013904223u;
    return state_;
  }
  // 2 * GetFloat() - 1, in Q31.
  static int32_t GetSigned() { return static_cast<int32_t>(GetWord() ^ 0x80000000u); }
  static uint32_t state() { return state_; }
  static void set_state(uint32_t state) { state_ = state; }

 private:
  static uint32_t state_;
};

const int32_t kStringDelaySize = 2048;    // upstream kDelayLineSize
const int32_t kStretchDelaySize = 1024;   // upstream kDelayLineSize / 2

// The dispersion allpass's delay line. Only strings with dispersion use one
// (MODEL_STRING, MODEL_STRING_AND_REVERB: one string per voice), so it lives
// outside StringFixed.
struct StretchLine {
  void Init();
  // The write pointer and the newest n samples (the rest is left as is).
  void CopyRecentFrom(const StretchLine& other, int32_t n);
  int32_t write_ptr;
  int32_t line[kStretchDelaySize];
};

class StringFixed {
 public:
  // Clears the delay line and the state (upstream String::Init).
  void Init();

  // Takes over another string's state and the part of its delay line it
  // can read in the next few milliseconds (1.5x its current delays, plus a
  // margin): what a voice fading out after a steal needs. Returns that
  // length in samples. Older samples are left as they were.
  int32_t CopyRecentFrom(const StringFixed& other);

  // Whether brightness or damping changed since the coefficients that
  // depend on them were computed (~2,000 cycles; a frequency change alone
  // costs a few hundred).
  bool needs_tone_update() const {
    return !tone_configured_ || brightness_ != tone_brightness_ || damping_ != tone_damping_;
  }

  // All Q31. frequency: f / sample rate. dispersion in [-1, 1]; the others
  // in [0, 1].
  void set_frequency(int32_t frequency) { frequency_ = frequency; }
  // Upstream's glide: frequency += coefficient * (frequency - current).
  void set_frequency(int32_t frequency, int32_t coefficient);
  void set_dispersion(int32_t dispersion) { dispersion_ = dispersion; }
  void set_brightness(int32_t brightness) { brightness_ = brightness; }
  void set_damping(int32_t damping) { damping_ = damping; }
  void set_position(int32_t position) { position_ = position; }

  // in: size Q25 excitation samples. out: size Q25 samples, to which the
  // string adds upstream's out - aux (the string minus its pickup, the mono
  // signal upstream's Part sends per voice). stretch: the dispersion
  // allpass's line, or null for a string without dispersion. size must be
  // a power of two. With update_tone false, the string keeps its
  // brightness- and damping-dependent coefficients even if those changed
  // (StringVoice spreads these updates); frequency changes always apply.
  void Process(const int32_t* in, int32_t* out, size_t size, StretchLine* stretch,
               bool update_tone = true);

 private:
  void ConfigureTone();
  void Configure();
  void ConfigureDispersion();
  template<bool kDispersion, bool kSrc>
  void ProcessInternal(const int32_t* in, int32_t* out, int32_t size, int32_t shift,
                       StretchLine* stretch);

  // Parameters (Q31).
  int32_t frequency_;
  int32_t dispersion_;
  int32_t brightness_;
  int32_t damping_;
  int32_t position_;

  // What depends on brightness and damping only (ConfigureTone), for the
  // last configured values: a glide only changes the frequency.
  bool tone_configured_;
  int32_t tone_brightness_;
  int32_t tone_damping_;
  FFloat decay_scale_;            // 120 / rt60 (semitones per sample)
  FFloat cutoff_ratio_;           // damping low-pass cutoff / f0
  int32_t infinite_;              // crossfade to infinite decay, Q31
  int32_t fir_brightness_;        // Q31, after that crossfade
  int32_t target_compensation_;   // Q31, 1 - lut_svf_shift(cutoff)
  int32_t noise_filter_;          // Q31
  int32_t noise_gain_;            // 1 / (0.2 + noise_filter), Q28

  // Coefficients for the last configured frequency (Configure).
  bool configured_;
  int32_t configured_frequency_;
  int32_t target_delay_;          // Q19
  int32_t src_ratio_;             // Q30, 1 << 30 when no resampling
  int32_t target_fir_h0_;         // damping (1 + brightness) / 2, Q31
  int32_t target_fir_h1_;         // damping (1 - brightness) / 4, Q31
  int32_t iir_coefficient_;       // g / (1 + g), Q31

  // Dispersion-derived values for the last configured dispersion (Q31).
  bool dispersion_configured_;
  int32_t configured_dispersion_;
  int32_t target_stretch_;
  int32_t target_noise_amount_;
  int32_t target_bridge_curving_;
  int32_t target_ac_blocking_;
  int32_t target_ap_gain_;

  // Interpolated values, where the last block ended. Until the first block
  // (primed_), they start at that block's targets.
  bool primed_;
  int32_t loop_delay_;            // Q19
  int32_t comb_delay_;            // Q19
  int32_t fir_h0_;                // Q31
  int32_t fir_h1_;                // Q31
  int32_t stretch_;
  int32_t noise_amount_;
  int32_t bridge_curving_;
  int32_t ac_blocking_;
  int32_t ap_gain_;

  // Signal state (Q25 unless noted).
  int32_t dispersion_noise_;      // Q28
  int32_t curved_bridge_;
  int32_t fir_x1_, fir_x2_;
  int32_t iir_s1_, iir_s2_;
  int32_t dc_x_, dc_y_;
  int32_t src_phase_;             // Q30
  int32_t out_sample_[2];
  int32_t aux_sample_[2];

  int32_t write_ptr_;
  // The samples, then guard words mirroring the first ones
  // (src/string_kernel.h).
  int32_t line_[kStringDelaySize + 4];
};

// Upstream Plucker: a noise burst through a comb, then a low-pass. Output in
// Q25.
class PluckerFixed {
 public:
  void Init();
  // frequency: f / sample rate; cutoff: low-pass cutoff / sample rate;
  // position: Rings' position. All Q31.
  void Trigger(int32_t frequency, int32_t cutoff, int32_t position);
  void Process(int32_t* out, size_t size);
  // Silent until the next Trigger (Process would output zeros).
  bool quiet() const { return quiet_ && !remaining_samples_; }

 private:
  int32_t remaining_samples_;
  int32_t comb_period_;          // samples, Q23 (< 255)
  int32_t comb_gain_;            // Q31
  int32_t svf_g_;                // Q29
  int32_t svf_k_;                // r + g, Q29
  int32_t svf_h_;                // Q31
  int32_t svf_s1_, svf_s2_;      // Q24
  bool quiet_;                   // silent until the next Trigger
  int32_t comb_write_ptr_;
  int32_t comb_[256];            // Q24
};

// stmlib::DCBlocker, Q25.
class DcBlockerFixed {
 public:
  void Init(int32_t pole) { pole_ = pole; x_ = y_ = 0; }
  void Process(int32_t* in_out, size_t size);
  // Zeros in would give zeros out.
  bool idle() const { return x_ == 0 && y_ == 0; }

 private:
  int32_t pole_;   // Q31
  int32_t x_, y_;
};

}  // namespace op1rings

#endif  // OP1RINGS_STRING_FIXED_H_
