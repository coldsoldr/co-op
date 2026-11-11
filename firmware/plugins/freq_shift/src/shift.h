// SHIFT: a Bode frequency shifter with barber-pole feedback and stereo
// spread, and a ring modulator (docs/fx-briefs/shift.md, docs/shift-port.md),
// in fixed point at the effect rate (44.1 kHz, up to 64 samples a call).
//
// coop port (plugins/freq_shift, from te-op1-cfw ports/fx/src/shift at
// 5aa314b; MIT, LICENSE-MIT): the kernels and the control are upstream's,
// unchanged. Changed: FreqShift runs one Hilbert pair PER CHANNEL (upstream
// summed L + R into one pair and spread that, which coop's rule against a
// mono collapse forbids: guide/plugin-guidelines.md §2.2), with the shortcut
// that an L == R block runs one pair for both; Process returns which
// channels it wrote; memset and <cstring> are gone; the C kernels are left
// out of the Blackfin build (the assembly runs there).
//
// Signal path (per sample), upstream:
//   x = (l + r) / 2 (stereo) or l (mono), x 2^10 in 32 bits
//   (coop: x = l for the left pair, r for the right pair)
//   -> Hilbert pair: two chains of 4 allpasses in z^-2 (Niemitalo's
//      polyphase IIR pair, as Warps' quadrature_transform.h), 32-bit data,
//      Q15 coefficients; the Q chain has an extra z^-1. I + jQ is the
//      analytic signal (> 49 dB sideband rejection, 50 Hz-18 kHz)
//   -> int16 I, Q; Q x m (m 1 = frequency shift, 0 = ring modulation,
//      ramped over 256 samples on a mode change)
//   per channel (L: shift + spread, R: shift - spread):
//   -> a = (I, mQ) + g fb, saturated (g = feedback; fb = the loop's
//      complex output 256 samples ago)
//   -> w = a e^(j phase) (512-entry sin/cos table, interpolated)
//   -> out = Re w (m 1: one sideband; m 0: a cos phase, both sidebands)
//   -> fb line <- low-pass(w) (one pole, 0.6)
//   -> dry/wet mix.
// The feedback runs in the complex (analytic) domain, so one Hilbert pair
// serves both channels and each still has its own barber-pole loop.
//
// The two kernels (Hilbert, channel) have C references (ShiftHilbertC,
// ShiftChannelC) and Blackfin versions (shift_bfin.S) that match them bit
// for bit (tests/test_shift.cc).

#ifndef OP1FX_SRC_SHIFT_SHIFT_H_
#define OP1FX_SRC_SHIFT_SHIFT_H_

#include <cstddef>
#include <cstdint>

#include "src/shift_layout.h"
#include "src/shift_tables.h"

namespace op1fx {

struct ShiftParams {
  int32_t shift;      // 0..32767, 16384 = 0 Hz: -2 kHz .. +2 kHz, cubic
  int32_t feedback;   // 0..32767 -> loop gain 0..0.93
  int32_t spread;     // 0..32767: L at shift + spread, R at shift - spread
  int32_t mix;        // 0..32767 dry -> wet
  bool ring;          // ring modulation (carrier |shift|) instead of shifting
  // An input gain on the wet path, Q14 (16384 = 0 dB; the kennel plugin's
  // page-2 word, -6..+6 dB): the analytic signal after the Hilbert is
  // scaled before the channels' rotation and feedback, saturating. The dry
  // side of the mix doesn't see it.
  int32_t gain = 16384;
};

struct ShiftHilbertKernel {
  int32_t xh[3];      // x[n-1], x[n-2], x[n-3]
  int32_t y[8][2];    // per section: y[n-1], y[n-2]
  int32_t m, dm;      // Q gain, Q15, 32768 = 1 (shift), 0 = ring; its step
  int16_t coef[8];
};

struct ShiftChannelKernel {
  uint32_t phase;
  int32_t inc, dinc;
  int32_t g, dg;      // feedback, Q15, and its step per sample
  int32_t mix, dmix;  // mix, Q15, and its step
  uint32_t lp;
  int32_t pos;        // bytes
  const uint32_t* osc;   // kShiftOsc
  uint32_t delay[SH_DELAY];
};

static_assert(offsetof(ShiftHilbertKernel, y) == SHK_Y, "layout");
static_assert(offsetof(ShiftHilbertKernel, m) == SHK_M, "layout");
static_assert(offsetof(ShiftHilbertKernel, dm) == SHK_DM, "layout");
static_assert(offsetof(ShiftHilbertKernel, coef) == SHK_COEF, "layout");
static_assert(sizeof(ShiftHilbertKernel) == SHK_SIZE, "layout");
static_assert(offsetof(ShiftChannelKernel, inc) == SCK_INC, "layout");
static_assert(offsetof(ShiftChannelKernel, dinc) == SCK_DINC, "layout");
static_assert(offsetof(ShiftChannelKernel, g) == SCK_G, "layout");
static_assert(offsetof(ShiftChannelKernel, dg) == SCK_DG, "layout");
static_assert(offsetof(ShiftChannelKernel, mix) == SCK_MIX, "layout");
static_assert(offsetof(ShiftChannelKernel, dmix) == SCK_DMIX, "layout");
static_assert(offsetof(ShiftChannelKernel, lp) == SCK_LP, "layout");
static_assert(offsetof(ShiftChannelKernel, pos) == SCK_POS, "layout");
#ifdef __bfin__   // (a pointer is 8 bytes on the host; the layout is for the asm)
static_assert(offsetof(ShiftChannelKernel, osc) == SCK_OSC, "layout");
static_assert(offsetof(ShiftChannelKernel, delay) == SCK_DELAY, "layout");
static_assert(sizeof(ShiftChannelKernel) == SCK_SIZE, "layout");
#endif

// The C kernels: the host build, and a Blackfin build with
// -DSHIFT_C_KERNELS (a simulator check); the plugin runs the assembly.
#if !defined(__bfin__) || defined(SHIFT_C_KERNELS)
#define SHIFT_HAVE_C 1
// Hilbert kernel: l (and r, or null) -> iq[i] = I (low half) | mQ (high
// half), mQ = Q when m = 32768 and dm = 0, else Q min(m, 32767) >> 15
// (m stepping by dm per sample). buf: SH_SCRATCH int32 scratch. n even,
// 2..64.
void ShiftHilbertC(ShiftHilbertKernel* k, const int16_t* l, const int16_t* r, uint32_t* iq,
                   int32_t* buf, int32_t n);
// Channel kernel: iq -> io (in place over the dry signal). write 0: io is
// only read (mix 0 throughout the call), the loop still runs. scratch:
// SH_MAX_BLOCK words (the assembly's; may be the Hilbert kernel's buf).
void ShiftChannelC(ShiftChannelKernel* k, const uint32_t* iq, int16_t* io, int32_t n,
                   int32_t write, uint32_t* scratch);
#endif

#ifdef __bfin__
extern "C" void ShiftHilbertAsm(ShiftHilbertKernel* k, const int16_t* l, const int16_t* r,
                                uint32_t* iq, int32_t* buf, int32_t n);
extern "C" void ShiftChannelAsm(ShiftChannelKernel* k, const uint32_t* iq, int16_t* io,
                                int32_t n, int32_t write, uint32_t* scratch);
#endif

// Control mappings (also used by the tests).
// Phase step for a signed knob position d (Q15, -32768..32767):
// 2000 Hz x (d / 32768)^3, as 2^32 / 44100 per Hz.
int32_t ShiftIncrement(int32_t d);
// Knob 0..32767 -> d: 2 (knob - 16384), so 0 -> -32768, 16384 -> 0.
inline int32_t ShiftKnobToD(int32_t knob) { return 2 * (knob - 16384); }
// Spread 0..32767 -> the offset in d: a third of the range at the top.
inline int32_t ShiftSpreadToD(int32_t spread) { return (spread * 10923) >> 15; }   // (no division)

class FreqShift {
 public:
  void Init();
  // coop: in place on both channels, n even 2..64. Each channel has its
  // own Hilbert pair (x = its own input; upstream's mono call with that
  // channel's input and settings, bit for bit) and its own shifter (L at
  // shift + spread, R at shift - spread). same: l[0..n) == r[0..n) (the
  // caller compared them); with the right pair known equal to the left one
  // such a block runs the left pair only and both channels read its I/Q.
  // Returns bit c set when channel c (0 l, 1 r) was written (mix 0 at both
  // ends of the call: not written).
  uint32_t Process(int16_t* l, int16_t* r, int32_t n, const ShiftParams& p, bool same);

#if defined(__bfin__) && defined(SHIFT_HAVE_C)
  void set_use_asm(bool use_asm) { use_asm_ = use_asm; }
#endif
  const ShiftHilbertKernel& hilbert(int i) const { return hk_[i]; }
  const ShiftChannelKernel& channel(int i) const { return ck_[i]; }
  // How the last block ran (the host test's counters): 1 one pair, 2 two.
  int32_t pairs_run() const { return pairs_run_; }
  // Syncs since Init: exact (the two pairs' states equal) and forced
  // (kShiftAgreeBlocks blocks of equal I/Q from unequal states).
  int32_t syncs_exact() const { return syncs_exact_; }
  int32_t syncs_forced() const { return syncs_forced_; }

 private:
  void Hilbert(int32_t c, const int16_t* x, int32_t n, int32_t m_end);

  ShiftHilbertKernel hk_[2];
  ShiftChannelKernel ck_[2];
  int32_t buf_[SH_SCRATCH];
  uint32_t iq_[2][SH_MAX_BLOCK];
  int32_t d_[2], inc_[2];   // the last positions and their phase steps
  // synced_: the right pair equals the left one (its stored state is stale
  // and is copied from the left before it runs again). agree_: L == R
  // blocks in a row in which the two pairs, unsynced, gave the same I/Q.
  int32_t synced_, agree_;
  int32_t pairs_run_, syncs_exact_, syncs_forced_;
  bool use_asm_;
};

// L == R blocks of equal I/Q from the two pairs, in a row, after which the
// right pair takes the left one's state (the difference left in the
// 32-bit states is then below the int16 rounding and decaying).
const int32_t kShiftAgreeBlocks = 16;

}  // namespace op1fx

#endif  // OP1FX_SRC_SHIFT_SHIFT_H_
