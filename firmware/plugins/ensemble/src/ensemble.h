// ENSEMBLE: a stereo chorus that morphs into a string ensemble, after Rings'
// and Plaits' fx/ensemble.h (Emilie Gillet, MIT), in fixed point, at
// 22050 Hz (the glue decimates 2:1 and interpolates the wet back, as for
// the plate). Wet only. docs/ensemble-port.md has the design and the
// measurements.
//
// Per side, up to three copies of the input, each read from a delay line
// at base + m_k (left) or base - m_k (right), where
//   m_k = A x slow(phi + k/3) + F x fast(psi + k/3),   k = 0, 1, 2
// slow: the RATE LFO (0.05..8 Hz); fast: a 6.57 Hz vibrato (Plaits'). The
// right side's sweeps are the left's inverted, so a mono input comes out
// wide. As in Rings, the third voice of each side reads the other side's
// input (stereo input).
//
// VOICES morphs one copy per side (chorus: gain 1, no vibrato) through two
// (1/2, 1/2) to three (1/3 each, the vibrato on: Rings' ensemble). The
// copies' gains always sum to 1.
//
// Arithmetic: int16 lines; each read is a two-tap interpolation (weights
// 32767 - f and f, a rounded MAC pair, as the plate's modulated taps); the
// voices are summed in a 40-bit accumulator, x their gains (Q15), rounded
// (the MAC's unbiased rounding) and saturated. The delays (Q16) and the
// gains are computed once per call and ramped per sample. The side kernel
// has a C reference (EnsembleSideRunC) and Blackfin versions
// (ensemble_bfin.S, bit-exact; tests/test_ensemble.cc).
//
// coop copy of te-op1-cfw ports/fx/src/ensemble/ensemble.h (namespace
// coop_ensemble, kernels EnsSide*Asm; on the Blackfin the assembly always
// runs and the C kernel isn't built). Docs: te-op1-cfw docs/ensemble-port.md.

#ifndef COOP_ENSEMBLE_ENSEMBLE_H_
#define COOP_ENSEMBLE_ENSEMBLE_H_

#include <cstdint>

#include "src/ensemble_layout.h"

namespace coop_ensemble {

struct EnsembleParams {
  int32_t rate;     // 0..32767 -> 0.05..8 Hz (log)
  int32_t depth;    // 0..32767 -> sweep 0..3.6 ms peak (less at fast rates)
  int32_t voices;   // 0..32767: one copy per side (chorus) -> three (ensemble)
};

const int32_t kEnsembleMaxBlock = 64;

// One side's run (ensemble_layout.h's ESK_ offsets). Read only: the caller
// works out each run's start values.
struct EnsembleSideKernel {
  const int16_t* line_a;   // voices 1 and 2: this side's line, at the run's first sample
  const int16_t* line_b;   // voice 3
  int32_t pos[3];          // delays, Q16 samples (+ step per sample)
  int32_t step[3];
  int32_t g12;             // g1 | g2 << 16 (Q15)
  int32_t g12_step;        // their steps, 16-bit lanes (wrapping adds)
  int32_t g3;              // g3 | its step << 16
};

// out[i] = sat(rnd(sum_k g_k x interp_k)), interp_k = the line read
// pos_k(i) samples back from sample i (line - i). voices: 3, or 1 (voice 1
// only: equal to 3 when g2 = g3 = 0 and their steps are 0). n > 0.
void EnsembleSideRunC(const EnsembleSideKernel* k, int16_t* out, int32_t n, int32_t voices);
extern "C" void EnsSide3Asm(const EnsembleSideKernel* k, int16_t* out, int32_t n);
extern "C" void EnsSide1Asm(const EnsembleSideKernel* k, int16_t* out, int32_t n);

// One call's control: the delays of the six reads (left voices 0..2, then
// right) and the three gains, each a start value and a step per sample.
struct EnsembleCoefs {
  int32_t pos0[6], pos_step[6];   // Q16 samples
  int32_t g0[3], g_step[3];       // Q15
  bool one_voice;                 // g2 = g3 = 0 all call: the voice-1 kernel
  // The smoothed values the delays came from (tests, the float reference).
  int32_t slow_amp, fast_amp;     // Q8 samples
  uint32_t slow_phase, fast_phase;   // at the end of the call
};

class EnsembleControl {
 public:
  void Init();
  // n even, 2..kEnsembleMaxBlock.
  void Update(const EnsembleParams& p, int32_t n, EnsembleCoefs* c);

 private:
  int32_t rate_, depth_, voices_;   // smoothed, Q15 << 8
  uint32_t slow_phase_, fast_phase_;
  int32_t pos_[6];                  // Q16, where the ramps ended
  int32_t g_[3];                    // Q15
  uint32_t inc_;                    // the slow LFO's increment
  int32_t a_, f_;                   // the sweep's and the vibrato's peaks, Q8 samples
  EnsembleParams last_;
  bool started_;
  bool settled_;                    // smoothers and gains on target
};

// The parameter curves (also used by the float reference).
uint32_t EnsembleRateIncrement(int32_t rate);   // slow LFO phase increment per sample
int32_t EnsembleSlowCap(int32_t rate);          // the sweep's peak at depth 1, Q8 samples
void EnsembleGains(int32_t voices, int32_t* g); // g[3], Q15
extern const uint32_t kEnsembleFastIncrement;   // 6.57 Hz

class Ensemble {
 public:
  void Init();
  // n even, 2..32 (up to 64). in_r null: mono input; wet_r null: mono
  // output (the left side only). Wet at the input's scale, saturated.
  // In-place calls (wet == in) are fine.
  void Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l, int16_t* wet_r,
               int32_t n, const EnsembleParams& p);

  // Tests.
  const EnsembleCoefs& coefs() const { return coefs_; }
  const int16_t* line(int32_t j) const { return j ? line_r_ : line_l_; }

 private:
  void Side(EnsembleSideKernel* k, int16_t* out, int32_t m);

  EnsembleControl control_;
  EnsembleCoefs coefs_;
  int32_t ptr_;                     // [1, EN_SIZE]: the next sample goes to ptr - 1
  alignas(4) int16_t line_l_[EN_WORDS];
  alignas(4) int16_t line_r_[EN_WORDS];
};

}  // namespace coop_ensemble

#endif  // COOP_ENSEMBLE_ENSEMBLE_H_
