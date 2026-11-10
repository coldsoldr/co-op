// Fixed-point version of Rings' reverb (rings/dsp/fx/reverb.h on
// rings/dsp/fx/fx_engine.h, 16-bit buffer), mono, for model 5 (string +
// reverb) on the OP-1.
//
// Upstream: a Griesinger topology (4 input allpasses, then a loop of two
// branches of 2 allpasses + a delay, the long delays modulated by two slow
// LFOs) on one 32768-sample 16-bit buffer, fed with left + right and mixed
// back into each side: side += (wet_side - side) * amount. Mono, the sum of
// the two sides is dry + amount * (wet_left + wet_right - dry), which is
// what Process computes; upstream's odd/even pan before the reverb doesn't
// change left + right.
//
// Formats: the accumulator in Q27; the buffer stores Q15 (upstream's 16-bit
// format: value * 32768, clipped; floored where upstream truncates towards
// 0, a 1-LSB difference).
//
// Clear() is O(1): every read fetches what the same delay line wrote k
// samples earlier, so after a clear, reads further back than the samples
// written since return 0 (as upstream's zeroed buffer would) until the
// longest delay has been written through.

#ifndef OP1RINGS_REVERB_FIXED_H_
#define OP1RINGS_REVERB_FIXED_H_

#include <cstddef>
#include <cstdint>

namespace op1rings {

class ReverbFixed {
 public:
  static const int32_t kSize = 32768;          // upstream's buffer
  static const int32_t kBufferSize = 54400;    // see buffer_

  void Init();
  // Forgets the reverb's contents (no memory traffic).
  void Clear();

  // Upstream's parameters, Q31 in [0, 1).
  void set_amount(int32_t amount) { amount_ = amount; }
  void set_input_gain(int32_t gain) { input_gain_ = gain; }
  void set_time(int32_t time) { time_ = time; }
  void set_diffusion(int32_t diffusion) { diffusion_ = diffusion; }
  void set_lp(int32_t lp) { lp_ = lp; }

  // in_out: size Q27 samples (the dry mono signal), replaced by the mix.
  void Process(int32_t* in_out, size_t size);

  // For tests: must stay in [0, kSize).
  int32_t write_ptr() const { return write_ptr_; }

 private:
  template<bool kFresh, bool kAlias>
  void ProcessInternal(int32_t* in_out, size_t size);
  // The settled case through src/reverb_bfin.S (Blackfin).
  template<bool kAlias>
  void ProcessAsm(int32_t* in_out, size_t size);

  int32_t amount_;
  int32_t input_gain_;
  int32_t time_;
  int32_t diffusion_;
  int32_t lp_;

  int32_t lp_decay_1_;     // Q27
  int32_t lp_decay_2_;
  int32_t write_ptr_;
  int32_t age_;            // samples written since Clear (saturates)
  uint32_t lfo_phase_[2];
  int32_t lfo_value_[2];   // Q31, [0, 1]
  // Upstream's buffer, indexed without wrapping: index write_ptr + offset
  // (write_ptr in [0, kSize)), so every access is a fixed offset from the
  // write pointer. A sample written within kMaxReach of the write
  // pointer's wrap is also written kSize further on, where it's read after
  // the wrap.
  int16_t buffer_[kBufferSize];
};

}  // namespace op1rings

#endif  // OP1RINGS_REVERB_FIXED_H_
