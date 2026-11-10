// Fixed-point version of Rings' FM voice (rings/dsp/fm_voice.cc), for the
// OP-1 build. Two sine operators: a modulator at a quantized ratio of the
// carrier, with feedback, and an internal envelope triggered on each strum.
//
// Upstream also runs an envelope follower on the audio input. With the
// internal exciter (the only one on the OP-1) that input is silent, so the
// follower's outputs are exactly 0 and the envelopes just decay from each
// trigger; the follower is left out.
//
// Output: the mono sum of upstream's two outputs, (carrier + 0.5 modulator)
// + 0.5 modulator, times the gain envelope. Upstream's float FMVoice stays
// in src/fm_voice.* for the harness build (src/part.cc).

#ifndef OP1RINGS_FM_VOICE_FIXED_H_
#define OP1RINGS_FM_VOICE_FIXED_H_

#include <cstddef>
#include <cstdint>

namespace op1rings {

class FmVoiceFixed {
 public:
  void Init();

  // All Q31. frequency: carrier frequency / sample rate. The others are in
  // [0, 1]: ratio (upstream: patch.structure), brightness (patch.brightness,
  // not squared), damping, feedback_amount (patch.position).
  void set_frequency(int32_t frequency) { carrier_frequency_ = frequency; }
  void set_ratio(int32_t ratio) { ratio_ = ratio; }
  void set_brightness(int32_t brightness) { brightness_ = brightness; }
  void set_damping(int32_t damping) { damping_ = damping; }
  void set_feedback_amount(int32_t feedback_amount) { feedback_amount_ = feedback_amount; }

  void TriggerInternalEnvelope() {
    amplitude_envelope_ = kOne;
    brightness_envelope_ = kOne;
  }

  // out: size Q27 samples (mono sum of upstream's out and aux).
  void Process(int32_t* out, size_t size);

 private:
  static const int32_t kOne = 0x7fffffff;

  void UpdateDecays();

  int32_t carrier_frequency_;    // Q31
  int32_t ratio_;
  int32_t brightness_;
  int32_t damping_;
  int32_t feedback_amount_;

  // Parameter interpolation state (upstream's previous_*).
  uint32_t previous_carrier_increment_;     // phase increment, f * 2^32
  uint32_t previous_modulator_increment_;
  int32_t previous_brightness_;             // Q31
  int32_t previous_feedback_;               // Q31, [-1, 1]

  // Decay coefficients for the current damping (per sample, Q31).
  int32_t decays_damping_;
  bool decays_valid_;
  int32_t amplitude_decay_;
  int32_t brightness_decay_;

  int32_t amplitude_envelope_;    // Q31
  int32_t brightness_envelope_;   // Q31
  int32_t gain_;                  // Q31
  int32_t fm_amount_;             // Q28, [0, 4]
  uint32_t carrier_phase_;
  uint32_t modulator_phase_;
  int32_t previous_sample_;       // Q30
};

}  // namespace op1rings

#endif  // OP1RINGS_FM_VOICE_FIXED_H_
