// Rings' string models for one OP-1 voice. See string_voice.h.

#include "src/string_voice.h"

#include "src/fixed.h"
#include "src/fixed_tables.h"
#include "src/scratch.h"

#include "src/rate_config.h"

namespace op1rings {

static_assert(kStringMaxBlockSize <= static_cast<size_t>(kScratchBlock), "string scratch");

namespace {

const int32_t kOne = INT32_MAX;
const int32_t kHalf = 1 << 30;
const int32_t k1_12Q31 = 178956971;     // 1 / 12

// Compile-time constants are integer literals (coop: no float or double in a plugin file; defloat.py made them
// from upstream's Q(x, n) = x * 2^n rounded, at OP1RINGS_SAMPLE_RATE 22050).
static_assert(OP1RINGS_SAMPLE_RATE == 22050, "the literals are for 22.05 kHz (defloat.py)");

// Semitones, Q24 (St(x) = Q(x, 24)).

// Upstream's chord table (part.cc, BRYAN_CHORDS, which upstream's dsp.h
// defines), [polyphony - 1][chord][string], semitones from the note.
const int32_t kChords[4][11][kMaxStrings] = {
  {
    { -201326592 /* St(-12.0) */, -167772 /* St(-0.01) */, 0 /* St(0.0) */,  167772 /* St(0.01) */,  335544 /* St(0.02) */,  200991048 /* St(11.98) */, 201158820 /* St(11.99) */, 201326592 /* St(12.0) */ },  // OCT
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  117272740 /* St(6.99) */,  117440512 /* St(7.0) */,   201158820 /* St(11.99) */, 201326592 /* St(12.0) */,  318767104 /* St(19.0) */ },  // 5
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  83886080 /* St(5.0) */,   117440512 /* St(7.0) */,   201158820 /* St(11.99) */, 201326592 /* St(12.0) */,  285212672 /* St(17.0) */ },  // sus4
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  50331648 /* St(3.0) */,   117440512 /* St(7.0) */,   50499420 /* St(3.01) */,  201326592 /* St(12.0) */,  318767104 /* St(19.0) */ },  // m
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  50331648 /* St(3.0) */,   117440512 /* St(7.0) */,   50499420 /* St(3.01) */,  167772160 /* St(10.0) */,  318767104 /* St(19.0) */ },  // m7
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  50331648 /* St(3.0) */,   234881024 /* St(14.0) */,  50499420 /* St(3.01) */,  167772160 /* St(10.0) */,  318767104 /* St(19.0) */ },  // m9
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  50331648 /* St(3.0) */,   117440512 /* St(7.0) */,   50499420 /* St(3.01) */,  167772160 /* St(10.0) */,  285212672 /* St(17.0) */ },  // m11
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  33554432 /* St(2.0) */,   117440512 /* St(7.0) */,   150994944 /* St(9.0) */,   268435456 /* St(16.0) */,  318767104 /* St(19.0) */ },  // 69
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  67108864 /* St(4.0) */,   117440512 /* St(7.0) */,   184549376 /* St(11.0) */,  234881024 /* St(14.0) */,  318767104 /* St(19.0) */ },  // M9
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  67108864 /* St(4.0) */,   117440512 /* St(7.0) */,   184549376 /* St(11.0) */,  184381604 /* St(10.99) */, 318767104 /* St(19.0) */ },  // M7
    { -201326592 /* St(-12.0) */, -83886080 /* St(-5.0) */,  0 /* St(0.0) */,  67108864 /* St(4.0) */,   117440512 /* St(7.0) */,   201158820 /* St(11.99) */, 201326592 /* St(12.0) */,  318767104 /* St(19.0) */ },  // M
  },
  {
    { -201326592 /* St(-12.0) */, 0 /* St(0.0) */,  167772 /* St(0.01) */,  201326592 /* St(12.0) */ },  // OCT
    { -201326592 /* St(-12.0) */, 117272740 /* St(6.99) */, 117440512 /* St(7.0) */,   201326592 /* St(12.0) */ },  // 5
    { -201326592 /* St(-12.0) */, 83886080 /* St(5.0) */,  117440512 /* St(7.0) */,   201326592 /* St(12.0) */ },  // sus4
    { -201326592 /* St(-12.0) */, 50331648 /* St(3.0) */,  201158820 /* St(11.99) */, 201326592 /* St(12.0) */ },  // m
    { -201326592 /* St(-12.0) */, 50331648 /* St(3.0) */,  167772160 /* St(10.0) */,  201326592 /* St(12.0) */ },  // m7
    { -201326592 /* St(-12.0) */, 50331648 /* St(3.0) */,  167772160 /* St(10.0) */,  234881024 /* St(14.0) */ },  // m9
    { -201326592 /* St(-12.0) */, 50331648 /* St(3.0) */,  167772160 /* St(10.0) */,  285212672 /* St(17.0) */ },  // m11
    { -201326592 /* St(-12.0) */, 33554432 /* St(2.0) */,  150994944 /* St(9.0) */,   268435456 /* St(16.0) */ },  // 69
    { -201326592 /* St(-12.0) */, 67108864 /* St(4.0) */,  184549376 /* St(11.0) */,  234881024 /* St(14.0) */ },  // M9
    { -201326592 /* St(-12.0) */, 67108864 /* St(4.0) */,  117440512 /* St(7.0) */,   184549376 /* St(11.0) */ },  // M7
    { -201326592 /* St(-12.0) */, 67108864 /* St(4.0) */,  117440512 /* St(7.0) */,   201326592 /* St(12.0) */ },  // M
  },
  {
    { 0 /* St(0.0) */, -201326592 /* St(-12.0) */ },
    { 0 /* St(0.0) */, 33554432 /* St(2.0) */ },
    { 0 /* St(0.0) */, 50331648 /* St(3.0) */ },
    { 0 /* St(0.0) */, 67108864 /* St(4.0) */ },
    { 0 /* St(0.0) */, 83886080 /* St(5.0) */ },
    { 0 /* St(0.0) */, 117440512 /* St(7.0) */ },
    { 0 /* St(0.0) */, 150994944 /* St(9.0) */ },
    { 0 /* St(0.0) */, 167772160 /* St(10.0) */ },
    { 0 /* St(0.0) */, 184549376 /* St(11.0) */ },
    { 0 /* St(0.0) */, 201326592 /* St(12.0) */ },
    { -201326592 /* St(-12.0) */, 201326592 /* St(12.0) */ },
  },
  {
    { 0 /* St(0.0) */, -201326592 /* St(-12.0) */ },
    { 0 /* St(0.0) */, 33554432 /* St(2.0) */ },
    { 0 /* St(0.0) */, 50331648 /* St(3.0) */ },
    { 0 /* St(0.0) */, 67108864 /* St(4.0) */ },
    { 0 /* St(0.0) */, 83886080 /* St(5.0) */ },
    { 0 /* St(0.0) */, 117440512 /* St(7.0) */ },
    { 0 /* St(0.0) */, 150994944 /* St(9.0) */ },
    { 0 /* St(0.0) */, 167772160 /* St(10.0) */ },
    { 0 /* St(0.0) */, 184549376 /* St(11.0) */ },
    { 0 /* St(0.0) */, 201326592 /* St(12.0) */ },
    { -201326592 /* St(-12.0) */, 201326592 /* St(12.0) */ },
  },
};
const int32_t kNumChords = 11;

// ComputeSympatheticStringsNotes' notes, relative to the note (the tonic is
// the note, see string_voice.h).
const int32_t kSympatheticNotes[9] = {
  0 /* St(0.0) */, -201326592 /* St(-12.0) */, -117768507 /* St(-7.01955) */, 0 /* St(0.0) */, 117768507 /* St(7.01955) */, 201326592 /* St(12.0) */, 319095099 /* St(19.01955) */, 402653184 /* St(24.0) */, 402653184 /* St(24.0) */,
};
const int32_t kDetunings[4] = { 218104 /* St(0.013) */, 184549 /* St(0.011) */, 117441 /* St(0.007) */, 285213 /* St(0.017) */ };

// Upstream's LFO frequencies per string (part.cc; the 8th is 0), as phase
// increments per sample. Upstream's CosineOscillator in approximate mode
// computes 2 cos(w) as 2 - 32 f^2, which runs at sqrt(32) / (2 pi) of f.
// LfoIncrement(hz) = 0.9003163161571061 hz / OP1RINGS_SAMPLE_RATE 2^32.
const uint32_t kLfoIncrements[kMaxStrings] = {
  87683u /* LfoIncrement(0.5) */, 70146u /* LfoIncrement(0.4) */, 61378u /* LfoIncrement(0.35) */, 40334u /* LfoIncrement(0.23) */,
  37002u /* LfoIncrement(0.211) */, 35073u /* LfoIncrement(0.2) */, 29987u /* LfoIncrement(0.171) */, 0,
};

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

// stmlib's SemitonesToRatio for semitones in Q24 (|x| < 128): floored to
// 1/256 semitone as its table lookup does, after the float rounding of
// semitones + 128 (to 2^-16 above 128, 2^-17 below), which snaps
// near-integers such as 11.999999 to the integer.
inline FFloat SemitonesToRatio(int32_t semitones) {
  const int32_t q8 = (semitones + (semitones < 0 ? 0x40 : 0x80)) >> 16;
  return Exp2(MulQ31(q8 << 15, k1_12Q31) << 1);
}

// x (2 - x) for x in [0, 1], Q31.
inline int32_t OneMinusSquaredComplement(int32_t x) {
  return ShlSat(x - (MulQ31(x, x) >> 1), 1);
}

// Upstream Part::Squash, x in [0, 1) Q31.
inline int32_t Squash(int32_t x) {
  if (x < kHalf) {
    int32_t y = x << 1;
    y = MulQ31(y, y);
    y = MulQ31(y, y);
    y = MulQ31(y, y);
    y = MulQ31(y, y);
    return y >> 1;
  }
  int32_t y = (kOne - x) << 1;
  y = MulQ31(y, y);
  y = MulQ31(y, y);
  y = MulQ31(y, y);
  y = MulQ31(y, y);
  return kOne - (y >> 1);
}

// 0.5 + 0.5 cos(2 pi phase), Q31 (upstream CosineOscillator::Next's value).
inline int32_t Lfo(uint32_t phase) {
  const uint32_t p = phase + 0x40000000u;   // cos = sin + 1/4 cycle
  const uint32_t i = p >> 20;
  const int32_t frac = static_cast<int32_t>((p & 0xfffff) << 11);
  const int32_t s = kSineQ31[i] + MulQ31Fast(kSineQ31[i + 1] - kSineQ31[i], frac);
  return kHalf + (s >> 1);
}

}  // namespace

void StringVoice::Init() {
  state_.model = -1;
  state_.polyphony = 1;
  state_.num_strings = 1;
  state_.dispersion = false;
  state_.sympathetic = false;
  state_.quantized = false;
  state_.tuned = false;
  state_.chord = 0;
  state_.glide_valid = false;
  state_.plucker.Init();
  state_.dc_blocker.Init(2146509733 /* Q(1.0 - 10.0 / OP1RINGS_SAMPLE_RATE, 31) */);
}

void StringVoice::Configure(ResonatorModel model, int32_t polyphony) {
  if (state_.model == model && state_.polyphony == polyphony) return;
  state_.model = model;
  state_.polyphony = polyphony;
  state_.sympathetic = model == MODEL_SYMPATHETIC_STRING ||
                       model == MODEL_SYMPATHETIC_STRING_QUANTIZED;
  state_.quantized = model == MODEL_SYMPATHETIC_STRING_QUANTIZED;
  state_.dispersion = model == MODEL_STRING || model == MODEL_STRING_AND_REVERB;
  state_.num_strings = state_.sympathetic ? 2 * 4 / polyphony : 1;
  state_.tuned = false;
  state_.glide_valid = false;
  state_.frequencies_valid = false;
  state_.next_update = 1;
  for (int32_t i = 0; i < state_.num_strings; ++i) {
    strings_[i].Init();
    state_.lfo_phase[i] = 0;
  }
  if (state_.dispersion) stretch_.Init();
}

void StringVoice::CopyFrom(const StringVoice& other) {
  state_ = other.state_;
  int32_t reach = 0;
  for (int32_t i = 0; i < state_.num_strings; ++i) {
    const int32_t r = strings_[i].CopyRecentFrom(other.strings_[i]);
    if (i == 0) reach = r;
  }
  // The stretch allpass reads at most half of string 0's delay back.
  if (state_.dispersion) stretch_.CopyRecentFrom(other.stretch_, reach);
}

void StringVoice::Strum(int32_t f0, int32_t cutoff, int32_t position) {
  // Upstream: filter_cutoff * 8, which Plucker::Trigger caps at 0.499.
  const int32_t plucker_cutoff = cutoff >= (kOne >> 3) ? kOne : cutoff << 3;
  state_.plucker.Trigger(f0, plucker_cutoff, position);
}

void StringVoice::UpdateTuning(int32_t structure) {
  int32_t parameter = structure;
  if (state_.quantized) {
    // Upstream CvScaler: chord = structure * 10, with hysteresis.
    const int32_t chord = MulQ31(structure, (kNumChords - 1) << 24);   // Q24
    const int32_t hysteresis = chord - (state_.chord << 24) > 0 ? -1677722 /* Q(0.1, 24) */ : 1677722 /* Q(0.1, 24) */;
    int32_t c = (chord + hysteresis + 8388608 /* Q(0.5, 24) */) >> 24;
    if (c < 0) c = 0;
    if (c > kNumChords - 1) c = kNumChords - 1;
    state_.chord = c;
    parameter = c;
  }
  if (state_.tuned && parameter == state_.tuned_parameter) return;
  state_.tuned = true;
  state_.frequencies_valid = false;
  state_.tuned_parameter = parameter;

  const int32_t n = state_.num_strings;
  int32_t notes[kMaxStrings];
  if (state_.quantized) {
    const int32_t* chord = kChords[state_.polyphony - 1][parameter];
    for (int32_t i = 0; i < n; ++i) notes[i] = chord[i];
  } else {
    // Upstream ComputeSympatheticStringsNotes.
    const int32_t num_detuned = (n - 1) >> 1;
    const int32_t first_detuned = n - num_detuned;
    for (int32_t i = 0; i < first_detuned; ++i) {
      int32_t note = 3 << 28;                                    // Q28
      if (i != 0) {
        note = MulQ31(parameter, 7 << 28);
        parameter += MulQ31(kOne - parameter, 429496730 /* Q(0.2, 31) */);
      }
      const int32_t integral = note >> 28;
      const int32_t fractional = Squash((note & 0x0fffffff) << 3);
      const int32_t a = kSympatheticNotes[integral];
      const int32_t b = kSympatheticNotes[integral + 1];
      notes[i] = a + MulQ31(b - a, fractional);
      if (i + first_detuned < n) notes[i + first_detuned] = notes[i] + kDetunings[i & 3];
    }
  }
  for (int32_t i = 0; i < n; ++i) state_.ratios[i] = SemitonesToRatio(notes[i]);
}

// The strings' parameters for a block (dispersion, tuning, the sympathetic strings' brightness, damping, position and
// glide, each string's frequency), and which strings may update their tone: bit k of the result. advance_lfo: move
// the LFOs on by the block (Process does; the coop batch, which runs ahead of the block's render, doesn't).
uint32_t StringVoice::SetParameters(int32_t f0, const VoicePatch& patch, size_t size, bool advance_lfo) {
  const int32_t n = state_.num_strings;
  // Dispersion from structure, with a dead zone at 0.24-0.26.
  const int32_t structure = patch.structure;
  int32_t dispersion = 0;
  if (structure < 515396076 /* Q(0.24, 31) */) {
    dispersion = MulQ31(structure - 515396076 /* Q(0.24, 31) */, 1118302110 /* Q(4.166, 28) */) << 3;
  } else if (structure > 558345748 /* Q(0.26, 31) */) {
    dispersion = MulQ31(structure - 558345748 /* Q(0.26, 31) */, 1451001014 /* Q(1.35135, 30) */) << 1;
  }

  if (state_.sympathetic) UpdateTuning(structure);

  // The sympathetic strings' brightness, damping, position range and glide.
  int32_t sympathetic_brightness = 0, sympathetic_damping = 0, lfo_amount = 0;
  if (n > 1) {
    sympathetic_brightness = OneMinusSquaredComplement(OneMinusSquaredComplement(patch.brightness));
    sympathetic_damping = 1503238554 /* Q(0.7, 31) */ + MulQ31(patch.damping, 579820585 /* Q(0.27, 31) */);
    int32_t distance = patch.position - kHalf;
    if (distance < 0) distance = -distance;
    lfo_amount = MulQ31(kHalf - distance, 1932735283 /* Q(0.9, 31) */);
    if (!state_.glide_valid || state_.glide_brightness != sympathetic_brightness ||
        state_.glide_size != static_cast<int32_t>(size)) {
      // glide = SemitonesToRatio((brightness - 1) * 36), per 32 samples at
      // 44.1 kHz; per 64 (or per 32 at 22.05 kHz: the same time),
      // 1 - (1 - glide)^2.
      const int32_t semitones = MulQ31(sympathetic_brightness - kOne, 36 << 24);   // Q24
      int32_t glide = SemitonesToRatio(semitones).ToQ31();
      if (size * (44100 / OP1RINGS_SAMPLE_RATE) == 64) {
        const int32_t rest = kOne - glide;
        glide = kOne - MulQ31(rest, rest);
      }
#if OP1RINGS_CONTROL_HALVED
      {
        // A step every other block: the coefficient for two, 1 - (1 - g)^2, so a glide takes as long.
        const int32_t rest = kOne - glide;
        glide = kOne - MulQ31(rest, rest);
      }
#endif
      state_.glide = glide;
      state_.glide_brightness = sympathetic_brightness;
      state_.glide_size = static_cast<int32_t>(size);
      state_.glide_valid = true;
    }
  }

  int32_t log2_n = 0;
  while ((1 << log2_n) < n) ++log2_n;
  const int32_t string_index_shift = 31 - log2_n;
  // The strings' frequencies, recomputed only when the pitch or the tuning
  // changes.
  if (state_.sympathetic && (!state_.frequencies_valid || state_.frequencies_f0 != f0)) {
    const FFloat f0f = FFloat::FromQ(f0, 31);
    for (int32_t k = 0; k < n; ++k) state_.frequencies[k] = (f0f * state_.ratios[k]).ToQ31();
    state_.frequencies_f0 = f0;
    state_.frequencies_valid = true;
  }

  // Every string's parameters for this block first.
  for (int32_t k = 0; k < n; ++k) {
    StringFixed& s = strings_[k];
    const int32_t lfo = Lfo(state_.lfo_phase[k]);
    if (advance_lfo) state_.lfo_phase[k] += kLfoIncrements[k] * static_cast<uint32_t>(size);

    int32_t brightness = patch.brightness;
    int32_t damping = patch.damping;
    int32_t position = patch.position;
    if (state_.model == MODEL_STRING_AND_REVERB) damping = OneMinusSquaredComplement(damping);
    if (k > 0) {
      brightness = sympathetic_brightness;
      damping = sympathetic_damping;
      position += MulQ31(lfo, lfo_amount);
    }
    // damping + (k / n) (0.95 - damping).
    const int32_t string_index = k << string_index_shift;              // Q31
    damping += MulQ31(string_index, 2040109466 /* Q(0.95, 31) */ - damping);

    s.set_dispersion(dispersion);
    if (state_.sympathetic) {
      const int32_t f = state_.frequencies[k];
      if (k == 0) {
        s.set_frequency(f);
      } else {
        s.set_frequency(f, state_.glide);
      }
    } else {
      s.set_frequency(f0);
    }
    s.set_brightness(brightness);
    s.set_position(position);
    s.set_damping(damping);
  }

  // Brightness and damping updates (~2,000 cycles each) are spread out:
  // string 0 and at most kMaxSympatheticUpdates sympathetic strings per
  // block, in turn. A sympathetic string waits a few blocks at most, behind
  // ~50 ms of knob smoothing. Frequency changes (glides) always apply.
  const int32_t kMaxSympatheticUpdates = 2;
  uint32_t allowed = 1;   // bit k: string k may update; string 0 always
  if (n > 1) {
    int32_t updates = 0;
    int32_t k = state_.next_update;
    for (int32_t j = 0; j < n - 1 && updates < kMaxSympatheticUpdates; ++j) {
      if (strings_[k].needs_tone_update()) {
        allowed |= 1u << k;
        ++updates;
        state_.next_update = k + 1 < n ? k + 1 : 1;
      }
      k = k + 1 < n ? k + 1 : 1;
    }
  }

  return allowed;
}

// OP1RINGS_CONTROL_BATCHED (the coop build): the block's control work ahead of its render, in the engine's batch
// (plugins/rings/rings.cc): the parameters, then each string's configuration; the render that follows holds them
// (set_hold) and runs the strings only.
void StringVoice::Control(int32_t f0, const VoicePatch& patch, size_t size) {
  const uint32_t allowed = SetParameters(f0, patch, size, false);
  for (int32_t k = 0; k < state_.num_strings; ++k) strings_[k].Prepare((allowed >> k) & 1, state_.dispersion);
}

void StringVoice::Process(int32_t f0, const VoicePatch& patch, int32_t* out, size_t size) {
  const int32_t n = state_.num_strings;

  // Scratch shared by all voices (src/scratch.h: L1 on the OP-1, where the
  // string kernels read and write it once per string and sample).
  int32_t* excitation = scratch()->strings.excitation;
  int32_t* sympathetic = scratch()->strings.sympathetic;
  int32_t* mono = scratch()->strings.mono;

  // Excitation: the plucker through the voice's DC blocker (the audio input
  // is silent on the OP-1). Once both are quiet, it's zeros.
  if (!state_.plucker.quiet() || !state_.dc_blocker.idle()) {
    state_.plucker.Process(excitation, size);
    state_.dc_blocker.Process(excitation, size);
  } else {
    for (size_t i = 0; i < size; ++i) excitation[i] = 0;
  }
  for (size_t i = 0; i < size; ++i) mono[i] = 0;

  if (hold_) {
    // A held block: the strings keep their parameters; only the LFOs advance (their pace is per sample).
    for (int32_t k = 0; k < n; ++k) state_.lfo_phase[k] += kLfoIncrements[k] * static_cast<uint32_t>(size);
    for (int32_t k = 0; k < n; ++k) {
      strings_[k].Process(k == 0 ? excitation : sympathetic, mono, size,
                          state_.dispersion ? &stretch_ : nullptr, false);
      if (k == 0 && n > 1) {
        int32_t log2_n = 0;
        while ((1 << log2_n) < n) ++log2_n;
        const int32_t gain = 429496730 /* Q(0.2, 31) */ >> log2_n;   // 0.2 / n, as below
        for (size_t i = 0; i < size; ++i) sympathetic[i] = MulQ31Fast(mono[i], gain);
      }
    }
    for (size_t i = 0; i < size; ++i) out[i] = ShlSat(mono[i], 2);   // Q25 -> Q27
    return;
  }

  const uint32_t allowed = SetParameters(f0, patch, size, true);
  int32_t log2_n = 0;
  while ((1 << log2_n) < n) ++log2_n;
  const int32_t string_index_shift = 31 - log2_n;

  for (int32_t k = 0; k < n; ++k) {
    strings_[k].Process(k == 0 ? excitation : sympathetic, mono, size,
                        state_.dispersion ? &stretch_ : nullptr, (allowed >> k) & 1);
    if (k == 0 && n > 1) {
      // Was 0.1f, Ben Wilson -> 0.2f (upstream).
      const int32_t gain = 429496730 /* Q(0.2, 31) */ >> (31 - string_index_shift);   // 0.2 / n
      for (size_t i = 0; i < size; ++i) sympathetic[i] = MulQ31Fast(mono[i], gain);
    }
  }

  for (size_t i = 0; i < size; ++i) out[i] = ShlSat(mono[i], 2);   // Q25 -> Q27
}

}  // namespace op1rings
