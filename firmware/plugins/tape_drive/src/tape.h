// coop: copied from te-op1-cfw ports/fx/src/tape (the kennel tape_drive 0.3
// build); changed: the include paths and gain_oct's initialiser (below).
//
// TAPE (dirt category,docs/fx-briefs/tape.md): tape saturation with
// wobble (wow and flutter) and age (head bump and HF loss), in fixed point
// at the effect rate (44.1 kHz, in place, n even 2..64). docs/tape-port.md
// has the design and the measurements.
//
// Wet path, per channel:
//   wobble   a modulated delay (Beads' WowAndFlutter, MIT, in fixed point):
//            0..245 samples, wow 0.25..1 Hz with a slowly drifting rate,
//            flutter about 7 Hz; linear interpolation; out at half scale
//   2x up    a polyphase IIR halfband (hiir's 4-coefficient design, two
//            first-order allpasses a path), the two paths packed in one
//            register's halves
//   saturate a quintic soft clip, S(v) = v - 2/3 v^3 + 1/5 v^5 (+-8/15
//            past |v| = 1), drive 0.125..8, the level made up for a
//            -12 dBFS sine
//   2x down  the same halfband
//   age      head bump: x + gb (lp180(x) - lp45(x)), up to +4 dB at 90 Hz
//            (the low-passes in the accumulators); HF loss: two one-poles,
//            20 kHz .. 2.2 kHz (Q16 states in data registers). Every state is
//            32-bit: no dead band, silence comes back to exact 0
//   mix      dry + (wet - dry) mix; mix 0 is the input, bit for bit. The
//            dry is not delayed (the wet's wobble delay is its own).
// Stereo runs the per-channel kernels on each side; the wobble is shared.
//
// Each run kernel has a C reference and a Blackfin version (tape_bfin.S,
// bit-exact, checked by tests/test_tape.cc). The 16-bit multiplies round to
// nearest, ties to even (RND_MOD cleared: unbiased, so no DC creeps in), as
// the C references model.

#ifndef OP1FX_SRC_TAPE_TAPE_H_
#define OP1FX_SRC_TAPE_TAPE_H_

#include <cstddef>
#include <cstdint>

#include "src/tape_layout.h"

namespace op1fx {

struct TapeParams {
  int32_t saturate;   // 0..32767: drive 0.125 .. 8 (exponential)
  int32_t wobble;     // 0..32767: none -> deep wow (half) -> flutter (top)
  int32_t age;        // 0..32767: new (flat) -> worn (dark, head bump)
  int32_t mix;        // 0..32767: dry -> wet
  // An input gain into the saturation, as Q16 octaves added to the drive's
  // exponent (the kennel plugin's page-2 word: -65536..65536, about -6..+6
  // dB). The exponent stays within the knob's own range (-2..4), so at
  // either end of the saturate knob the gain adds nothing; the level make-up
  // follows the knob alone, so the gain is heard as more or less saturation,
  // and the dry side of the mix never sees it.
  // coop: no default member initialiser here (it gave TapeParams, and so
  // TapeSat, a constructor: a static instance would then need .ctors, which
  // coop refuses). Every caller sets it.
  int32_t gain_oct;
};

const int32_t kTapeMaxBlock = 64;

struct TapeDelayKernel {
  uint32_t pos;       // the read position, Q16: (widx << 16) + the delay (mod 2^24)
  int32_t step;       // the delay's change, Q16 per sample
  int32_t widx;       // the last write, 0..TAPE_RING-1
};

struct TapeHbKernel {
  int32_t x, s1, s2;  // pairs
  int32_t c1, c2;     // pairs: c1 | c0, c3 | c2
};

struct TapeSatKernel {
  int32_t hsh;        // h << 16 | shift
  int32_t k12;        // K1 << 16 | K2
  int32_t k0;         // K0 << 16 | 32767
  int32_t j;          // output shift
};

struct TapeToneKernel {
  int32_t a0, a1;     // the bump's low-passes' accumulators (lp << 16)
  int32_t lp;         // pair: their rounded outputs, lpb | lpa
  int32_t s1, s2;     // the HF loss's one-poles: Q16 + 0x8000 (high half = rounded)
  int32_t kab;        // pair: kb | ka
  int32_t gk;         // pair: gb | k
  int32_t mix;        // pair: step | mix
};

struct TapeLaneKernel {
  TapeHbKernel up, down;
  TapeToneKernel tone;
};

static_assert(offsetof(TapeDelayKernel, pos) == TDK_POS, "layout");
static_assert(offsetof(TapeDelayKernel, step) == TDK_STEP, "layout");
static_assert(offsetof(TapeDelayKernel, widx) == TDK_WIDX, "layout");
static_assert(offsetof(TapeHbKernel, x) == THK_X, "layout");
static_assert(offsetof(TapeHbKernel, s1) == THK_S1, "layout");
static_assert(offsetof(TapeHbKernel, s2) == THK_S2, "layout");
static_assert(offsetof(TapeHbKernel, c1) == THK_C1, "layout");
static_assert(offsetof(TapeHbKernel, c2) == THK_C2, "layout");
static_assert(offsetof(TapeSatKernel, hsh) == TSK_HSH, "layout");
static_assert(offsetof(TapeSatKernel, k12) == TSK_K12, "layout");
static_assert(offsetof(TapeSatKernel, k0) == TSK_K0, "layout");
static_assert(offsetof(TapeSatKernel, j) == TSK_J, "layout");
static_assert(offsetof(TapeToneKernel, a0) == TTK_A0, "layout");
static_assert(offsetof(TapeToneKernel, a1) == TTK_A1, "layout");
static_assert(offsetof(TapeToneKernel, lp) == TTK_LP, "layout");
static_assert(offsetof(TapeToneKernel, s1) == TTK_S1, "layout");
static_assert(offsetof(TapeToneKernel, s2) == TTK_S2, "layout");
static_assert(offsetof(TapeToneKernel, kab) == TTK_KAB, "layout");
static_assert(offsetof(TapeToneKernel, gk) == TTK_GK, "layout");
static_assert(offsetof(TapeToneKernel, mix) == TTK_MIX, "layout");
static_assert(offsetof(TapeLaneKernel, down) == TLK_DOWN, "layout");
static_assert(offsetof(TapeLaneKernel, tone) == TLK_TONE, "layout");

// The run kernels (n >= 1). Buffers: ring TAPE_RING_WORDS pairs (R | L);
// w a lane's block (int16); u a lane's 2x block, one pair per input
// sample: (this even output | the previous odd one), the decimator's input
// order. The up and sat kernels write u[-1] (scratch) and read one past
// their input's end; the down kernel writes y[-1] (scratch).
//
// The wobble delay: l, r into the ring, the delayed samples out to wl, wr
// at half scale. r null: mono (the ring gets l | 0; wr unused). The
// assembly versions need widx >= n (no wrap within a run).
void TapeDelayRunC(TapeDelayKernel* k, int32_t* ring, const int16_t* l, const int16_t* r,
                   int16_t* wl, int16_t* wr, int32_t n);
// x -> u[0..n).
void TapeUpRunC(TapeHbKernel* k, const int16_t* x, int32_t* u, int32_t n);
// u[0..n) in place (stateless).
void TapeSatRunC(const TapeSatKernel* k, int32_t* u, int32_t n);
// u[0..n) -> y, full scale.
void TapeDownRunC(TapeHbKernel* k, const int32_t* u, int16_t* y, int32_t n);
// Age and mix: w (the wet) in place (filtered; w[-1] scratch), then io in
// place (mixed).
void TapeToneRunC(TapeToneKernel* k, int16_t* w, int16_t* io, int32_t n);
// One lane, all of the above but the delay: w -> u -> w -> io.
void TapeLaneRunC(TapeLaneKernel* k, const TapeSatKernel* sat, int16_t* w, int32_t* u, int16_t* io,
                  int32_t n);

extern "C" void TapeDelayRunAsm(TapeDelayKernel* k, int32_t* ring, const int16_t* l,
                                const int16_t* r, int16_t* wl, int16_t* wr, int32_t n);
extern "C" void TapeDelayRunMonoAsm(TapeDelayKernel* k, int32_t* ring, const int16_t* l,
                                    int16_t* wl, int32_t n);
// TapeLaneRunC in one call; n even (2..64), io needn't be aligned, w and
// u must be (4 bytes).
extern "C" void TapeLaneAsm(TapeLaneKernel* k, const TapeSatKernel* sat, int16_t* w, int32_t* u,
                            int16_t* io, int32_t n);

class TapeSat {
 public:
  void Init();
  // In place, n even 2..64; r null: mono.
  void Process(int16_t* l, int16_t* r, int32_t n, const TapeParams& p);

  // Tests.
  void set_use_asm(bool use_asm) { use_asm_ = use_asm; }
  int32_t delay_q16() const {
    return static_cast<int32_t>((dk_.pos - (static_cast<uint32_t>(dk_.widx) << 16)) & 0xffffffu);
  }
  const TapeSatKernel& sat_kernel() const { return sk_; }

 private:
  void Control(const TapeParams& p, int32_t n);
  void Knobs(const TapeParams& p, int32_t n, bool snap);
  void Mix(const TapeParams& p, int32_t n, bool snap);
  bool Depths(int32_t n);
  void Lfo(int32_t n);
  void CopyLeftToRight();
  void Lane(int32_t lane, int16_t* io, int32_t n);

  // Smoothed knobs, Q15 << 8.
  int32_t sat_, age_, wob_;
  int32_t mix_;                     // Q15
  int32_t sat_done_, age_done_, wob_done_;   // the values the coefficients are for
  int32_t gain_done_;                        // the gain the saturation's are for
  int32_t aw_, af_;                 // wow and flutter depths, Q8 samples
  int32_t aw_t_, af_t_;             // their targets
  uint32_t ph_wow_, ph_flut_, ph_rnd_;
  uint32_t rnd_inc_;                // the drift's rate, Q32 turns per 64 samples
  uint32_t rng_;
  TapeParams last_;                 // the knobs the smoothers last ran for
  bool started_;
  bool settled_;                    // the smoothers and depths are at their targets
  bool lfo_call_;                   // this call runs the LFOs (else the knobs)
  bool mono_;                       // the last call was mono
  bool use_asm_;

  TapeDelayKernel dk_;
  TapeSatKernel sk_;
  TapeLaneKernel lane_[2];
  int32_t ring_[TAPE_RING_WORDS];
  // A lane's block at w_[lane] + 2 and u_[lane] + 1: the kernels use the
  // word before (scratch) and read one past the end.
  int16_t w_[2][kTapeMaxBlock + 4];
  int32_t u_[2][kTapeMaxBlock + 2];
};

}  // namespace op1fx

#endif  // OP1FX_SRC_TAPE_TAPE_H_
