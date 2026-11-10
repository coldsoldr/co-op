// String + reverb's shared reverb on the OP-1 (src/op1_voice.h,
// SharedReverb): one reverb for every voice, as upstream runs one on the sum
// of its voices, instead of one per voice (whose cost grew with polyphony:
// four of them glitched on the device, Test 9).
//
// The OP-1 renders each sounding voice once per block (128 samples at
// 88.2 kHz), one call at a time, and applies the voice's envelope after the
// call. So:
// - the voices of string + reverb that aren't fading out after a steal (the
//   carriers) add their dry signal to the send while they render;
// - a block ends when a voice renders a second time, whatever order the
//   OP-1 renders its voices in; the reverb then runs once on that block's
//   send;
// - the next block's carriers each mix an equal share of its wet signal
//   into their output, before their limiter (upstream limits dry + wet), so
//   the voices together play it once, each share through its voice's
//   envelope (as the per-voice reverbs were). The wet signal is one block
//   (1.45 ms) late;
// - when the number of carriers changes, the shares move to 1 / n over a
//   block. A new carrier takes no share in its first block (it isn't
//   counted yet) and ramps in with the others in the next, so the shares
//   always sum to 1: a new note doesn't swell the reverb. A carrier that
//   stops is one whose envelope has closed, so its share was already
//   silent;
// - a steal hands the stolen voice's share to the voice that takes over its
//   ring (Transfer), so the reverb carries on through the new note as the
//   ring does;
// - a reverb that hasn't run for kGapCycles (no carriers: nothing sounding
//   in this model) restarts empty, so an old tail doesn't come back with the
//   next note.
//
// Header-only and free of OP-1 addresses: tests/test_reverb_bus.cc runs it
// on the host.

#ifndef OP1RINGS_OP1_REVERB_BUS_H_
#define OP1RINGS_OP1_REVERB_BUS_H_

#include <cstdint>

#include "src/fixed.h"
#include "src/op1_voice.h"

namespace op1 {

class ReverbBus {
 public:
  static const int32_t kBlock = static_cast<int32_t>(op1rings::kControlBlockSize);
  static const int32_t kNumVoices = 6;
  static const uint32_t kGapCycles = 20000000;   // 50 ms at 400 MHz

  void Init() {
    reverb_.Init();
    rendered_ = carriers_ = last_carriers_ = 0;
    share_ = 0;
    ran_ = false;
    last_run_ = 0;
    runs_ = clears_ = 0;
    for (int32_t v = 0; v < kNumVoices; ++v) voice_share_[v] = 0;
    for (int32_t i = 0; i < kBlock; ++i) send_[i] = wet_[i] = 0;
  }

  // Before rendering voice v; now: the cycle counter; patch: the voice's
  // parameters (the reverb follows those of the voice that ends a block).
  // A carrier then sends to send() and gets its share of the wet signal in
  // share_out (kBlock Q27 samples), to mix in before its limiter; returns
  // false (share_out untouched) if its share is 0.
  bool Render(int32_t v, uint32_t now, const op1rings::VoicePatch& patch, bool carrier,
              int32_t* share_out) {
    if (rendered_ & (1u << v)) EndBlock(now, patch);
    rendered_ |= 1u << v;
    if (!carrier) return false;
    carriers_ |= 1u << v;
    // A carrier the last block didn't count waits for the next one.
    const int32_t target = (last_carriers_ & (1u << v)) ? share_ : 0;
    int32_t share = voice_share_[v];
    voice_share_[v] = target;
    if (!share && !target) return false;
    const int32_t step = (target - share) / kBlock;
    for (int32_t i = 0; i < kBlock; ++i) {
      share += step;
      share_out[i] = op1rings::MulQ31Fast(wet_[i], share);
    }
    return true;
  }

  // A carrier's send, for RenderMono (kBlock samples).
  int32_t* send() { return send_; }

  // A steal (op1/voice_allocator.h): voice `to` takes over `from`'s ring,
  // and with it its place among the carriers and its share; `from` fades
  // out without one.
  void Transfer(int32_t from, int32_t to) {
    const uint32_t f = 1u << from, t = 1u << to;
    carriers_ = (carriers_ & ~(f | t)) | ((carriers_ & f) ? t : 0);
    last_carriers_ = (last_carriers_ & ~(f | t)) | ((last_carriers_ & f) ? t : 0);
    voice_share_[to] = voice_share_[from];
    voice_share_[from] = 0;
  }

  // For tests: reverb runs and clears so far.
  int32_t runs() const { return runs_; }
  int32_t clears() const { return clears_; }

 private:
  void EndBlock(uint32_t now, const op1rings::VoicePatch& patch) {
    int32_t n = 0;
    for (int32_t v = 0; v < kNumVoices; ++v) {
      if (carriers_ & (1u << v)) {
        ++n;
      } else {
        voice_share_[v] = 0;
      }
    }
    if (n) {
      if (!ran_ || now - last_run_ > kGapCycles) {
        reverb_.Clear();
        ++clears_;
      }
      ran_ = true;
      last_run_ = now;
      for (int32_t i = 0; i < kBlock; ++i) wet_[i] = send_[i];
      reverb_.Process(patch, wet_);
      ++runs_;
    }
    // 1 / n in Q31 (1 as INT32_MAX); 0: no wet signal for the next block.
    static const int32_t kShares[kNumVoices + 1] = {
      0, INT32_MAX, 1073741824, 715827883, 536870912, 429496730, 357913941 };
    share_ = kShares[n];
    for (int32_t i = 0; i < kBlock; ++i) send_[i] = 0;
    last_carriers_ = carriers_;
    rendered_ = carriers_ = 0;
  }

  op1rings::SharedReverb reverb_;
  int32_t send_[kBlock];           // this block's carriers' dry sum
  int32_t wet_[kBlock];            // the last block's wet signal
  uint32_t rendered_;              // voices rendered in this block
  uint32_t carriers_;              // the carriers among them
  uint32_t last_carriers_;         // the last block's carriers
  int32_t share_;                  // Q31: 1 / their number
  int32_t voice_share_[kNumVoices];   // each carrier's share at its last render
  bool ran_;
  uint32_t last_run_;
  int32_t runs_;
  int32_t clears_;
};

}  // namespace op1

#endif  // OP1RINGS_OP1_REVERB_BUS_H_
