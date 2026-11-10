// Fixed-point version of Rings' modal resonator (rings/dsp/resonator.cc).
//
// Audio loop: the Chamberlin mode bank (mode_bank.h / mode_bank_bfin.S).
// Coefficients: per block, in fixed point from tables (fixed_tables.h); only
// the partial frequencies use FFloat. With phi = atan(tan_fast(pf)) (upstream's
// tan approximation), r = 1 / Q and x = r sin(phi) cos(phi):
//   rho = (1 + x)^-1/2
//   f/2 = sin(phi) rho        q/2 = r cos(phi) rho / 2     gain = cos(phi) rho / 2
// which equal upstream's f = 2 g sqrt(h), q = r sqrt(h), gain = sqrt(h) / 2.
// When the coefficients change, the state is translated so the equivalent TPT
// state stays continuous, as in upstream (see docs/rings-port.md).

#ifndef OP1RINGS_MODAL_RESONATOR_H_
#define OP1RINGS_MODAL_RESONATOR_H_

#include <cstddef>
#include <cstdint>

#include "src/mode_bank.h"

namespace op1rings {

const int32_t kMaxModalModes = 64;
const int32_t kMaxModalBlockSize = 32;

// Profiling (tests/sim_engine.cc): when
// profile_clock is set, the cycles spent recomputing coefficients are added
// to profile_update_cycles.
extern uint32_t (*profile_clock)();
extern uint32_t profile_update_cycles;
extern uint32_t profile_mode_bank_cycles;   // the mode bank's share of Process

class ModalResonator {
 public:
  void Init();

  // All parameters in Q31: frequency is f0 / sample rate; the others are in
  // [0, 1]. brightness is the value upstream passes (patch.brightness^2).
  void set_frequency(int32_t frequency) { frequency_ = frequency; }
  void set_structure(int32_t structure) { structure_ = structure; }
  void set_brightness(int32_t brightness) { brightness_ = brightness; }
  void set_damping(int32_t damping) { damping_ = damping; }
  void set_position(int32_t position) { position_ = position; }
  // Fewer modes than before drops the top ones at once; modes that come
  // back (a trimmed voice struck again, op1/engine.cc) start from rest.
  void set_resolution(int32_t resolution) {
    resolution -= resolution & 1;
    if (resolution > kMaxModalModes) resolution = kMaxModalModes;
    for (int32_t i = resolution_; i < resolution; ++i) modes_[i].lp = modes_[i].bp = 0;
    resolution_ = resolution;
  }

  // in: Q27 input (upstream scale, before its * 0.125); out, aux: Q27
  // (out == aux: their sum in one buffer).
  void Process(const int32_t* in, int32_t* out, int32_t* aux, size_t size);

  // While held, Process keeps the current coefficients even if the
  // parameters changed (the OP-1 engine spreads coefficient updates over
  // blocks: op1/coefficient_budget.h). The first update always happens.
  void set_hold(bool hold) { hold_ = hold; }
  // Whether the parameters differed from the coefficients' at a Process
  // since the last call (updated or held); clears the flag.
  bool TakeChanged() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
  }

  int32_t num_modes() const { return num_modes_; }
  int32_t resolution() const { return resolution_; }

  // For tests: computes the coefficients for the current parameters.
  int32_t UpdateCoefficients() { return ComputeFilters(); }
  const Mode& mode(int32_t i) const { return modes_[i]; }
  int32_t gain(int32_t i) const { return gain_[i]; }

 private:
  // Per-mode values kept from the last coefficient update, for the state
  // translation.
  struct Geometry {
    int32_t sin;      // Q31
    int32_t cos;      // Q31
    int32_t sec;      // Q28
    int32_t tan;      // Q28
    int32_t rho;      // Q31
    int32_t x;        // r sin cos, Q31, [0, 0.5]
  };

  int32_t ComputeFilters();
  int32_t RecomputeFilters();   // ComputeFilters' work, when it's needed
  void ComputeWeights(int32_t position, int32_t* weights, int32_t n) const;

  int32_t frequency_;
  int32_t structure_;
  int32_t brightness_;
  int32_t damping_;
  int32_t position_;
  int32_t previous_position_;
  int32_t resolution_;

  // Control values used for the current coefficients (to skip recomputing).
  int32_t computed_frequency_;
  int32_t computed_structure_;
  int32_t computed_brightness_;
  int32_t computed_damping_;
  int32_t computed_resolution_;
  bool coefficients_valid_;
  bool hold_;
  bool changed_;
  // scratch_start_ holds the weights for previous_position_ with the current
  // gains (n of them): the next block starts from them.
  bool weights_valid_;
  int32_t weights_n_;

  int32_t num_modes_;
  int32_t gain_[kMaxModalModes];   // cos rho / 2, Q31
  int32_t previous_input_;         // Q27, already * 0.125
  Mode modes_[kMaxModalModes];
  Geometry geometry_[kMaxModalModes];
  int32_t log2_pf_[kMaxModalModes];  // log2(partial frequency), Q24

  // Kept off the stack: on the OP-1 the render call may run on a small
  // interrupt stack. (The input and the mode bank's scratch are shared by
  // all voices: src/scratch.h.)
  int32_t scratch_start_[kMaxModalModes];
  int32_t scratch_end_[kMaxModalModes];
};

}  // namespace op1rings

#endif  // OP1RINGS_MODAL_RESONATOR_H_
