// Types shared by the OP-1 voice (src/op1_voice.h) and its models.

#ifndef OP1RINGS_VOICE_PATCH_H_
#define OP1RINGS_VOICE_PATCH_H_

#include <cstdint>

namespace op1rings {

// Rings' parameters, in Q31 ([0, 1]).
struct VoicePatch {
  int32_t structure;
  int32_t brightness;
  int32_t damping;
  int32_t position;
};

// Rings' resonator models (upstream order, as rings::ResonatorModel).
enum ResonatorModel {
  MODEL_MODAL = 0,
  MODEL_SYMPATHETIC_STRING = 1,
  MODEL_STRING = 2,
  MODEL_FM_VOICE = 3,
  MODEL_SYMPATHETIC_STRING_QUANTIZED = 4,
  MODEL_STRING_AND_REVERB = 5,
};

}  // namespace op1rings

#endif  // OP1RINGS_VOICE_PATCH_H_
