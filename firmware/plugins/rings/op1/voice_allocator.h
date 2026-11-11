// Rings polyphony on top of the OP-1's six voices (op1/engine.cc).
//
// The OP-1 allocates its own six voices and renders each one separately,
// with its own pitch and envelope. In polyphony 1, 2 and 4 this class keeps
// that many of them "active", oldest first. A note that needs a voice when
// all are busy steals the oldest: the new OP-1 voice takes over the stolen
// voice's resonator state (copy_from) and strikes it at its own pitch, so the
// ring carries on into the new note as on Rings, while the stolen OP-1 voice
// fades out over ~6 ms. At most kMaxFading voices fade at once; older fades
// are shortened to keep the CPU bounded (in polyphony 1 every fading voice
// costs a full 60-mode voice).
//
// Polyphony "all" is the engine's original behaviour: every OP-1 voice keeps
// its own resonator and nothing is stolen.
//
// A voice stolen before it has rendered anything (a chord in polyphony 1:
// each note-on steals the one just before it) goes silent at once: it never
// sounded, so there's nothing to fade, and fading it would render up to six
// 60-mode voices in one block.
//
// A voice whose fade has finished is silent (not rendered) until it's struck
// again; its frozen state must then be cleared (reset).

#ifndef OP1RINGS_OP1_VOICE_ALLOCATOR_H_
#define OP1RINGS_OP1_VOICE_ALLOCATOR_H_

#include <cstdint>

#include "op1/settings.h"

namespace op1 {

class VoiceAllocator {
 public:
  static const int32_t kNumVoices = 6;
  static const int32_t kFadeSamples = 512;   // output samples (88.2 kHz): 5.8 ms
  static const int32_t kShortFadeSamples = 128;
  static const int32_t kMaxFading = 2;
  static const int32_t kFullGain = 32768;   // Q15

  struct NoteOnAction {
    int32_t copy_from;   // OP-1 voice whose state to take over, or -1
    bool reset;          // clear the voice's state before striking
  };

  void Init() {
    poly_ = POLY_ALL;
    num_active_ = 0;
    for (int32_t v = 0; v < kNumVoices; ++v) {
      silent_[v] = false;
      fresh_[v] = false;
      fade_gain_[v] = 0;
      fade_step_[v] = 0;
    }
  }

  NoteOnAction NoteOn(int32_t v, PolyState poly) {
    NoteOnAction action = { -1, silent_[v] };
    if (poly != poly_) {
      // A new polyphony starts a fresh voice list; voices still ringing from
      // the old one decay on their own.
      poly_ = poly;
      num_active_ = 0;
    }
    fade_gain_[v] = 0;   // a fading voice that's struck again stops fading
    fade_step_[v] = 0;
    if (poly != POLY_ALL) {
      Remove(v);
      if (num_active_ >= kPolyVoices[poly]) {
        const int32_t stolen = active_[0];
        Remove(stolen);
        StartFade(stolen);
        action.copy_from = stolen;
        action.reset = false;
      }
      active_[num_active_++] = v;
    }
    silent_[v] = false;
    fresh_[v] = true;
    return action;
  }

  // OP-1 voice v now plays another engine (stock phase): it leaves the
  // Rings voice list, stops fading, and its Rings state is reset when it's
  // next struck as Rings (it's marked silent; the other engine renders it).
  void Forget(int32_t v) {
    Remove(v);
    fade_gain_[v] = 0;
    fade_step_[v] = 0;
    fresh_[v] = false;
    silent_[v] = true;
  }

  // The engine calls this after rendering a voice.
  void Rendered(int32_t v) { fresh_[v] = false; }

  bool silent(int32_t v) const { return silent_[v]; }
  bool fading(int32_t v) const { return fade_step_[v] != 0; }

  // Fade gain (Q15) for sample i of the next block, and the per-sample step.
  int32_t fade_gain(int32_t v) const { return fade_gain_[v]; }
  int32_t fade_step(int32_t v) const { return fade_step_[v]; }

  // After rendering `samples` samples of a fading voice.
  void AdvanceFade(int32_t v, int32_t samples) {
    if (!fade_step_[v]) return;
    fade_gain_[v] -= fade_step_[v] * samples;
    if (fade_gain_[v] <= 0) {
      fade_gain_[v] = 0;
      fade_step_[v] = 0;
      silent_[v] = true;
    }
  }

  int32_t num_active() const { return num_active_; }
  int32_t active(int32_t i) const { return active_[i]; }

 private:
  void Remove(int32_t v) {
    int32_t j = 0;
    for (int32_t i = 0; i < num_active_; ++i) {
      if (active_[i] != v) active_[j++] = active_[i];
    }
    num_active_ = j;
  }

  void StartFade(int32_t v) {
    if (fresh_[v]) {
      silent_[v] = true;   // never rendered: nothing to fade
      return;
    }
    int32_t fading_count = 0;
    for (int32_t w = 0; w < kNumVoices; ++w) fading_count += fade_step_[w] != 0;
    if (fading_count >= kMaxFading) {
      // Finish the other fades within kShortFadeSamples, from where they are.
      for (int32_t w = 0; w < kNumVoices; ++w) {
        if (!fade_step_[w]) continue;
        const int32_t step = (fade_gain_[w] + kShortFadeSamples - 1) / kShortFadeSamples;
        if (step > fade_step_[w]) fade_step_[w] = step;
      }
    }
    fade_gain_[v] = kFullGain;
    fade_step_[v] = kFullGain / kFadeSamples;
  }

  PolyState poly_;
  int32_t active_[kNumVoices];   // oldest first
  int32_t num_active_;
  bool silent_[kNumVoices];
  bool fresh_[kNumVoices];        // struck, not rendered since
  int32_t fade_gain_[kNumVoices];
  int32_t fade_step_[kNumVoices];
};

}  // namespace op1

#endif  // OP1RINGS_OP1_VOICE_ALLOCATOR_H_
