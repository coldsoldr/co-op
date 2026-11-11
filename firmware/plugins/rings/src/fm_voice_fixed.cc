// Fixed-point version of Rings' FM voice. See fm_voice_fixed.h.
//
// Per sample, as upstream FMVoice::Process (rings/dsp/fm_voice.cc), in the
// same order. Formats: sine values Q31; previous_sample Q30 (the one-pole
// input c - p reaches +-2); fm_amount Q28 (up to 4 cycles of phase
// modulation); phase offsets are taken modulo 2^32, which is what upstream's
// (fm + 4) * 2^32 bias computes.

#include "src/fm_voice_fixed.h"

#include "src/ffloat.h"
#include "src/fixed.h"
#include "src/fixed_tables.h"

#ifndef OP1RINGS_SAMPLE_RATE
#define OP1RINGS_SAMPLE_RATE 44100
#endif

#include "src/rate_config.h"
static_assert(OP1RINGS_SAMPLE_RATE == 22050, "the literals are for 22.05 kHz (defloat.py)");

namespace op1rings {

namespace {

const int32_t kHalfQ31 = 1 << 30;
const int32_t k0_9Q31 = 1932735283;
const int32_t k0_1Q31 = 214748365;
const int32_t k0_005Q31 = 10737418;
const int32_t k0_045Q31 = 96636764;
const int32_t k0_015Q31 = 32212255;
const int32_t k0_005Q28 = 1342177;
const int32_t k7_8Q31 = 1879048192;
const int32_t k1_12Q31 = 178956971;     // 1 / 12

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

// Upstream: decay = 1 - 0.001^(1 / rt60) = 1 - e^-y with y = ln(1000) / rt60
// <= 0.0016, so y - y^2 / 2 is exact to ~1e-9. rt60 in samples.
inline int32_t DecayQ31(FFloat rt60) {
  static constexpr FFloat kLn1000 = FFloat::Raw(1854286438, -28) /* FFloat(6.907755278982137) */;
  FFloat y = kLn1000 * FFloat::Reciprocal(rt60);
  return (y - (y * y).Scaled(-1)).ToQ31();
}

// One sine cycle per 2^32 of phase, linear interpolation (upstream SineFm).
inline int32_t Sine(uint32_t phase) {
  const uint32_t i = phase >> 20;
  const int32_t frac = static_cast<int32_t>((phase & 0xfffff) << 11);   // Q31
  const int32_t a = kSineQ31[i];
  return a + MulQ31Fast(kSineQ31[i + 1] - a, frac);
}

}  // namespace

void FmVoiceFixed::Init() {
  carrier_frequency_ = FFloat::Raw(1371273005, -37) /* FFloat(220.0 / OP1RINGS_SAMPLE_RATE) */.ToQ31();
  ratio_ = kHalfQ31;
  brightness_ = kHalfQ31;
  damping_ = kHalfQ31;
  feedback_amount_ = 0;

  // As upstream: the interpolators start from these values.
  previous_carrier_increment_ = static_cast<uint32_t>(carrier_frequency_) << 1;
  previous_modulator_increment_ = previous_carrier_increment_;
  previous_brightness_ = brightness_;
  previous_feedback_ = 0;   // upstream starts it at feedback_amount_ (0)

  decays_valid_ = false;
  amplitude_envelope_ = 0;
  brightness_envelope_ = 0;
  gain_ = 0;
  fm_amount_ = 0;
  carrier_phase_ = 0;
  modulator_phase_ = 0;
  previous_sample_ = 0;
}

void FmVoiceFixed::UpdateDecays() {
  // rt60 = 0.1 s * 2^(damping * 8) (amplitude), 2^(damping * 7) (brightness).
  static constexpr FFloat kTenthSecond = FFloat::Raw(1156055040, -19) /* FFloat(0.1 * OP1RINGS_SAMPLE_RATE) */;
  amplitude_decay_ = DecayQ31(kTenthSecond * Exp2(damping_ >> 4));
  brightness_decay_ = DecayQ31(kTenthSecond * Exp2(MulQ31(damping_, k7_8Q31) >> 4));
  decays_damping_ = damping_;
  decays_valid_ = true;
}

void FmVoiceFixed::Process(int32_t* out, size_t size) {
  if (!decays_valid_ || damping_ != decays_damping_) UpdateDecays();

  // Between "oscillator" (no envelope, damping 1) and the struck FM sound.
  const int32_t envelope_amount = damping_ < k0_9Q31 ? kOne : (kOne - damping_) * 10;

  // Modulator frequency: carrier * 2^(ratio semitones / 12), at most 0.5.
  const int32_t index = ratio_ >> 24;                          // ratio * 128
  const int32_t frac = (ratio_ & 0xffffff) << 7;               // Q31
  // Floored to 1/256 semitone, as upstream's SemitonesToRatio (stmlib
  // units.h) looks it up: the modulator is tuned exactly like upstream's.
  const int32_t semitones = (kFmRatioSemitonesQ24[index] + MulQ31Fast(
      kFmRatioSemitonesQ24[index + 1] - kFmRatioSemitonesQ24[index], frac)) & ~0xffff;
  const FFloat modulator_frequency =
      FFloat::FromQ(carrier_frequency_, 31) * Exp2(MulQ31(semitones, k1_12Q31));
  static constexpr FFloat kHalf = FFloat::Raw(1073741824, -31) /* FFloat(0.5) */;
  const uint32_t modulator_increment = modulator_frequency >= kHalf
      ? 0x7fffffffu : static_cast<uint32_t>(modulator_frequency.ToQ31()) << 1;
  const uint32_t carrier_increment = static_cast<uint32_t>(carrier_frequency_) << 1;

  // feedback = (amount - 0.5) * 2, Q30 in [-1, 1].
  const int32_t feedback = feedback_amount_ - kHalfQ31;
  // Phase feedback (negative feedback) uses the block's value, as upstream.
  const int32_t phase_feedback = feedback < 0 ? MulQ31(feedback, feedback) << 1 : 0;   // Q31

  // Upstream's ParameterInterpolators: linear over the block, reaching the
  // target on the last sample.
  const int32_t n = static_cast<int32_t>(size);
  const int32_t carrier_step =
      static_cast<int32_t>(carrier_increment - previous_carrier_increment_) / n;
  const int32_t modulator_step =
      static_cast<int32_t>(modulator_increment - previous_modulator_increment_) / n;
  const int32_t brightness_step = (brightness_ - previous_brightness_) / n;
  const int32_t feedback_step = feedback / n - previous_feedback_ / n;

  uint32_t carrier_inc = previous_carrier_increment_;
  uint32_t modulator_inc = previous_modulator_increment_;
  int32_t brightness = previous_brightness_;
  int32_t feedback_value = previous_feedback_;

  int32_t amplitude_envelope = amplitude_envelope_;
  int32_t brightness_envelope = brightness_envelope_;
  int32_t fm_amount = fm_amount_;
  int32_t gain = gain_;
  uint32_t carrier_phase = carrier_phase_;
  uint32_t modulator_phase = modulator_phase_;
  int32_t previous_sample = previous_sample_;

  for (size_t i = 0; i < size; ++i) {
    // Envelopes: the follower's outputs are 0, so SLOPE only decays them.
    amplitude_envelope -= MulQ31Fast(amplitude_envelope, amplitude_decay_);
    brightness_envelope -= MulQ31Fast(brightness_envelope, brightness_decay_);

    // FM amount from brightness and the brightness envelope.
    brightness += brightness_step;
    const int32_t b2 = MulQ31Fast(brightness, brightness);         // Q31
    int32_t fm_min, fm_max;                                        // Q30
    if (b2 < kHalfQ31) {
      fm_min = 0;
      fm_max = b2;                                                 // 2 b2
    } else {
      fm_min = b2 - kHalfQ31;                                      // 2 b2 - 1
      fm_max = kHalfQ31;                                           // 1
    }
    const int32_t fm_envelope =
        kHalfQ31 + MulQ31Fast(envelope_amount, brightness_envelope - kHalfQ31);   // Q31
    // (fm_min + fm_max * fm_envelope) * 2 in Q28 is the same integer as the
    // sum in Q29.
    const int32_t fm_target = (fm_min >> 1) + (MulQ31Fast(fm_max, fm_envelope) >> 1);
    const int32_t slew = k0_005Q28 + (MulQ31Fast(fm_max, k0_015Q31) >> 2);   // Q28
    int32_t error = fm_target - fm_amount;
    if (error > slew) error = slew;
    if (error < -slew) error = -slew;
    fm_amount += error;

    // FM synthesis.
    carrier_inc += carrier_step;
    modulator_inc += modulator_step;
    const int32_t x = MulQ31Fast(previous_sample, phase_feedback) << 1;   // Q31
    modulator_phase += modulator_inc + (static_cast<uint32_t>(
        MulQ31Fast(static_cast<int32_t>(modulator_inc >> 1), x)) << 1);
    carrier_phase += carrier_inc;

    feedback_value += feedback_step;
    const int32_t modulator_fb = feedback_value > 0
        ? MulQ31(feedback_value, feedback_value) : 0;              // 0.25 fb^2, Q31
    const int32_t modulator = Sine(modulator_phase + (static_cast<uint32_t>(
        MulQ31Fast(modulator_fb, previous_sample)) << 2));
    const int32_t carrier = Sine(carrier_phase + (static_cast<uint32_t>(
        MulQ31Fast(fm_amount, modulator)) << 4));
    previous_sample += MulQ31Fast((carrier >> 1) - previous_sample, k0_1Q31);

    // Amplitude envelope.
    const int32_t gain_target = kOne + MulQ31Fast(envelope_amount, amplitude_envelope - kOne);
    const int32_t coefficient = k0_005Q31 + (MulQ31Fast(fm_amount, k0_045Q31) << 3);
    gain += MulQ31Fast(gain_target - gain, coefficient);

    out[i] = MulQ31Fast((carrier >> 1) + (modulator >> 1), gain) >> 3;   // Q30 -> Q27
  }

  previous_carrier_increment_ = carrier_increment;
  previous_modulator_increment_ = modulator_increment;
  previous_brightness_ = brightness_;
  previous_feedback_ = feedback;
  amplitude_envelope_ = amplitude_envelope;
  brightness_envelope_ = brightness_envelope;
  fm_amount_ = fm_amount;
  gain_ = gain;
  carrier_phase_ = carrier_phase;
  modulator_phase_ = modulator_phase;
  previous_sample_ = previous_sample;
}

}  // namespace op1rings
