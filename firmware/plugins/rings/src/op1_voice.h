// One Rings voice for the OP-1, all fixed point (no floating point at run
// time). It replaces rings::Part for a single voice: the OP-1 core does the
// voice allocation, so there's no note filter, strummer or polyphony here.
//
// Signal path (modal model, internal exciter, as upstream with polyphony 1):
//   note on -> impulse -> excitation low-pass -> ModalResonator -> x1.4 ->
//   limiter -> out (odd modes) / aux (even modes)
// The FM voice (FmVoiceFixed) and the string models (StringVoice) replace
// the impulse, filter and resonator; string + reverb sends to a reverb
// shared by every voice (SharedReverb).
//
// Pitch comes from the OP-1's per-sample phase increments (f / fs * 2^32);
// parameters from its four encoders.

#ifndef OP1RINGS_OP1_VOICE_H_
#define OP1RINGS_OP1_VOICE_H_

#include <cstddef>
#include <cstdint>

#include "src/ffloat.h"
#include "src/fm_voice_fixed.h"
#include "src/modal_resonator.h"
#include "src/reverb_fixed.h"
#include "src/string_voice.h"
#include "src/voice_patch.h"

#include "src/rate_config.h"

namespace op1rings {

#ifndef OP1RINGS_SUBBLOCK_SIZE
#define OP1RINGS_SUBBLOCK_SIZE 32
#endif

// Pitch and parameters are latched once per control block (a multiple of
// the sub-block), so the coefficients are recomputed at most once per
// control block. Upstream's control rate is 2 kHz (24 samples at 48 kHz).
#ifndef OP1RINGS_CONTROL_BLOCK_SIZE
#define OP1RINGS_CONTROL_BLOCK_SIZE 64
#endif

const size_t kSubblockSize = OP1RINGS_SUBBLOCK_SIZE;
const size_t kControlBlockSize = OP1RINGS_CONTROL_BLOCK_SIZE;

// Low-pass TPT SVF (stmlib::Svf, FREQUENCY_DIRTY) for the exciter, Q27 state.
class ExcitationFilter {
 public:
  void Init();
  // f: cutoff / sample rate (Q31, <= 0.499), resonance 1.5 (internal exciter).
  void SetCutoff(int32_t f);
  // Adds a unit impulse of amplitude x0 (given as g, h-scaled terms) at the
  // next sample: see op1_voice.cc.
  void AddImpulse(int32_t first_output, int32_t s1, int32_t s2);
  void Process(int32_t* out, size_t size);   // zero input, Q27 output

  int32_t g() const { return g_; }
  int32_t h() const { return h_; }

 private:
  int32_t g_;          // Q29
  int32_t k_;          // r + g, Q29
  int32_t h_;          // Q31
  int32_t s1_, s2_;    // Q27
  bool pending_;       // impulse terms to add at the next sample
  int32_t pending_output_;
  int32_t pending_s1_;
  int32_t pending_s2_;
};

// Rings' limiter (rings/dsp/limiter.h), stereo, fixed point.
class Limiter {
 public:
  void Init();
  void Process(int32_t* l, int32_t* r, size_t size, int32_t pre_gain_q28);

 private:
  int32_t peak_;   // Q27
  int32_t gain_;   // 1 / max(peak, 1), Q31
};

// The limiter's fixed gains for a model: upstream's pre-gain
// (Part::model_gains_) and pre-gain * 0.8 (the gain below the threshold).
struct LimiterGains {
  int32_t pre_q28;
  int32_t out_q30;
  FFloat out;
};

// MonoLimiter's sample loop, in C and in Blackfin assembly
// (src/limiter_bfin.S), bit-exact with each other: peak follower on
// |x pre_gain|, x = SoftLimit(2 x gain).
void MonoLimiterLoopC(int32_t* x, int32_t size, int32_t* peak, int32_t gain, int32_t pre_gain);
extern "C" void MonoLimiterAsm(int32_t* x, int32_t size, int32_t* peak, int32_t gain,
                               int32_t pre_gain);

// Mono limiter for the OP-1 build (the voice's two outputs are summed): same
// peak follower as Rings' limiter, with the gain (1 / peak) updated once per
// sub-block and the fixed gains folded into one multiply.
class MonoLimiter {
 public:
  void Init();
  void Process(int32_t* x, size_t size, const LimiterGains& gains);

 private:
  int32_t peak_;   // Q27
  int32_t gain_;   // pre-gain * 0.8 / max(peak, 1), Q30
};

// String + reverb's reverb (upstream Part::Process on rings/dsp/fx/
// reverb.h), shared by the voices as upstream runs one reverb on the sum of
// its voices. Upstream mixes dry + amount (wet - dry); the OP-1 renders its
// voices separately, so each voice plays dry (1 - amount) itself and sends
// its dry signal (RenderMono's send), and this runs on the sum and gives
// back amount * wet, which op1/reverb_bus.h shares out among the voices.
class SharedReverb {
 public:
  void Init();
  void Clear() { reverb_.Clear(); }
  // in_out: kControlBlockSize Q27 samples, the voices' summed dry signal;
  // replaced by amount * wet (the voices' limiters come after).
  void Process(const VoicePatch& patch, int32_t* in_out);
  // Upstream's amount, 0.1 + 0.5 damping (Q31).
  static int32_t Amount(int32_t damping);

 private:
  ReverbFixed reverb_;
};

class RingsVoice {
 public:
  void Init(int32_t resolution);
  void NoteOn() { strum_ = true; silent_blocks_ = 0; }

  // Any model (RenderMono only; Render is modal).
  void set_model(ResonatorModel model) { model_ = model; }
  ResonatorModel model() const { return model_; }
  // Modes per voice (the coefficients are recomputed on the next render).
  void set_resolution(int32_t resolution) { resonator_.set_resolution(resolution); }
  int32_t resolution() const { return resonator_.resolution(); }
  // RenderMono's voice has been below kSleepLevel long enough to stop
  // computing until its next note (it renders silence, or only its share
  // of the shared reverb).
  bool asleep() const { return !strum_ && silent_blocks_ >= kSilentBlocksToSleep; }
  static const int32_t kSleepLevel = 1 << 11;      // Q27 peak: ~ -96 dBFS (Test 29: -72 cut tails short)
  static const int32_t kSilentBlocksToSleep = 8;   // RenderMono calls (~12 ms)
  // Modal model: keep the resonator's coefficients this render even if the
  // parameters move (a strike always updates them); see
  // op1/coefficient_budget.h.
  void set_coefficient_hold(bool hold) { coefficient_hold_ = hold; }
  // The modes whose coefficients the parameters asked to recompute since
  // the last call (updated or held): the resolution, or 0.
  int32_t TakeCoefficientDemand() {
    return resonator_.TakeChanged() ? resonator_.resolution() : 0;
  }
  // Rings' polyphony (1, 2 or 4): the string models' strings per voice and
  // chord table.
  void set_polyphony(int32_t polyphony) { polyphony_ = polyphony; }

  // Takes over another voice's state, for a voice that fades out after a
  // steal. Copies only what the voice's model uses, and of the string delay
  // lines only what the next few milliseconds read: a plain copy would move
  // up to ~70 KB.
  void CopyFrom(const RingsVoice& other);

  // phase_increment: size values (f / fs * 2^32); out, aux: size Q27 samples
  // (after the limiter, ~[-1, 1]). size must be a multiple of
  // kControlBlockSize.
  void Render(const uint32_t* phase_increment, const VoicePatch& patch,
              int32_t* out, int32_t* aux, size_t size);

  // OP-1 build: the model's two outputs summed, mono limiter. out: size Q27
  // samples. String + reverb plays its dry part, adds its dry signal to
  // send and mixes wet (its share of the shared reverb) in before the
  // limiter; either may be null (a voice fading out after a steal has
  // neither: the voice that took over carries the ring on). size samples
  // each.
  void RenderMono(const uint32_t* phase_increment, const VoicePatch& patch,
                  int32_t* out, size_t size, int32_t* send = nullptr,
                  const int32_t* wet = nullptr);

 private:
  void RenderSubblock(uint32_t phase_increment, const VoicePatch& patch,
                      int32_t* out, int32_t* aux);

  void RenderFmSubblock(uint32_t phase_increment, const VoicePatch& patch,
                        int32_t* out);

  // A whole control block (string models run at the control rate).
  void RenderStringBlock(uint32_t phase_increment, const VoicePatch& patch,
                         int32_t* out, int32_t* send);

  ResonatorModel model_;
  int32_t polyphony_;
  bool strum_;
  bool coefficient_hold_;
  // The excitation filter's cutoff for the last pitch and brightness (it
  // costs FFloat work, a reciprocal included, and they rarely change).
  bool cutoff_valid_;
  int32_t cutoff_f0_;
  int32_t cutoff_brightness_;
  FFloat cutoff_fc_;
  int32_t silent_blocks_;               // consecutive renders below the sleep level
  ModalResonator resonator_;
  FmVoiceFixed fm_voice_;
  ExcitationFilter excitation_filter_;
  Limiter limiter_;
  MonoLimiter mono_limiter_;
  StringVoice string_voice_;
};

}  // namespace op1rings

#endif  // OP1RINGS_OP1_VOICE_H_
