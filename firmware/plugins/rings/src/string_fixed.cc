// Fixed-point string and plucker. See string_fixed.h.
//
// Per block, StringFixed recomputes its coefficients (upstream's
// String::ProcessInternal preamble) with FFloat, only when frequency,
// brightness or damping change. Per sample, it runs upstream's loop in fixed
// point.

#include "src/string_fixed.h"

#include <cstddef>

#include "src/ffloat.h"
#include "src/fixed.h"
#include "src/fixed_tables.h"
#include "src/profile.h"
#include "src/string_kernel.h"

#include "src/rate_config.h"

namespace op1rings {

uint32_t RandomFixed::state_ = 0x21;   // stmlib's seed

namespace {

const int32_t kOne = INT32_MAX;
const int32_t kHalf = 1 << 30;
const int32_t k1_12Q31 = 178956971;     // 1 / 12

// Compile-time constants are integer literals (coop: no float or double in a plugin file; defloat.py made them
// from upstream's Q(x, n) = x * 2^n rounded, at OP1RINGS_SAMPLE_RATE 22050).
static_assert(OP1RINGS_SAMPLE_RATE == 22050, "the literals are for 22.05 kHz (defloat.py)");

// 2^-l for l >= 0 in Q24; result in Q31 (as in op1_voice.cc).
inline int32_t Exp2NegQ31(int32_t l) {
  int32_t n = l >> 24;
  if (n >= 31) return 0;
  int32_t frac = l & 0xffffff;
  int32_t i = frac >> 16;
  int32_t f = (frac & 0xffff) << 15;
  int32_t m = kExp2NegQ31[i] + MulQ31Fast(kExp2NegQ31[i + 1] - kExp2NegQ31[i], f);
  return m >> n;
}

// 2^l for l in Q24 (any sign), as an FFloat (as in op1_voice.cc).
inline FFloat Exp2(int32_t l) {
  int32_t n = l >> 24;
  int32_t frac = l & 0xffffff;
  int32_t m = Exp2NegQ31((1 << 24) - frac);
  return FFloat::FromParts(m, n + 1 - 31);
}

// stmlib's SemitonesToRatio: its table lookup floors the semitones to 1/256.
// |semitones| < 128.
inline FFloat SemitonesToRatio(FFloat semitones) {
  const int32_t q8 = semitones.Scaled(8).Floor();         // 1/256 semitones
  return Exp2(MulQ31(q8 << 15, k1_12Q31) << 1);           // octaves, Q24
}

// x (2 - x) for x in [0, 1], Q31.
inline int32_t OneMinusSquaredComplement(int32_t x) {
  return ShlSat(x - (MulQ31(x, x) >> 1), 1);
}

inline int32_t Abs(int32_t x) { return x < 0 ? -x : x; }

inline int32_t Clamp(int32_t x, int32_t limit) {
  return x > limit ? limit : (x < -limit ? -limit : x);
}

// Hermite read at delay d (Q19) from a power-of-two line (upstream
// DelayLine::ReadHermite). Samples within +-7 (Q25) keep every term within
// +-64.
inline int32_t ReadHermite(const int32_t* line, int32_t mask, int32_t write_ptr, int32_t d) {
  const int32_t t = write_ptr + (d >> 19);
  const int32_t f = (d & 0x7ffff) << 12;                  // Q31
  const int32_t xm1 = line[(t - 1) & mask];
  const int32_t x0 = line[t & mask];
  const int32_t x1 = line[(t + 1) & mask];
  const int32_t x2 = line[(t + 2) & mask];
  const int32_t c = (x1 - xm1) >> 1;
  const int32_t v = x0 - x1;
  const int32_t w = c + v;
  const int32_t a = w + v + ((x2 - x0) >> 1);
  const int32_t b_neg = w + a;
  int32_t r = MulQ31Fast(a, f) - b_neg;
  r = MulQ31Fast(r, f) + c;
  return MulQ31Fast(r, f) + x0;
}

// Linear read at delay d (Q19) (upstream DelayLine::Read(float)).
inline int32_t ReadLinear(const int32_t* line, int32_t mask, int32_t write_ptr, int32_t d) {
  const int32_t t = write_ptr + (d >> 19);
  const int32_t a = line[t & mask];
  const int32_t b = line[(t + 1) & mask];
  return a + MulQ31Fast(b - a, (d & 0x7ffff) << 12);
}

}  // namespace

// --- StretchLine ------------------------------------------------------------

void StretchLine::Init() {
  write_ptr = 0;
  for (int32_t i = 0; i < kStretchDelaySize; ++i) line[i] = 0;
}

void StretchLine::CopyRecentFrom(const StretchLine& other, int32_t n) {
  // The newest sample is at write_ptr + 1 (the line is written downwards).
  write_ptr = other.write_ptr;
  if (n > kStretchDelaySize) n = kStretchDelaySize;
  for (int32_t i = 1; i <= n; ++i) {
    const int32_t j = (write_ptr + i) & (kStretchDelaySize - 1);
    line[j] = other.line[j];
  }
}

// --- StringFixed ------------------------------------------------------------

void StringFixed::Init() {
  for (int32_t i = 0; i < kStringDelaySize + kStringKernelGuard; ++i) line_[i] = 0;
  write_ptr_ = 0;

  // Upstream String::Init's defaults.
  frequency_ = 21426141 /* Q(220.0 / OP1RINGS_SAMPLE_RATE, 31) */;
  dispersion_ = 536870912 /* Q(0.25, 31) */;
  brightness_ = kHalf;
  damping_ = 644245094 /* Q(0.3, 31) */;
  position_ = 1717986918 /* Q(0.8, 31) */;

  tone_configured_ = false;
  configured_ = false;
  dispersion_configured_ = false;
  primed_ = false;

  // Upstream's interpolators start from compensation 0, position 0 and
  // damping 0: a loop delay of -1 and silent taps (reading the empty line).
  loop_delay_ = -(1 << 19);
  comb_delay_ = 0;
  fir_h0_ = 0;
  fir_h1_ = 0;
  // Upstream's previous_dispersion_ starts at 0: the derived values of 0.
  stretch_ = 0;
  noise_amount_ = 0;
  bridge_curving_ = 0;
  ac_blocking_ = 0;
  ap_gain_ = 0;

  dispersion_noise_ = 0;
  curved_bridge_ = 0;
  fir_x1_ = fir_x2_ = 0;
  iir_s1_ = iir_s2_ = 0;
  dc_x_ = dc_y_ = 0;
  src_phase_ = 0;
  out_sample_[0] = out_sample_[1] = 0;
  aux_sample_[0] = aux_sample_[1] = 0;
}

int32_t StringFixed::CopyRecentFrom(const StringFixed& other) {
  // Everything before line_ is plain state (copied byte by byte, which may
  // alias anything: no libc in a coop plugin; only on a voice steal).
  {
    unsigned char* d = reinterpret_cast<unsigned char*>(this);
    const unsigned char* s = reinterpret_cast<const unsigned char*>(&other);
    for (uint32_t i = 0; i < offsetof(StringFixed, line_); ++i) d[i] = s[i];
  }
  int32_t reach = other.loop_delay_ > other.comb_delay_ ? other.loop_delay_ : other.comb_delay_;
  if (reach < 0) reach = 0;
  reach >>= 19;
  reach += (reach >> 1) + 16;
  if (reach > kStringDelaySize) reach = kStringDelaySize;
  for (int32_t i = 1; i <= reach; ++i) {
    const int32_t j = (write_ptr_ + i) & (kStringDelaySize - 1);
    line_[j] = other.line_[j];
  }
  for (int32_t i = 0; i < kStringKernelGuard; ++i) line_[kStringDelaySize + i] = line_[i];
  return reach;
}

void StringFixed::set_frequency(int32_t frequency, int32_t coefficient) {
  const int32_t error = frequency - frequency_;
  // Upstream's glide converges forever; end it within ~0.1 cent (2^-14),
  // so the string stops recomputing its coefficients.
  const int32_t threshold = frequency >> 14;
  if (error <= threshold && error >= -threshold) {
    frequency_ = frequency;
  } else {
    frequency_ += MulQ31(coefficient, error);
  }
}

// Upstream String::ProcessInternal's preamble, the parts that depend on
// brightness and damping only.
void StringFixed::ConfigureTone() {
  static constexpr FFloat kRt60Scale = FFloat::Raw(1618477056, -20) /* FFloat(0.07 * OP1RINGS_SAMPLE_RATE) */;
  static constexpr FFloat k96 = FFloat::Raw(1610612736, -24) /* FFloat(96.0) */;
  static constexpr FFloat k120 = FFloat::Raw(2013265920, -24) /* FFloat(120.0) */;
  static constexpr FFloat k48 = FFloat::Raw(1610612736, -25) /* FFloat(48.0) */;
  static constexpr FFloat k0_2 = FFloat::Raw(1717986918, -33) /* FFloat(0.2) */;
  constexpr int32_t k0_95 = 2040109466 /* Q(0.95, 31) */;

  const int32_t b = brightness_;
  const int32_t d = damping_;
  tone_configured_ = true;
  tone_brightness_ = b;
  tone_damping_ = d;

  // Decay: rt60 = 0.07 s * 2^(8 lf_damping); 120 / rt60 semitones per
  // sample of delay.
  const int32_t lf_damping = OneMinusSquaredComplement(d);
  const FFloat rt60 = kRt60Scale * SemitonesToRatio(FFloat::FromQ(lf_damping, 31) * k96);
  decay_scale_ = k120 * FFloat::Reciprocal(rt60);

  int32_t brightness = MulQ31(b, b);
  // A per-sample one-pole coefficient (upstream string.cc: dispersion_noise
  // += noise_filter x (noise - dispersion_noise)), with its level
  // compensation below. Not scaled with the sample rate: the pair sets the
  // noise's level as much as its colour (scaling it made the strings 2 dB
  // quieter at 22.05 kHz), so the exciter stays sample-identical, as it is
  // at 44.1 kHz against upstream's 48 kHz.
  const FFloat noise_filter = SemitonesToRatio(FFloat::FromQ(b - kOne, 31) * k48);
  noise_filter_ = noise_filter.ToQ31();
  noise_gain_ = FFloat::Reciprocal(k0_2 + noise_filter).ToQ(28);

  // Damping low-pass cutoff, in semitones above f0 (Q23).
  int32_t cutoff = (24 << 23) + MulQ31(MulQ31(d, d), 48 << 23) + MulQ31(brightness, 24 << 23);
  if (cutoff > (84 << 23)) cutoff = 84 << 23;
  cutoff_ratio_ = SemitonesToRatio(FFloat::FromQ(cutoff, 23));

  // Crossfade to infinite decay (the frequency-dependent half is in
  // Configure).
  infinite_ = 0;
  if (d >= k0_95) {
    infinite_ = d - k0_95 >= 107374182 /* Q(0.05, 31) */ ? kOne : (d - k0_95) * 20;
    brightness += MulQ31(infinite_, kOne - brightness);
    cutoff += MulQ31(infinite_, (128 << 23) - cutoff);
  }
#if OP1RINGS_SAMPLE_RATE == 22050
  // The loop's brightness FIR (h1, h0, h1 below) loses (1 - b) / 2 x
  // (1 - cos w) per pass at w = 2 pi f / fs: at half the rate, four times
  // the loss at a given Hz. b' = (3 + b) / 4 matches the loss at low
  // frequencies. Measured against the 44.1 kHz build (simulator, the
  // string + reverb scenario): without it 2.8 dB quieter and 4 dB duller
  // in 2-5 kHz, with it within 0.4 dB in every band below 9 kHz. What it
  // can't match: above ~3.5 kHz the half-rate FIR loses less than the
  // formula, so the highest partials of a dark string ring a little longer
  // (seen at -55 dB relative in the plain string scenario).
  fir_brightness_ = brightness + MulQ31(kOne - brightness, 1610612736 /* Q(0.75, 31) */);
#elif OP1RINGS_SAMPLE_RATE != 44100
#error "string FIR: add the rate's remap"
#else
  fir_brightness_ = brightness;
#endif

  // The low-pass delays the loop by lut_svf_shift(cutoff) periods.
  const int32_t index = cutoff >> 23;
  const int32_t frac = (cutoff & 0x7fffff) << 8;
  const int32_t shift = kSvfShiftQ31[index] + MulQ31(kSvfShiftQ31[index + 1] - kSvfShiftQ31[index], frac);
  target_compensation_ = kOne - shift;
}

// The rest of the preamble: what depends on the frequency.
void StringFixed::Configure() {
  static constexpr FFloat kOneF = FFloat::Raw(1073741824, -30) /* FFloat(1.0) */;
  static constexpr FFloat kMinDelay = FFloat::Raw(1073741824, -28) /* FFloat(4.0) */;
  static constexpr FFloat kMaxDelay = FFloat::Raw(2143289344, -20);   /* FFloat(kStringDelaySize - 4.0), kStringDelaySize = 2048 */
  static_assert(kStringDelaySize == 2048, "kMaxDelay");
  static constexpr FFloat kSrcThreshold = FFloat::Raw(2147268899, -31) /* FFloat(0.9999) */;
  static constexpr FFloat k127 = FFloat::Raw(2130706432, -24) /* FFloat(127.0) */;
  static constexpr FFloat k0_499 = FFloat::Raw(2143188680, -32) /* FFloat(0.499) */;
  static constexpr FFloat k0_4999 = FFloat::Raw(2147054151, -32) /* FFloat(0.4999) */;

  configured_ = true;
  configured_frequency_ = frequency_;
  const FFloat f = FFloat::FromQ(frequency_, 31);

  // delay = 1 / f in [4, 2044]; below ~21.6 Hz (at 44.1 kHz) the string
  // plays at that pitch and is resampled.
  FFloat delay = f.mantissa() > 0 ? FFloat::Reciprocal(f) : kMaxDelay;
  if (delay > kMaxDelay) delay = kMaxDelay;
  if (delay < kMinDelay) delay = kMinDelay;
  FFloat src_ratio = delay * f;
  if (src_ratio >= kSrcThreshold) src_ratio = kOneF;
  target_delay_ = delay.ToQ(19);
  src_ratio_ = src_ratio.ToQ(30);

  // Damping per period: 2^(-120 delay / rt60 / 12), at most 127 semitones.
  FFloat decay_semitones = decay_scale_ * delay;
  if (src_ratio != kOneF) decay_semitones = decay_semitones * FFloat::Reciprocal(src_ratio);
  if (decay_semitones > k127) decay_semitones = k127;
  int32_t damping_coefficient = SemitonesToRatio(-decay_semitones).ToQ31();
  FFloat damping_f = f * cutoff_ratio_;
  if (damping_f > k0_499) damping_f = k0_499;
  if (infinite_) {
    damping_coefficient += MulQ31(infinite_, kOne - damping_coefficient);
    damping_f += FFloat::FromQ(infinite_, 31) * (k0_4999 - damping_f);
  }
  target_fir_h0_ = MulQ31(damping_coefficient, (fir_brightness_ >> 1) + kHalf);
  target_fir_h1_ = MulQ31(damping_coefficient, (kOne - fir_brightness_) >> 2);

  // The low-pass (resonance 0.5) as two one-poles: G = g / (1 + g), g from
  // stmlib's tan<FREQUENCY_ACCURATE>, tabulated (damping_f <= 0.4999).
  const int32_t x = damping_f.ToQ31();
  const int32_t i = x >> 20;
  const int32_t frac = (x & 0xfffff) << 11;
  iir_coefficient_ = kStringLowpassQ31[i] + MulQ31(kStringLowpassQ31[i + 1] - kStringLowpassQ31[i], frac);
}

void StringFixed::ConfigureDispersion() {
  static constexpr FFloat kApGain = FFloat::Raw(-1327144894, -31) /* FFloat(-0.618) */;
  static constexpr FFloat k0_15 = FFloat::Raw(1288490188, -33) /* FFloat(0.15) */;
  const int32_t d = dispersion_ < -kOne ? -kOne : dispersion_;
  dispersion_configured_ = true;
  configured_dispersion_ = dispersion_;

  target_stretch_ = d <= 0 ? 0 : MulQ31(OneMinusSquaredComplement(d), 1020054733 /* Q(0.475, 31) */);
  if (d > 1610612736 /* Q(0.75, 31) */) {
    const int32_t n = ShlSat(d - 1610612736 /* Q(0.75, 31) */, 2);
    target_noise_amount_ = MulQ31(MulQ31(n, n), 53687091 /* Q(0.025, 31) */);
  } else {
    target_noise_amount_ = 0;
  }
  const int32_t bridge = d < 0 ? -d : 0;
  target_ac_blocking_ = bridge;
  target_bridge_curving_ = MulQ31(MulQ31(bridge, bridge), 21474836 /* Q(0.01, 31) */);
  const FFloat x = FFloat::FromQ(d, 31);
  target_ap_gain_ = (kApGain * x * FFloat::Reciprocal(k0_15 + FFloat::Abs(x))).ToQ31();
}

void StringFixed::Prepare(bool update_tone, bool stretch) {
  if (!tone_configured_ || (update_tone && needs_tone_update())) {
    ConfigureTone();
    Configure();
  } else if (!configured_ || frequency_ != configured_frequency_) {
    Configure();
  }
  if (stretch && (!dispersion_configured_ || dispersion_ != configured_dispersion_)) ConfigureDispersion();
}

void StringFixed::Process(const int32_t* in, int32_t* out, size_t size, StretchLine* stretch,
                          bool update_tone) {
  PROF_START(t_config);
  if (!tone_configured_ || (update_tone && needs_tone_update())) {
    ConfigureTone();
    Configure();
  } else if (!configured_ || frequency_ != configured_frequency_) {
    Configure();
  }
  PROF_ADD(PROF_STRING_CONFIG, t_config);
  const int32_t n = static_cast<int32_t>(size);
  int32_t shift = 0;
  while ((1 << shift) < n) ++shift;
  const bool src = src_ratio_ != kHalf;
  if (stretch) {
    if (!dispersion_configured_ || dispersion_ != configured_dispersion_) ConfigureDispersion();
    if (src) {
      ProcessInternal<true, true>(in, out, n, shift, stretch);
    } else {
      ProcessInternal<true, false>(in, out, n, shift, stretch);
    }
  } else {
    if (src) {
      ProcessInternal<false, true>(in, out, n, shift, nullptr);
    } else {
      ProcessInternal<false, false>(in, out, n, shift, nullptr);
    }
  }
}

namespace {

// The FIR and IIR damping filters (upstream DampingFilter, then the
// low-pass as two TPT one-poles), and the clamp before the delay line.
inline __attribute__((always_inline)) int32_t Damp(
    int32_t s, int32_t h0, int32_t h1, int32_t g,
    int32_t& x1, int32_t& x2, int32_t& s1, int32_t& s2) {
  const int32_t kWriteLimit = 7 << 25;
  const int32_t y = MulQ31Fast(h0, x1) + MulQ31Fast(h1, s + x2);
  x2 = x1;
  x1 = s;
  const int32_t v1 = MulQ31Fast(y - s1, g);
  const int32_t lp1 = v1 + s1;
  s1 = lp1 + v1;
  const int32_t v2 = MulQ31Fast(lp1 - s2, g);
  const int32_t lp = Clamp(v2 + s2, kWriteLimit);
  s2 = lp + v2;
  return lp;
}

}  // namespace

template<bool kDispersion, bool kSrc>
void StringFixed::ProcessInternal(const int32_t* in, int32_t* out, int32_t size, int32_t shift,
                                  StretchLine* stretch) {
  const int32_t kMask = kStringDelaySize - 1;
  const int32_t kStretchMask = kStretchDelaySize - 1;
  const int32_t kBridgeThreshold = 838861 /* Q(0.025, 25) */;
  const int32_t kDcPole = 2145535817 /* Q(1.0 - 20.0 / OP1RINGS_SAMPLE_RATE, 31) */;
  const int32_t kMinApDelay = 4 << 19;

  // Pickup position: 0.5 - 0.98 |position - 0.5|.
  const int32_t clamped_position = kHalf - MulQ31(Abs(position_ - kHalf), 2104533975 /* Q(0.98, 31) */);
  // The loop delay (minus the FIR's sample) and the pickup delay.
  const int32_t target_loop_delay = MulQ31(target_delay_, target_compensation_) - (1 << 19);
  const int32_t target_comb_delay = MulQ31(target_delay_, clamped_position);

  if (!primed_) {
    primed_ = true;
    loop_delay_ = target_loop_delay;
    comb_delay_ = target_comb_delay;
    fir_h0_ = target_fir_h0_;
    fir_h1_ = target_fir_h1_;
    if (kDispersion) {
      stretch_ = target_stretch_;
      noise_amount_ = target_noise_amount_;
      bridge_curving_ = target_bridge_curving_;
      ac_blocking_ = target_ac_blocking_;
      ap_gain_ = target_ap_gain_;
    }
  }

  // Linear over the block (power-of-two size, so a shift divides).
  int32_t loop_delay = loop_delay_;
  int32_t comb_delay = comb_delay_;
  int32_t h0 = fir_h0_;
  int32_t h1 = fir_h1_;
  const int32_t loop_delay_step = (target_loop_delay - loop_delay) >> shift;
  const int32_t comb_delay_step = (target_comb_delay - comb_delay) >> shift;
  const int32_t h0_step = (target_fir_h0_ - h0) >> shift;
  const int32_t h1_step = (target_fir_h1_ - h1) >> shift;

  const int32_t g = iir_coefficient_;
  int32_t fir_x1 = fir_x1_, fir_x2 = fir_x2_;
  int32_t iir_s1 = iir_s1_, iir_s2 = iir_s2_;
  int32_t* line = line_;
  int32_t write_ptr = write_ptr_;

  if (!kDispersion && !kSrc) {
    // The common case: src/string_kernel.h (assembly on Blackfin).
    StringKernel k;
    k.line = line;
    k.write_ptr = write_ptr;
    k.loop_delay = loop_delay;
    k.loop_delay_step = loop_delay_step;
    k.comb_delay = comb_delay;
    k.comb_delay_step = comb_delay_step;
    k.h0 = h0;
    k.h0_step = h0_step;
    k.h1 = h1;
    k.h1_step = h1_step;
    k.g = g;
    k.x1 = fir_x1;
    k.x2 = fir_x2;
    k.s1 = iir_s1;
    k.s2 = iir_s2;
    StringKernelProcess(&k, in, out, size);
    loop_delay_ = target_loop_delay;
    comb_delay_ = target_comb_delay;
    fir_h0_ = target_fir_h0_;
    fir_h1_ = target_fir_h1_;
    fir_x1_ = k.x1;
    fir_x2_ = k.x2;
    iir_s1_ = k.s1;
    iir_s2_ = k.s2;
    write_ptr_ = k.write_ptr;
    src_phase_ = kHalf;   // upstream: src_phase_ = 1 at full rate
    return;
  }

  int32_t stretch_point = stretch_;
  int32_t noise_amount = noise_amount_;
  int32_t bridge_curving = bridge_curving_;
  int32_t ac_blocking = ac_blocking_;
  int32_t ap_gain = ap_gain_;
  int32_t stretch_step = 0, noise_amount_step = 0, bridge_curving_step = 0;
  int32_t ac_blocking_step = 0, ap_gain_step = 0;
  if (kDispersion) {
    stretch_step = (target_stretch_ - stretch_point) >> shift;
    noise_amount_step = (target_noise_amount_ - noise_amount) >> shift;
    bridge_curving_step = (target_bridge_curving_ - bridge_curving) >> shift;
    ac_blocking_step = (target_ac_blocking_ - ac_blocking) >> shift;
    ap_gain_step = (target_ap_gain_ - ap_gain) >> shift;
  }

  if (kDispersion && !kSrc && !noise_amount && !target_noise_amount_ && !bridge_curving &&
      !target_bridge_curving_ && !ac_blocking && !target_ac_blocking_) {
    // Only the stretch allpass is active (dispersion in [0, 0.75]): the
    // lean loop (src/string_kernel.h). The noise and the DC-blocked copy
    // would be multiplied by 0; their states are left as they are, and the
    // noise generator still advances one draw per sample, as upstream's.
    StringKernel k;
    k.line = line;
    k.write_ptr = write_ptr;
    k.loop_delay = loop_delay;
    k.loop_delay_step = loop_delay_step;
    k.comb_delay = comb_delay;
    k.comb_delay_step = comb_delay_step;
    k.h0 = h0;
    k.h0_step = h0_step;
    k.h1 = h1;
    k.h1_step = h1_step;
    k.g = g;
    k.x1 = fir_x1;
    k.x2 = fir_x2;
    k.s1 = iir_s1;
    k.s2 = iir_s2;
    k.stretch_line = stretch->line;
    k.stretch_ptr = stretch->write_ptr;
    k.stretch = stretch_point;
    k.stretch_step = stretch_step;
    k.ap_gain = ap_gain;
    k.ap_gain_step = ap_gain_step;
    StringKernelStretchProcess(&k, in, out, size);
    for (int32_t i = 0; i < size; ++i) RandomFixed::GetWord();
    loop_delay_ = target_loop_delay;
    comb_delay_ = target_comb_delay;
    fir_h0_ = target_fir_h0_;
    fir_h1_ = target_fir_h1_;
    stretch_ = target_stretch_;
    ap_gain_ = target_ap_gain_;
    fir_x1_ = k.x1;
    fir_x2_ = k.x2;
    iir_s1_ = k.s1;
    iir_s2_ = k.s2;
    write_ptr_ = k.write_ptr;
    stretch->write_ptr = k.stretch_ptr;
    src_phase_ = kHalf;   // upstream: src_phase_ = 1 at full rate
    return;
  }

  const int32_t noise_filter = noise_filter_;
  const int32_t noise_gain = noise_gain_;
  int32_t dispersion_noise = dispersion_noise_;
  int32_t curved_bridge = curved_bridge_;
  int32_t dc_x = dc_x_, dc_y = dc_y_;
  int32_t* stretch_line = kDispersion ? stretch->line : nullptr;
  int32_t stretch_write_ptr = kDispersion ? stretch->write_ptr : 0;

  // One string sample: returns the written sample, and the pickup in *aux.
  auto tick = [&](int32_t input, int32_t* aux) __attribute__((always_inline)) -> int32_t {
    loop_delay += loop_delay_step;
    comb_delay += comb_delay_step;
    int32_t d = loop_delay;

    int32_t s;
    if (kDispersion) {
      const int32_t noise = MulQ31Fast(RandomFixed::GetSigned(), noise_gain);   // Q28
      dispersion_noise += MulQ31Fast(noise - dispersion_noise, noise_filter);

      stretch_point += stretch_step;
      noise_amount += noise_amount_step;
      bridge_curving += bridge_curving_step;
      ac_blocking += ac_blocking_step;
      ap_gain += ap_gain_step;

      const int32_t delay_fm = (1 << 30) + (MulQ31Fast(dispersion_noise, noise_amount) << 2) -
                               (MulQ31Fast(curved_bridge, bridge_curving) << 5);   // Q30
      d = MulQ31Fast(d, delay_fm) << 1;
      const int32_t ap_delay = MulQ31Fast(d, stretch_point);
      const int32_t main_delay = d - ap_delay;
      if (ap_delay >= kMinApDelay && main_delay >= kMinApDelay) {
        s = ReadHermite(line, kMask, write_ptr, main_delay);
        // stretch_.Allpass(s, (size_t) ap_delay, ap_gain).
        const int32_t read = stretch_line[(stretch_write_ptr + (ap_delay >> 19)) & kStretchMask];
        const int32_t write = s + MulQ31Fast(read, ap_gain);
        stretch_line[stretch_write_ptr] = write;
        stretch_write_ptr = (stretch_write_ptr - 1) & kStretchMask;
        s = read - MulQ31Fast(write, ap_gain);
      } else {
        s = ReadHermite(line, kMask, write_ptr, d);
      }
      // DC-blocked copy, mixed in by ac_blocking (negative dispersion).
      dc_y = MulQ31Fast(dc_y, kDcPole) + s - dc_x;
      dc_x = s;
      s += MulQ31Fast(dc_y - s, ac_blocking);

      const int32_t value = Abs(s) - kBridgeThreshold;
      curved_bridge = value <= 0 ? 0 : (s > 0 ? value << 1 : -3 * value);
    } else {
      s = ReadHermite(line, kMask, write_ptr, d);
    }

    s = Damp(s + input, h0, h1, g, fir_x1, fir_x2, iir_s1, iir_s2);
    h0 += h0_step;
    h1 += h1_step;
    StringKernelWrite(line, write_ptr, s);
    write_ptr = (write_ptr - 1) & kMask;
    *aux = ReadLinear(line, kMask, write_ptr, comb_delay);
    return s;
  };

  if (!kSrc) {
    for (int32_t i = 0; i < size; ++i) {
      int32_t aux;
      const int32_t s = tick(in[i], &aux);
      out[i] += s - aux;
    }
    loop_delay_ = target_loop_delay;
    comb_delay_ = target_comb_delay;
    fir_h0_ = target_fir_h0_;
    fir_h1_ = target_fir_h1_;
    if (kDispersion) {
      stretch_ = target_stretch_;
      noise_amount_ = target_noise_amount_;
      bridge_curving_ = target_bridge_curving_;
      ac_blocking_ = target_ac_blocking_;
      ap_gain_ = target_ap_gain_;
    }
    src_phase_ = kHalf;   // upstream: src_phase_ = 1 at full rate
  } else {
    // Below the delay line's lowest pitch: the string runs at src_ratio of
    // the sample rate, linearly interpolated (upstream's "very crappy"
    // upsampler). The interpolators only advance on string samples, so they
    // keep where they got to.
    const int32_t src_ratio = src_ratio_;
    int32_t src_phase = src_phase_;
    for (int32_t i = 0; i < size; ++i) {
      src_phase += src_ratio;
      if (src_phase > kHalf) {
        src_phase -= kHalf;
        int32_t aux;
        const int32_t s = tick(in[i], &aux);
        out_sample_[1] = out_sample_[0];
        aux_sample_[1] = aux_sample_[0];
        out_sample_[0] = s;
        aux_sample_[0] = aux;
      }
      const int32_t o = out_sample_[1] + (MulQ31Fast(out_sample_[0] - out_sample_[1], src_phase) << 1);
      const int32_t a = aux_sample_[1] + (MulQ31Fast(aux_sample_[0] - aux_sample_[1], src_phase) << 1);
      out[i] += o - a;
    }
    src_phase_ = src_phase;
    loop_delay_ = loop_delay;
    comb_delay_ = comb_delay;
    fir_h0_ = h0;
    fir_h1_ = h1;
    if (kDispersion) {
      stretch_ = stretch_point;
      noise_amount_ = noise_amount;
      bridge_curving_ = bridge_curving;
      ac_blocking_ = ac_blocking;
      ap_gain_ = ap_gain;
    }
  }

  dispersion_noise_ = dispersion_noise;
  curved_bridge_ = curved_bridge;
  fir_x1_ = fir_x1;
  fir_x2_ = fir_x2;
  iir_s1_ = iir_s1;
  iir_s2_ = iir_s2;
  dc_x_ = dc_x;
  dc_y_ = dc_y;
  write_ptr_ = write_ptr;
  if (kDispersion) stretch->write_ptr = stretch_write_ptr;
}

// --- PluckerFixed -----------------------------------------------------------

void PluckerFixed::Init() {
  remaining_samples_ = 0;
  comb_period_ = 0;
  comb_gain_ = 0;
  // stmlib Svf::Init: set_f_q<FREQUENCY_DIRTY>(0.01, 100).
  svf_g_ = 16871705 /* Q(0.031426, 29) */;
  svf_k_ = 22240414 /* Q(0.01 + 0.031426, 29) */;
  svf_h_ = 2144691574 /* Q(1.0 / (1.0 + 0.01 * 0.031426 + 0.031426 * 0.031426), 31) */;
  svf_s1_ = svf_s2_ = 0;
  comb_write_ptr_ = 0;
  for (int32_t i = 0; i < 256; ++i) comb_[i] = 0;
  quiet_ = true;
}

void PluckerFixed::Trigger(int32_t frequency, int32_t cutoff, int32_t position) {
  static constexpr FFloat k255 = FFloat::Raw(2139095040, -23) /* FFloat(255.0) */;
  static constexpr FFloat kOneF = FFloat::Raw(1073741824, -30) /* FFloat(1.0) */;
  // stmlib OnePole::tan<FREQUENCY_DIRTY>: f (pi + a f^2), a = 0.3736 pi^3.
  static constexpr FFloat kPi = FFloat::Raw(1686629713, -29) /* FFloat(3.14159265358979) */;
  static constexpr FFloat kA = FFloat::Raw(1554770774, -27) /* FFloat(0.3736 * 3.14159265358979 * 3.14159265358979 * 3.14159265358979) */;
  const int32_t k0_499 = 1071594340 /* Q(0.499, 31) */;

  const int32_t ratio = MulQ31(position, 1932735283 /* Q(0.9, 31) */) + 107374182 /* Q(0.05, 31) */;
  FFloat comb_period = frequency > 0
      ? FFloat::FromQ(ratio, 31) * FFloat::Reciprocal(FFloat::FromQ(frequency, 31))
      : FFloat::FromInt(1 << 20);
  remaining_samples_ = comb_period.Floor();
  while (comb_period >= k255) comb_period = comb_period.Scaled(-1);
  comb_period_ = comb_period.ToQ(23);
  comb_gain_ = MulQ31(kOne - position, 1717986918 /* Q(0.8, 31) */);

  // Low-pass, resonance 1 (r = 1).
  const FFloat f = FFloat::FromQ(cutoff < k0_499 ? cutoff : k0_499, 31);
  const FFloat g = f * (kPi + kA * f * f);
  svf_g_ = g.ToQ(29);
  svf_k_ = (kOneF + g).ToQ(29);
  svf_h_ = FFloat::Reciprocal(kOneF + g + g * g).ToQ31();
  quiet_ = false;
}

void PluckerFixed::Process(int32_t* out, size_t size) {
  // Once the burst is over and the comb and filter have decayed below ~1e-6
  // (2^-20, in Q24), the plucker is silent until the next strum: its state
  // is cleared and it costs nothing.
  if (!remaining_samples_ && quiet_) {
    for (size_t i = 0; i < size; ++i) out[i] = 0;
    return;
  }
  const int32_t gain = comb_gain_;
  const int32_t period = comb_period_;
  const int32_t index = period >> 23;
  const int32_t frac = (period & 0x7fffff) << 8;
  int32_t write_ptr = comb_write_ptr_;
  for (size_t i = 0; i < size; ++i) {
    int32_t in = 0;
    if (remaining_samples_) {
      in = RandomFixed::GetSigned() >> 7;                  // Q24
      --remaining_samples_;
    }
    const int32_t t = write_ptr + index;
    const int32_t a = comb_[t & 255];
    const int32_t b = comb_[(t + 1) & 255];
    const int32_t y = in + MulQ31Fast(gain, a + MulQ31Fast(b - a, frac));
    comb_[write_ptr] = y;
    write_ptr = (write_ptr - 1) & 255;
    out[i] = y;
  }
  comb_write_ptr_ = write_ptr;

  // TPT state-variable low-pass, Q24 states.
  const int32_t g = svf_g_, k = svf_k_, h = svf_h_;
  int32_t s1 = svf_s1_, s2 = svf_s2_;
  for (size_t i = 0; i < size; ++i) {
    const int32_t hp = MulQ31Fast(out[i] - (MulQ31Fast(s1, k) << 2) - s2, h);
    const int32_t g_hp = MulQ31Fast(hp, g) << 2;
    const int32_t bp = g_hp + s1;
    s1 = g_hp + bp;
    const int32_t g_bp = MulQ31Fast(bp, g) << 2;
    const int32_t lp = g_bp + s2;
    s2 = g_bp + lp;
    out[i] = lp << 1;                                      // Q25
  }
  svf_s1_ = s1;
  svf_s2_ = s2;

  if (!remaining_samples_) {
    const int32_t kQuiet = 1 << 4;
    bool quiet = Abs(s1) < kQuiet && Abs(s2) < kQuiet;
    for (int32_t i = 0; quiet && i < 256; ++i) quiet = Abs(comb_[i]) < kQuiet;
    if (quiet) {
      svf_s1_ = svf_s2_ = 0;
      for (int32_t i = 0; i < 256; ++i) comb_[i] = 0;
      quiet_ = true;
    }
  }
}

// --- DcBlockerFixed ---------------------------------------------------------

void DcBlockerFixed::Process(int32_t* in_out, size_t size) {
  // In fixed point a DC blocker left alone settles on a small constant
  // instead of 0, and a constant input would build up in a string with no
  // decay; so once input and output are quiet, it's cleared.
  const int32_t kQuiet = 1 << 12;   // ~1.2e-4 in Q25
  if (x_ == 0 && y_ == 0 && in_out[0] == 0 && in_out[size - 1] == 0) {
    bool silent = true;
    for (size_t i = 1; silent && i < size - 1; ++i) silent = in_out[i] == 0;
    if (silent) return;   // zeros in, zeros out
  }
  int32_t x = x_, y = y_;
  const int32_t pole = pole_;
  for (size_t i = 0; i < size; ++i) {
    const int32_t old_x = x;
    x = in_out[i];
    y = MulQ31Fast(y, pole) + x - old_x;
    in_out[i] = y;
  }
  if (x == 0 && Abs(y) < kQuiet) y = 0;
  x_ = x;
  y_ = y;
}

}  // namespace op1rings
