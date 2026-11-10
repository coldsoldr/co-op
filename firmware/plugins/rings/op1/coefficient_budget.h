// A budget for the modal model's coefficient updates on the OP-1
// (op1/engine.cc). While parameters move (encoders turning, knob smoothing,
// pitch bend), every ringing modal voice recomputes its coefficients each
// render: ~860 cycles per mode, so six 16-mode voices take ~35% more CPU
// than when still, and polyphony 1 with a fading 60-mode voice doubles a
// 60-mode update. On the device that overloaded the CPU (Test 12: clicks and
// pops while turning).
//
// So at most kBudgetModes modes are recomputed per OP-1 block (one render
// per sounding voice), plus one voice whatever its size:
// - a block ends when a voice renders a second time (as op1/reverb_bus.h)
//   or when the voice index goes down: the OP-1 renders its voices in index
//   order (inferred), so blocks line up with its calls even after the set
//   of sounding voices changes. (With "repeat" alone, a block could start at
//   voice 1 and straddle two calls: 4 updates in one, 2 in the next.) In any
//   other order blocks only get shorter: more updates, never fewer;
// - at the start of a block, the voices whose last render wanted an update
//   are granted it in turn (round robin, from just after the last voice
//   granted) until the next would overrun the budget; the others keep their
//   coefficients for a block (RingsVoice::set_coefficient_hold) and ask
//   again. A grant lasts until the voice renders (and counts against the
//   budget until then);
// - a voice that wanted none last block waits one block (1.45 ms) when its
//   parameters start moving, so a knob turn never makes every voice update
//   at once;
// - a strike always updates its voice (RingsVoice), outside the budget;
// - while a voice fades out after a steal (polyphony 1, 2, 4: two full
//   voices render for 512 samples), no voice updates: the fade lasts four
//   blocks, and the new note's strike has just updated it. The fading voice
//   itself never updates (op1/engine.cc).
// Six 16-mode voices turning then update two per block, each one every
// third block (~230 Hz), behind the ~50 ms knob smoothing: a sudden +0.5
// structure step clicks at -65.5 dB re the note (every block: -69.7 dB;
// make step-params checks below -60 dB). Polyphony 2 and 4 alternate the
// same way; a single 60-mode voice still updates every block.
//
// Header-only and free of OP-1 addresses: tests/test_voice_allocator.cc
// runs it on the host.

#ifndef OP1RINGS_OP1_COEFFICIENT_BUDGET_H_
#define OP1RINGS_OP1_COEFFICIENT_BUDGET_H_

#include <cstdint>

namespace op1 {

class CoefficientBudget {
 public:
  static const int32_t kNumVoices = 6;
  static const int32_t kBudgetModes = 32;   // two voices of "all" (14 modes)

  void Init() {
    rendered_ = granted_ = 0;
    last_voice_ = 0;
    fading_ = last_fading_ = false;
    start_ = 0;
    for (int32_t v = 0; v < kNumVoices; ++v) demand_[v] = 0;
  }

  // Before rendering voice v: whether it may recompute its coefficients
  // (using up its grant).
  bool Allow(int32_t v) {
    if ((rendered_ & (1u << v)) || v < last_voice_) NewBlock();
    rendered_ |= 1u << v;
    last_voice_ = v;
    const bool allowed = (granted_ >> v) & 1;
    granted_ &= ~(1u << v);
    return allowed;
  }

  // After rendering voice v: the modes it wanted to recompute (0 if none),
  // and whether it's fading out. Kept until its next render.
  void Report(int32_t v, int32_t modes, bool fading) {
    demand_[v] = fading ? 0 : modes;
    if (fading) fading_ = true;
  }

 private:
  // v + k for v, k < kNumVoices, modulo kNumVoices (no division).
  static int32_t Wrap(int32_t v) { return v >= kNumVoices ? v - kNumVoices : v; }

  void NewBlock() {
    last_fading_ = fading_;
    fading_ = false;
    int32_t budget = last_fading_ ? 0 : kBudgetModes;
    // Grants not used yet (a voice that hasn't rendered since) still count.
    for (int32_t v = 0; v < kNumVoices; ++v) {
      if (granted_ & (1u << v)) budget -= demand_[v];
    }
    int32_t next = Wrap(start_ + 1);
    for (int32_t k = 0; k < kNumVoices; ++k) {
      const int32_t v = Wrap(start_ + k);
      const int32_t d = demand_[v];
      if (!d || (granted_ & (1u << v))) continue;
      // The first voice in turn always gets its update, even over budget
      // (but none while a voice fades).
      if (d > budget && (budget != kBudgetModes || last_fading_)) break;
      granted_ |= 1u << v;
      budget -= d;
      next = Wrap(v + 1);
    }
    // Round robin: the next block starts after the last voice granted.
    start_ = next;
    rendered_ = 0;
  }

  uint32_t rendered_;   // voices rendered in this block
  uint32_t granted_;    // voices allowed to update at their next render
  int32_t last_voice_;  // the last voice rendered
  bool fading_;         // a voice faded in this block
  bool last_fading_;    // in the last one
  int32_t start_;       // the voice first in turn
  int32_t demand_[kNumVoices];   // each voice's last report
};

}  // namespace op1

#endif  // OP1RINGS_OP1_COEFFICIENT_BUDGET_H_
