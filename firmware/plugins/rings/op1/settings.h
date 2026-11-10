// Engine, model and polyphony settings, stored in the phase engine's spare
// preset values knobs[6], knobs[4] and knobs[5] (docs/screen-interface.md
// §5: stock code never writes them for phase, every preset stores 0, and
// they're saved and restored with presets). Values are setting x 4096, so 0
// (every existing preset) decodes as stock phase, and for Rings as the modal
// model and polyphony "all".

#ifndef OP1RINGS_OP1_SETTINGS_H_
#define OP1RINGS_OP1_SETTINGS_H_

#include <cstdint>

namespace op1 {

const int32_t kModelKnob = 4;
const int32_t kPolyKnob = 5;
const int32_t kEngineKnob = 6;
const int32_t kSettingStep = 4096;

// The engines the phase slot hosts ("MULTI"): stock phase first, so every
// existing phase preset keeps its sound.
enum Engine { ENGINE_PHASE = 0, ENGINE_RINGS = 1 };
const int32_t kNumEngines = 2;

const int32_t kNumModels = 6;   // Rings' models, as op1rings::ResonatorModel

// Polyphony states: "all" (6 voices) first so that 0 keeps the original
// behaviour; then Rings' 1, 2 and 4 voices.
enum PolyState { POLY_ALL = 0, POLY_1 = 1, POLY_2 = 2, POLY_4 = 3 };
const int32_t kNumPolyStates = 4;

// Rings voices and resonances per voice (Rings: 64 / polyphony - 4 modes;
// "all" has 14: 16 when it was sized from the measured free CPU, cut to 14
// in multi4 because six 16-mode voices crackled when the encoders turned,
// Tests 12-14; polyphony 1, 2 and 4 are upstream's and clean).
const int32_t kPolyVoices[kNumPolyStates] = { 6, 1, 2, 4 };
const int32_t kPolyModes[kNumPolyStates] = { 14, 60, 28, 12 };
// Rings' polyphony per state, for the string models: 8 / polyphony strings
// per sympathetic voice and the chord table. "All" plays as Rings'
// polyphony 4 (2 strings per voice).
const int32_t kRingsPolyphony[kNumPolyStates] = { 4, 1, 2, 4 };

inline int32_t DecodeSetting(int16_t k, int32_t count) {
  int32_t s = (static_cast<int32_t>(k) + kSettingStep / 2) / kSettingStep;
  if (s < 0) s = 0;
  if (s >= count) s = count - 1;
  return s;
}

inline int16_t EncodeSetting(int32_t s) { return static_cast<int16_t>(s * kSettingStep); }

// The models reachable by clicks, in Rings' order: modal, sympathetic
// strings, string, FM voice, quantized sympathetic strings, string + reverb.
const int32_t kModelCycle[] = { 0, 1, 2, 3, 4, 5 };
const int32_t kModelCycleLength = sizeof(kModelCycle) / sizeof(kModelCycle[0]);

// The next (direction 1) or previous (-1) model in kModelCycle; a model
// outside the cycle steps to the first.
inline int32_t StepModel(int32_t model, int32_t direction) {
  int32_t i = 0;
  while (i < kModelCycleLength && kModelCycle[i] != model) ++i;
  if (i == kModelCycleLength) return kModelCycle[0];
  i = (i + direction + kModelCycleLength) % kModelCycleLength;
  return kModelCycle[i];
}

// Polyphony cycles all -> 1 -> 2 -> 4 -> all (and back with -1).
inline int32_t StepPoly(int32_t poly, int32_t direction) {
  return (poly + direction + kNumPolyStates) % kNumPolyStates;
}

// The next (direction 1) or previous (-1) engine.
inline int32_t StepEngine(int32_t engine, int32_t direction) {
  return (engine + direction + kNumEngines) % kNumEngines;
}

#if defined(OP1_COOP)
// As a coop engine (plugins/rings): the core owns the controls (plugin.json
// "controls": fn + blue turn the model, fn + green turn the polyphony) and
// stores each one's step index in words 5 and 6, passed to note_on as
// params[5] and params[6]. A sound may hold anything there (one written by
// kennel, by hand, or before the controls existed): clamped, so 0 and any
// out-of-range word are the modal model and polyphony "all".
inline int32_t StepSetting(int16_t k, int32_t count) { return k < 0 ? 0 : (k >= count ? count - 1 : k); }
inline Engine EngineSetting(const int16_t*) { return ENGINE_RINGS; }
inline int32_t ModelSetting(const int16_t* knobs) { return StepSetting(knobs[5], kNumModels); }
inline PolyState PolySetting(const int16_t* knobs) {
  return static_cast<PolyState>(StepSetting(knobs[6], kNumPolyStates));
}
#elif defined(OP1_KENNEL)
// As a kennel plugin (ports/knl/cs_rings) Rings is its own engine, and the
// model and polyphony are the page-2 words a knob press edits (op1_plugin.h:
// blue -> word 5, green -> word 6). Kennel moves a page-2 word by about
// 1536 per encoder click (21 clicks for the range, Test 52), so a setting
// is the word in clicks, one step per click, cycling: model 0..5,
// polyphony all, 1, 2, 4. Rounded to the nearest click, so a step size a
// little off 1536 can't split a click. All-zero words (a new sound) are
// the modal model, polyphony "all", as before.
const int32_t kPageClick = 1536;
inline int32_t RangeSetting(int16_t k, int32_t count) {
  int32_t v = k;
  if (v < 0) v = 0;
  return ((v + kPageClick / 2) / kPageClick) % count;
}
inline Engine EngineSetting(const int16_t*) { return ENGINE_RINGS; }
inline int32_t ModelSetting(const int16_t* knobs) { return RangeSetting(knobs[5], kNumModels); }
inline PolyState PolySetting(const int16_t* knobs) {
  return static_cast<PolyState>(RangeSetting(knobs[6], kNumPolyStates));
}
#else
inline Engine EngineSetting(const int16_t* knobs) {
  return static_cast<Engine>(DecodeSetting(knobs[kEngineKnob], kNumEngines));
}
inline int32_t ModelSetting(const int16_t* knobs) { return DecodeSetting(knobs[kModelKnob], kNumModels); }
inline PolyState PolySetting(const int16_t* knobs) {
  return static_cast<PolyState>(DecodeSetting(knobs[kPolyKnob], kNumPolyStates));
}
#endif

}  // namespace op1

#endif  // OP1RINGS_OP1_SETTINGS_H_
