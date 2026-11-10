// Rings' string models for one OP-1 voice: upstream Part::RenderStringVoice
// for one Rings voice with the internal exciter, in fixed point.
//
//   strum -> plucker (noise burst + comb + low-pass) -> DC blocker -> string 0
//   string 0's out - aux, x 0.2 / n -> strings 1..n-1 (sympathetic)
//   output: the sum of every string's out - aux
//
// Models:
//   MODEL_STRING, MODEL_STRING_AND_REVERB: one string with dispersion
//     (structure; dead zone at 0.24-0.26). The reverb itself isn't here:
//     this class only reshapes damping as upstream does for that model.
//   MODEL_SYMPATHETIC_STRING: n = 8 / polyphony strings (8, 4, 2 for Rings'
//     polyphony 1, 2, 4), tuned from structure by upstream's
//     ComputeSympatheticStringsNotes;
//   MODEL_SYMPATHETIC_STRING_QUANTIZED: the same, tuned to a chord from
//     upstream's table (Bryan Noll's, as upstream builds), picked by
//     structure with upstream's hysteresis.
//
// Upstream tunes the sympathetic strings around its tonic (the FREQUENCY
// knob) and the V/OCT note. The OP-1 only gives the voice's pitch, so the
// tonic is the played note: Rings played from its FREQUENCY knob.
//
// Mono: Rings sends out - aux per voice when polyphonic; for a string that's
// the string minus its pickup tap, the pickup-position comb. Every
// polyphony uses it here.

#ifndef OP1RINGS_STRING_VOICE_H_
#define OP1RINGS_STRING_VOICE_H_

#include <cstddef>
#include <cstdint>

#include "src/ffloat.h"
#include "src/string_fixed.h"
#include "src/voice_patch.h"

namespace op1rings {

const int32_t kMaxStrings = 8;               // upstream kNumStrings
#ifndef OP1RINGS_CONTROL_BLOCK_SIZE
#define OP1RINGS_CONTROL_BLOCK_SIZE 64       // src/op1_voice.h: one control block per render call
#endif
const size_t kStringMaxBlockSize = OP1RINGS_CONTROL_BLOCK_SIZE;

class StringVoice {
 public:
  // Cheap: the strings are cleared by the next Configure.
  void Init();

  // A string model and Rings' polyphony (1, 2 or 4). A change re-initialises
  // the strings it uses, as upstream's ConfigureResonators does.
  void Configure(ResonatorModel model, int32_t polyphony);

  // Takes over another voice's state (only the strings it uses).
  void CopyFrom(const StringVoice& other);

  // A strum: upstream triggers the plucker with (frequency, filter cutoff x 8,
  // position). f0 and cutoff (the excitation filter's, <= 0.499) are Q31
  // fractions of the sample rate.
  void Strum(int32_t f0, int32_t cutoff, int32_t position);

  // f0: Q31. out: size Q27 samples (size: a power of two, at most
  // kStringMaxBlockSize). Upstream's per-block glide is scaled to the block
  // size: per 32 samples as given, squared per 64.
  void Process(int32_t f0, const VoicePatch& patch, int32_t* out, size_t size);

  int32_t num_strings() const { return state_.num_strings; }
  // String k's frequency / f0 (for tests).
  FFloat ratio(int32_t k) const { return state_.ratios[k]; }

 private:
  void UpdateTuning(int32_t structure);

  // Everything but the delay lines, copied in one go by CopyFrom.
  struct State {
    int32_t model;                  // -1: not configured
    int32_t polyphony;
    int32_t num_strings;
    bool dispersion;
    bool sympathetic;
    bool quantized;
    bool tuned;
    int32_t tuned_parameter;        // structure, or the chord when quantized
    int32_t chord;                  // quantized: current chord (hysteresis)
    FFloat ratios[kMaxStrings];     // string frequency / f0
    bool glide_valid;
    int32_t glide_brightness;
    int32_t glide_size;
    int32_t glide;                  // sympathetic strings' glide per block, Q31
    bool frequencies_valid;
    int32_t frequencies_f0;
    int32_t frequencies[kMaxStrings];   // f0 x ratios, Q31
    uint32_t lfo_phase[kMaxStrings];
    int32_t next_update;            // sympathetic string to update first
    PluckerFixed plucker;
    DcBlockerFixed dc_blocker;
  };

  State state_;
  StretchLine stretch_;
  StringFixed strings_[kMaxStrings];

};

}  // namespace op1rings

#endif  // OP1RINGS_STRING_VOICE_H_
