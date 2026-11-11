// Encoder smoothing for the OP-1 engine, as upstream smooths its pots
// (rings/cv_scaler.cc: one-pole low-pass, 0.01 per 2 kHz control block,
// ~50 ms). Raw encoder steps would reach the resonator as coefficient steps,
// which click on large jumps (tests/step_params.cc).
//
// One smoother per voice, updated once per render call (689 Hz at 128
// samples / 88.2 kHz). A value snaps to its target once within 2^-12, so the
// resonator stops recomputing its coefficients soon after a knob stops. Snap()
// on note-on starts a new note where the knobs are, with no glide.

#ifndef OP1RINGS_OP1_PARAM_SMOOTHER_H_
#define OP1RINGS_OP1_PARAM_SMOOTHER_H_

#include <cstdint>

#include "src/fixed.h"

namespace op1 {

class ParamSmoother {
 public:
  static const int32_t kNumParams = 4;

  // 1 - exp(-1 / (50 ms * 689 Hz)), Q31.
  static const int32_t kCoefficient = 61434709;
  // Within 2^-12 of the target: snap. A step that small can't click.
  static const int32_t kSnap = 1 << 19;

  void Init() {
    for (int32_t i = 0; i < kNumParams; ++i) value_[i] = 0;
    snap_ = true;
  }

  void Snap() { snap_ = true; }

  // target: kNumParams values in Q31, [0, 1]. Returns the smoothed values.
  const int32_t* Process(const int32_t* target) {
    for (int32_t i = 0; i < kNumParams; ++i) {
      const int32_t delta = target[i] - value_[i];   // both >= 0: no overflow
      if (snap_ || (delta < kSnap && delta > -kSnap)) {
        value_[i] = target[i];
      } else {
        value_[i] += op1rings::MulQ31Fast(delta, kCoefficient);
      }
    }
    snap_ = false;
    return value_;
  }

 private:
  int32_t value_[kNumParams];
  bool snap_;
};

}  // namespace op1

#endif  // OP1RINGS_OP1_PARAM_SMOOTHER_H_
