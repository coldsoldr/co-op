/*
 * rings: Mutable Instruments Rings as a coop synth engine (docs/engines.md E3). The engine is te-op1-cfw's
 * ports/rings/op1/engine.cc as the kennel plugin built it (-DOP1_KENNEL, 22.05 kHz, upsampled 4x), moved onto the coop
 * engine ABI (sdk/coop_plugin.h):
 *   - the voice index, the pitch words and the knob words come from the call's arguments: the firmware addresses the
 *     kennel build read (the iter voice objects, 0xff801950, 0xff8013c4) are gone;
 *   - model and polyphony are coop controls (plugin.json "controls": fn + blue turn, fn + green turn), the step index
 *     in params[5] and params[6], latched at note on as before (op1/settings.h OP1_COOP);
 *   - the per-call scratch is the core's L1 scratchpad loan (coop_l1_scratch, the window kennel rings used), the
 *     soft-limit table goes into the L1 lease at load (coop_l1_lease), each with an SDRAM fallback;
 *   - the voice states and the shared reverb (~572 KB) are ordinary .bss: the loader zeroes it, and an engine is
 *     relocated once, so the six voices share it (kennel put them at a fixed SDRAM page outside its arena);
 *   - everything is set up in init (main thread, at load) instead of at the first note.
 *
 * Six OP-1 voices, one RingsVoice each; the stock envelope, gain and LFO follow the render, and coop places each voice
 * in stereo. Knobs: blue structure, green brightness, white damping, orange position, smoothed per voice over ~50 ms
 * (op1/param_smoother.h). Velocity is not used, as in the kennel build (Rings' strike has no level of its own).
 *
 * Rings and stmlib: MIT, (c) Emilie Gillet; the fixed-point port: coldsoldr (te-op1-cfw), same terms. This file:
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <cstdint>

#include "coop_plugin.h"
#include "op1/coefficient_budget.h"
#include "op1/op1.h"
#include "op1/param_smoother.h"
#include "op1/reverb_bus.h"
#include "op1/settings.h"
#include "op1/voice_allocator.h"
#include "src/fixed.h"
#include "src/fixed_tables.h"
#include "src/halfband_taps.h"
#include "src/op1_voice.h"
#include "src/profile.h"
#include "src/scratch.h"

static_assert(OP1RINGS_UPSAMPLE == 4 && OP1RINGS_SAMPLE_RATE == 22050, "the coop build is 22.05 kHz, upsampled 4x");

// The upsampler's half-band stage in Blackfin assembly (src/halfband_bfin.S, from gen_halfband_asm.py): the C loop
// below with every product on the MAC units, bit-identical (tests/host/rings/halfband_check.cc).
extern "C" void HalfbandAsm(const int16_t* x, const int16_t* xs, int32_t* mid3);

extern "C" const int32_t* kSoftLimitTable;           // src/op1_voice.cc: the limiter kernel's table

namespace op1rings {
Scratch* g_scratch;                                    // src/scratch.h: this call's scratch
#ifdef RINGS_PROFILE
uint64_t g_profile[PROF_COUNT];
#endif
}

namespace {

using op1rings::RingsVoice;
using op1rings::VoicePatch;

const int32_t kRingsBlockSize = op1::kBlockSize / OP1RINGS_UPSAMPLE;   // 32 samples at 22.05 kHz
static_assert(kRingsBlockSize == op1rings::kScratchBlock, "scratch block size");
static_assert(kRingsBlockSize == static_cast<int32_t>(op1rings::kControlBlockSize),
              "one control block per render call (OP1RINGS_CONTROL_BLOCK_SIZE)");

// 4x upsampler, two stages (engine.cc, unchanged). First a half-band FIR doubles 22.05 kHz to 44.1 kHz
// (src/halfband_taps.h: flat to 9 kHz, -0.7 dB at 10 kHz, -59 dB from 13 kHz); the even outputs are the input delayed
// by 12 samples, the odd ones a 24-tap symmetric sum. Then a 4-point Lagrange 2x stage to 88.2 kHz. Delay: 12 samples
// at 22.05 kHz + 2 at 44.1 kHz (0.59 ms). The input is clamped to 16 bits, so the Q14 sums fit 32 bits.
class Upsampler {
 public:
  void Init() {
    for (int32_t i = 0; i < op1rings::kScratchHalfbandHistory; ++i) history_[i] = 0;
    mid_history_[0] = mid_history_[1] = mid_history_[2] = 0;
  }

  // in: kRingsBlockSize Q27 samples; out: 4 * kRingsBlockSize samples.
  void Process(const int32_t* in, int16_t* out) {
    const int32_t kHistory = op1rings::kScratchHalfbandHistory;   // 24 = 2 x 12 taps
    // The 22.05 kHz samples are clamped to 16 bits, so they are kept as int16 and every product is a 16 x 16
    // multiply (the MAC units') instead of a 32 x 32 one; c (x[-j] + x[1 + j]) is computed as c x[-j] + c x[1 + j],
    // the same integer sum (no term or partial sum leaves 32 bits).
    int16_t* x = op1rings::scratch()->upsampler.x;                // the voice has rendered
    int32_t* mid = op1rings::scratch()->upsampler.mid;
    for (int32_t i = 0; i < kHistory; ++i) x[i] = history_[i];
    for (int32_t i = 0; i < kRingsBlockSize; ++i) x[kHistory + i] = op1::Saturate16(in[i] >> 12);
    mid[0] = mid_history_[0];
    mid[1] = mid_history_[1];
    mid[2] = mid_history_[2];
#if defined(__bfin__) && !defined(OP1RINGS_NO_ASM)
    int16_t* xs = op1rings::scratch()->upsampler.xs;
    for (int32_t i = 0; i + 1 < kHistory + kRingsBlockSize; ++i) xs[i] = x[i + 1];
    HalfbandAsm(x, xs, mid + 3);
#else
    for (int32_t m = 0; m < kRingsBlockSize; ++m) {
      const int16_t* c = x + m + 12;
      int32_t acc = 1 << 13;
      for (int32_t j = 0; j < 12; ++j) {
        const int16_t tap = static_cast<int16_t>(op1rings::kHalfbandTaps[j]);
        acc += static_cast<int32_t>(tap) * c[-j];
        acc += static_cast<int32_t>(tap) * c[1 + j];
      }
      mid[3 + 2 * m] = c[0];
      mid[3 + 2 * m + 1] = acc >> 14;
    }
#endif
    for (int32_t k = 0; k < 2 * kRingsBlockSize; ++k) {
      out[2 * k] = op1::Saturate16(mid[k + 1]);
      out[2 * k + 1] = op1::Saturate16((9 * (mid[k + 1] + mid[k + 2]) - mid[k] - mid[k + 3]) >> 4);
    }
    for (int32_t i = 0; i < kHistory; ++i) history_[i] = x[kRingsBlockSize + i];
    mid_history_[0] = mid[2 * kRingsBlockSize];
    mid_history_[1] = mid[2 * kRingsBlockSize + 1];
    mid_history_[2] = mid[2 * kRingsBlockSize + 2];
  }

 private:
  int16_t history_[op1rings::kScratchHalfbandHistory];   // last 24 inputs of the previous block (16-bit)
  int32_t mid_history_[3];                                // last three 44.1 kHz samples
};

// Rings voice states, and which one each OP-1 voice plays. A steal swaps them: the new OP-1 voice takes over the
// ringing state at no cost, and the stolen one fades out with a copy of only what it still needs
// (RingsVoice::CopyFrom).
// The voice states and the reverb are raw .bss, as kennel's were raw SDRAM: their FFloat members have a constructor
// (zero is m = 0 with the exponent -100000), and a coop plugin has no static constructors. The loader zeroes .bss,
// which FFloat reads as zero too (m = 0: the exponent is ignored), and Init sets every object up before any use.
alignas(RingsVoice) uint8_t states_memory[sizeof(RingsVoice) * op1::kNumVoices];
alignas(op1::ReverbBus) uint8_t reverb_memory[sizeof(op1::ReverbBus)];
#define states (reinterpret_cast<RingsVoice*>(states_memory))
#define reverb_bus (*reinterpret_cast<op1::ReverbBus*>(reverb_memory))   // string + reverb's, shared by the voices
RingsVoice* voices[op1::kNumVoices];
Upsampler upsamplers[op1::kNumVoices];
op1::ParamSmoother smoothers[op1::kNumVoices];
op1::VoiceAllocator allocator;
op1::CoefficientBudget coefficient_budget;   // the modal model's updates per block
static_assert(op1::ReverbBus::kBlock == kRingsBlockSize, "reverb bus: one block per call");
op1rings::Scratch scratch_fallback;          // when the core lends less L1 scratch than a Scratch

#if OP1RINGS_CONTROL_HALVED
// The control rate halved (the owner's choice, 2026-11-10: the device ran out of cycles): each voice does its control
// work (coefficients, cutoff, string parameters: src/op1_voice.h set_control_hold) every other OP-1 block, 2.9 ms
// apart instead of 1.45 ms, the voices offset so half of them do it in any block, and a strike always does. A block
// is told by the render order, as op1/coefficient_budget.h tells it: a voice rendered twice, or a lower index.
uint32_t clock_rendered;
int32_t clock_last;
uint32_t clock_block;
bool ControlBlock(int32_t v) {
  if (((clock_rendered >> v) & 1) || v < clock_last) {
    clock_rendered = 0;
    ++clock_block;
  }
  clock_rendered |= 1u << v;
  clock_last = v;
  return ((clock_block + static_cast<uint32_t>(v)) & 1) == 0;
}
#endif

// For the screen (rings_screen.cc): each voice's level (the block's peak, decaying) and its age in blocks since its
// note on. Written by the audio thread, read by the GUI thread: 16-bit words, no tearing.
volatile int16_t screen_level[op1::kNumVoices];
volatile uint16_t screen_age[op1::kNumVoices];
volatile uint8_t screen_released[op1::kNumVoices];   // key up: the stock envelope is closing

void ScreenQuiet(int32_t v) { screen_level[v] = static_cast<int16_t>(screen_level[v] - (screen_level[v] >> 2)); }

// The level is the block's peak before the stock envelope (applied after render), so a released voice reads loud
// until its ring dies: after the key up the level is let go by a quarter a block (~12 ms to a tenth) and the peak no
// longer tops it up, the way the heard note stops.
void ScreenRendered(int32_t v, const int16_t* out) {
  int32_t peak = 0;
  for (int32_t i = 0; i < op1::kBlockSize; ++i) {
    const int32_t a = out[i] < 0 ? -out[i] : out[i];
    if (a > peak) peak = a;
  }
  const int32_t level = screen_level[v];
  if (screen_released[v]) {
    screen_level[v] = static_cast<int16_t>(level - (level >> 2));
  } else {
    const int32_t held = level - (level >> 4);
    screen_level[v] = static_cast<int16_t>(peak > held ? peak : held);
  }
  if (screen_age[v] != 0xffff) screen_age[v] = static_cast<uint16_t>(screen_age[v] + 1);
}

// This call's scratch: the core's L1 loan when it is big enough, else the SDRAM fallback.
void BindScratch() {
  uint32_t bytes = 0;
  void* l1 = coop_l1_scratch(&bytes);
  op1rings::g_scratch = l1 && bytes >= sizeof(op1rings::Scratch) ? static_cast<op1rings::Scratch*>(l1)
                                                                   : &scratch_fallback;
}

inline int32_t ParamToQ31(int16_t p) { return p <= 0 ? 0 : static_cast<int32_t>(p) << 16; }

int Init() {
  // The limiter's soft-limit table into L1 (read every sample of every voice); refused: it stays in SDRAM.
  const uint32_t kTableBytes = sizeof(op1rings::kSoftLimitQ30);
  int32_t* table = static_cast<int32_t*>(coop_l1_lease(kTableBytes));
  if (table) {
    for (uint32_t i = 0; i < kTableBytes / 4; ++i) table[i] = op1rings::kSoftLimitQ30[i];
    kSoftLimitTable = table;
  }
  op1rings::g_scratch = &scratch_fallback;   // Init below may touch it: not inside a note_on or render yet
  for (int32_t v = 0; v < op1::kNumVoices; ++v) {
    voices[v] = &states[v];
    voices[v]->Init(op1::kPolyModes[op1::POLY_ALL]);
    upsamplers[v].Init();
    smoothers[v].Init();
  }
  allocator.Init();
  coefficient_budget.Init();
  reverb_bus.Init();
  return 0;
}

void Render(int voice, void* state, int16_t* out, const int32_t* pitch, const int16_t* params) {
  (void)state;
  const int32_t v = voice;
  if (v < 0 || v >= op1::kNumVoices) return;
  BindScratch();
  if (allocator.silent(v) ||
      (voices[v]->asleep() && !allocator.fading(v) &&
       voices[v]->model() != op1rings::MODEL_STRING_AND_REVERB)) {
    // Its note was taken over by another voice and has faded out, or its tail has gone quiet (the shared reverb's
    // carriers keep rendering: a share of the reverb plays through them).
    for (int32_t i = 0; i < op1::kBlockSize; ++i) out[i] = 0;
    upsamplers[v].Init();
    ScreenQuiet(v);
    return;
  }
  PROF_START(t_smooth);
  int32_t target[op1::ParamSmoother::kNumParams];
  for (int32_t i = 0; i < op1::ParamSmoother::kNumParams; ++i) target[i] = ParamToQ31(params[i]);
  const int32_t* smoothed = smoothers[v].Process(target);
  PROF_ADD(PROF_SMOOTH, t_smooth);
  VoicePatch patch;
  patch.structure = smoothed[0];
  patch.brightness = smoothed[1];
  patch.damping = smoothed[2];
  patch.position = smoothed[3];
  // The OP-1's increment is f / 88200 * 2^32; at a quarter of the rate it quadruples. Pitch is latched at the start
  // of the block (as the stock string engine does).
  const uint32_t inc = static_cast<uint32_t>(pitch[0]);
  const uint32_t rings_inc = inc < 0x40000000u ? inc << 2 : 0xffffffffu;
  // String + reverb sends to the shared reverb and plays a share of it (op1/reverb_bus.h); a voice fading out after a
  // steal does neither (the voice that took over carries the ring on).
  const bool carrier = voices[v]->model() == op1rings::MODEL_STRING_AND_REVERB && !allocator.fading(v);
  int32_t* out_buffer = op1rings::scratch()->render_out;
  int32_t* wet_share = op1rings::scratch()->wet_share;
  PROF_START(t_bus);
  const bool shared = reverb_bus.Render(v, op1::Cycles(), patch, carrier, wet_share);
  PROF_ADD(PROF_REVERB_BUS, t_bus);
  // The modal model recomputes its coefficients within a budget per block (op1/coefficient_budget.h); a voice fading
  // out after a steal keeps its own and asks for none.
  const bool fading = allocator.fading(v);
#if OP1RINGS_CONTROL_HALVED
  const bool control = ControlBlock(v) || voices[v]->striking();
  // A held block asks the budget for nothing: its grant waits for the voice's next control block.
  const bool allowed = control && coefficient_budget.Allow(v);
  voices[v]->set_control_hold(!control);
#else
  const bool allowed = coefficient_budget.Allow(v);
#endif
  voices[v]->set_coefficient_hold(fading || !allowed);
  PROF_START(t_voice);
  voices[v]->RenderMono(&rings_inc, patch, out_buffer, kRingsBlockSize,
                        carrier ? reverb_bus.send() : nullptr, shared ? wet_share : nullptr);
  PROF_ADD(PROF_VOICE, t_voice);
  const int32_t demand = voices[v]->TakeCoefficientDemand();
  coefficient_budget.Report(v, demand, fading);
  PROF_START(t_up);
  upsamplers[v].Process(out_buffer, out);
  PROF_ADD(PROF_UPSAMPLE, t_up);
  PROF_START(t_tail);
  allocator.Rendered(v);
  if (allocator.fading(v)) {
    // Linear fade-out (Q15 gain) of a voice whose note was taken over.
    int32_t gain = allocator.fade_gain(v);
    const int32_t step = allocator.fade_step(v);
    for (int32_t i = 0; i < op1::kBlockSize; ++i) {
      out[i] = static_cast<int16_t>((out[i] * (gain > 0 ? gain : 0)) >> 15);
      gain -= step;
    }
    allocator.AdvanceFade(v, op1::kBlockSize);
  }
  ScreenRendered(v, out);
  PROF_ADD(PROF_TAIL, t_tail);
}

void NoteOn(int voice, void* state, int32_t velocity, const int16_t* params) {
  (void)state;
  (void)velocity;
  const int32_t v = voice;
  if (v < 0 || v >= op1::kNumVoices) return;
  BindScratch();
  const int32_t model = op1::ModelSetting(params);
  op1::PolyState poly = op1::PolySetting(params);
  // String + reverb: "all" plays as Rings' polyphony 4, its maximum (op1/reverb_bus.h).
  if (model == op1rings::MODEL_STRING_AND_REVERB && poly == op1::POLY_ALL) poly = op1::POLY_4;

  const op1::VoiceAllocator::NoteOnAction action = allocator.NoteOn(v, poly);
  if (action.copy_from >= 0) {
    // Take over the stolen voice's ringing state; it fades out on its own.
    const int32_t u = action.copy_from;
    RingsVoice* ringing = voices[u];
    voices[u] = voices[v];
    voices[v] = ringing;
    // A stolen voice that never sounded goes silent at once (no fade), and is reset when struck again: no copy.
    if (!allocator.silent(u)) voices[u]->CopyFrom(*ringing);
    upsamplers[v] = upsamplers[action.copy_from];
    // The shared reverb's share goes with the ring.
    if (model == op1rings::MODEL_STRING_AND_REVERB) reverb_bus.Transfer(u, v);
  } else if (action.reset) {
    voices[v]->Init(op1::kPolyModes[poly]);
    upsamplers[v].Init();
  }
  voices[v]->set_resolution(op1::kPolyModes[poly]);
  voices[v]->set_polyphony(op1::kRingsPolyphony[poly]);
  voices[v]->set_model(static_cast<op1rings::ResonatorModel>(model));
  voices[v]->NoteOn();
  smoothers[v].Snap();
  screen_age[v] = 0;
  screen_released[v] = 0;
}

// Only tells the screen: the stock envelope does the release.
void NoteOff(int voice, void* state) {
  (void)state;
  if (voice >= 0 && voice < op1::kNumVoices) screen_released[voice] = 1;
}

}  // namespace

// The screen's view of the voices (GUI thread): level = the last block's peak, decaying; age = blocks since the note
// on (saturating).
extern "C" void RingsScreenVoices(int16_t* level, uint16_t* age) {
  for (int32_t v = 0; v < op1::kNumVoices; ++v) {
    level[v] = screen_level[v];
    age[v] = screen_age[v];
  }
}

extern "C" const coop_screen_t SCREEN_rings;            // rings_screen.cc
extern "C" const coop_engine_t ENGINE_rings = {
  COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 0, Init, NoteOn, Render, NoteOff, &SCREEN_rings,
};
