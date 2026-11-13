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

// What the L1 lease got at init (rings 0.4), one bit each: 1 the engine's state (Hot), 2 the modal modes, 4 the
// soft-limit table. For the tests.
extern "C" { uint32_t g_rings_l1; }
enum { L1_HOT = 1, L1_MODES = 2, L1_TABLE = 4 };

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

// 0.4: OP1RINGS_VOICE_44K renders the voice at 44.1 kHz (COOP_ENGINE_F_44K, docs/engines.md "The 44.1 kHz voice"):
// out[0..63] are the half-band's outputs, the even samples of the 88.2 kHz build, and the core repeats each one. The
// Lagrange 2x stage is gone; the fade and the screen's peak run over the 64 samples.
#ifndef OP1RINGS_VOICE_44K
#define OP1RINGS_VOICE_44K 0
#endif
#if OP1RINGS_VOICE_44K
const int32_t kOutSamples = COOP_VOICE_BLOCK_44K;                      // 64 at 44.1 kHz
const int32_t kOutStep = 2;                                             // 88.2 kHz samples a written sample
#else
const int32_t kOutSamples = op1::kBlockSize;                            // 128 at 88.2 kHz
const int32_t kOutStep = 1;
#endif
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
#if OP1RINGS_VOICE_44K
    for (int32_t k = 0; k < 2 * kRingsBlockSize; ++k) out[k] = op1::Saturate16(mid[k + 1]);
#else
    for (int32_t k = 0; k < 2 * kRingsBlockSize; ++k) {
      out[2 * k] = op1::Saturate16(mid[k + 1]);
      out[2 * k + 1] = op1::Saturate16((9 * (mid[k + 1] + mid[k + 2]) - mid[k] - mid[k + 3]) >> 4);
    }
#endif
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
#if defined(COPA_OPUS)
// rings 0.9, as an Opus 2 engine (op1-plugins' adapter builds its members with COPA_OPUS): Opus loads a plugin's
// code and zero-filled data into whichever pool has room, and on an OP-1 (2026-11-14) rings' landed in the far pool,
// which the display shares and which costs about twice as much while it sends a frame: one note of sympathetic
// strings took a whole audio block. The voice states and the reverb (~572 KB, read and written every sample) come
// from the audio pool instead (copa_alloc_audio: opus_alloc_audio, else opus_alloc; zeroed, as .bss is), at Init.
extern "C" void* copa_alloc_audio(uint32_t bytes);
uint8_t* states_memory;
uint8_t* reverb_memory;
#else
alignas(RingsVoice) uint8_t states_memory[sizeof(RingsVoice) * op1::kNumVoices];
alignas(op1::ReverbBus) uint8_t reverb_memory[sizeof(op1::ReverbBus)];
#endif
#define states (reinterpret_cast<RingsVoice*>(states_memory))
#define reverb_bus (*reinterpret_cast<op1::ReverbBus*>(reverb_memory))   // string + reverb's, shared by the voices
// rings 0.4: the engine's own state that every render writes (the upsamplers' history, the smoothed knobs, the voice
// allocator, the batch's records, the block clock, the screen's levels) in one block, leased in L1 at init (`Hot`
// below); `hot_fallback` in .bss when the lease is refused. On the device each SDRAM line the render writes is a
// cache miss now and a write-back later, beside the stock's own data (plugins/rings/README.md "m1").
op1::CoefficientBudget coefficient_budget;   // the modal model's updates per block (not used by the batch)
static_assert(op1::ReverbBus::kBlock == kRingsBlockSize, "reverb bus: one block per call");
op1rings::Scratch scratch_fallback;          // when the core lends less L1 scratch than a Scratch

// OP1RINGS_COOP_POLY4 (rings 0.3, the owner's choice after the 0.2 device run): the polyphony setting 0 ("all" in
// te-op1-cfw: six voices of 14 modes, each its own resonator) plays as Rings' own polyphony 4 (four voices, the
// oldest stolen and faded), with 10 modes a modal voice instead of Rings' 12. Polyphony 1 and 2 are Rings'.
// OP1RINGS_COOP_POLY_ALL (rings 0.5, the owner's choice): "all" is back as its own setting, six voices of 14 modes
// each (te-op1-cfw's); polyphony 4 keeps 0.3's 10 modes. The steps are 1, 2, 4, all (op1/settings.h kCoopPolyOrder).
#if OP1RINGS_COOP_POLY4
const int32_t kPoly4Modes = 10;
#if OP1RINGS_COOP_POLY_ALL
inline op1::PolyState PlayedPoly(op1::PolyState poly) { return poly; }
#else
inline op1::PolyState PlayedPoly(op1::PolyState poly) { return poly == op1::POLY_ALL ? op1::POLY_4 : poly; }
#endif
inline int32_t PolyModes(op1::PolyState poly) { return poly == op1::POLY_4 ? kPoly4Modes : op1::kPolyModes[poly]; }
#else
inline op1::PolyState PlayedPoly(op1::PolyState poly) { return poly; }
inline int32_t PolyModes(op1::PolyState poly) { return op1::kPolyModes[poly]; }
#endif

struct Hot {
  RingsVoice* voices[op1::kNumVoices];
  Upsampler upsamplers[op1::kNumVoices];
  op1::ParamSmoother smoothers[op1::kNumVoices];
  op1::VoiceAllocator allocator;
  uint32_t clock_rendered;   // the halved schedule's block clock (NewBlock)
  int32_t clock_last;
  uint32_t clock_block;
  uint32_t last_inc[op1::kNumVoices];   // the batch's records (Batch)
  VoicePatch last_patch[op1::kNumVoices];
  uint32_t batched_block[op1::kNumVoices];
  bool have_last[op1::kNumVoices];
  // For the screen (rings_screen.cc): each voice's level (the block's peak, decaying) and its age in blocks since its
  // note on. Written by the audio thread, read by the GUI thread: 16-bit words, no tearing.
  volatile int16_t screen_level[op1::kNumVoices];
  volatile uint16_t screen_age[op1::kNumVoices];
  volatile uint8_t screen_released[op1::kNumVoices];   // key up: the stock envelope is closing
};
Hot hot_fallback;
Hot* hot = &hot_fallback;

// rings 0.6: each voice's model and polyphony as it was struck (Setting; -1 none), and the setting the sound has now.
// When the setting changes, every voice struck with another one is cut off at once (allocator.Forget: silent, out of
// the voice list, reset when next struck). Left ringing, the old setting's voices kept their cost (a polyphony-1 voice
// is 60 modes) beside the new setting's six, and on an Opus 2 OP-1 that overran the audio block (2026-11-13).
int8_t struck_setting[op1::kNumVoices] = { -1, -1, -1, -1, -1, -1 };
int8_t current_setting = -1;
#define voices (hot->voices)
#define upsamplers (hot->upsamplers)
#define smoothers (hot->smoothers)
#define allocator (hot->allocator)
#define clock_rendered (hot->clock_rendered)
#define clock_last (hot->clock_last)
#define clock_block (hot->clock_block)
#define last_inc (hot->last_inc)
#define last_patch (hot->last_patch)
#define batched_block (hot->batched_block)
#define have_last (hot->have_last)
#define screen_level (hot->screen_level)
#define screen_age (hot->screen_age)
#define screen_released (hot->screen_released)

// rings 0.4: the modal model's mode state (op1rings::Mode: written every sample) in L1 for the first kL1Modes modes of
// each voice state, the polyphony-4 voice's 10 (ModalResonator::set_l1_bank); polyphony 1 and 2 (60 and 28 modes) keep
// theirs in the voice state. Leased at init after Hot; refused: every voice keeps its own. The host test builds it
// with 14 (-DOP1RINGS_L1_MODES=14), the te-op1-cfw build's "all", so the moves across it are checked bit for bit.
#ifndef OP1RINGS_L1_MODES
#define OP1RINGS_L1_MODES 10
#endif
const int32_t kL1Modes = OP1RINGS_L1_MODES;

#if OP1RINGS_CONTROL_HALVED
// The control rate halved (the owner's choice, 2026-11-10: the device ran out of cycles): each voice does its control
// work (coefficients, cutoff, string parameters: src/op1_voice.h set_control_hold) every other OP-1 block, 2.9 ms
// apart instead of 1.45 ms, the voices offset so half of them do it in any block, and a strike always does. A block
// is told by the render order, as op1/coefficient_budget.h tells it: a voice rendered twice, or a lower index.
// Before voice v renders: true when it starts a new OP-1 block.
bool NewBlock(int32_t v) {
  const bool fresh = ((clock_rendered >> v) & 1) || v < clock_last;
  if (fresh) {
    clock_rendered = 0;
    ++clock_block;
  }
  clock_rendered |= 1u << v;
  clock_last = v;
  return fresh;
}
inline bool Turn(int32_t v) { return ((clock_block + static_cast<uint32_t>(v)) & 1) == 0; }
#endif

#if OP1RINGS_CONTROL_BATCHED
static_assert(OP1RINGS_CONTROL_HALVED, "the batch runs the halved schedule");
// The control work batched (rings 0.3, the owner's choice): at a block's first render, the control work of every
// voice whose turn it is (Turn) runs back to back, with each voice's pitch and smoothed knobs from its last block,
// so that code is fetched once a block instead of once a voice, and the renders that follow run the audio path
// only (held). A voice that strikes, fades, sleeps or has no last block does its own control in its render, as in
// the halved schedule. The modal coefficient budget is not used: at most two voices of 10 modes, or one of 60
// (polyphony 1, which the budget always let through), update in a block.
// last_inc, last_patch, have_last (Hot): each voice's pitch and smoothed knobs from its last block; batched_block: the
// block whose control the batch did for the voice (+ 1; 0 = none).

void Batch() {
  for (int32_t u = 0; u < op1::kNumVoices; ++u) {
    if (!Turn(u) || !have_last[u] || allocator.silent(u) || allocator.fading(u) || voices[u]->striking() ||
        voices[u]->asleep()) {
      continue;
    }
    voices[u]->Control(last_inc[u], last_patch[u]);
    batched_block[u] = clock_block + 1;
  }
}
#endif

void ScreenQuiet(int32_t v) { screen_level[v] = static_cast<int16_t>(screen_level[v] - (screen_level[v] >> 2)); }

// The level is the block's peak before the stock envelope (applied after render), so a released voice reads loud
// until its ring dies: after the key up the level is let go by a quarter a block (~12 ms to a tenth) and the peak no
// longer tops it up, the way the heard note stops.
void ScreenRendered(int32_t v, const int16_t* out) {
  int32_t peak = 0;
  for (int32_t i = 0; i < kOutSamples; ++i) {
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

// The core lends 1312 bytes (sdk/coop_plugin.h coop_l1_scratch): a bigger Scratch would silently fall back to SDRAM.
static_assert(sizeof(op1rings::Scratch) <= 1312, "the L1 scratch loan");
// This call's scratch: the core's L1 loan when it is big enough, else the SDRAM fallback.
void BindScratch() {
  uint32_t bytes = 0;
  void* l1 = coop_l1_scratch(&bytes);
  op1rings::g_scratch = l1 && bytes >= sizeof(op1rings::Scratch) ? static_cast<op1rings::Scratch*>(l1)
                                                                   : &scratch_fallback;
}

inline int32_t ParamToQ31(int16_t p) { return p <= 0 ? 0 : static_cast<int32_t>(p) << 16; }

int Init() {
#if defined(COPA_OPUS)
  if (!states_memory) states_memory = static_cast<uint8_t*>(copa_alloc_audio(sizeof(RingsVoice) * op1::kNumVoices));
  if (!reverb_memory) reverb_memory = static_cast<uint8_t*>(copa_alloc_audio(sizeof(op1::ReverbBus)));
  if (!states_memory || !reverb_memory) return -1;   // no room: the file is refused, as coop refuses it
#endif
  // Into L1, most written first (each refused part stays in SDRAM, the output the same): the engine's per-render state
  // (Hot, ~0.8 KB), the modal voices' modes (6 x 10 x 24 bytes), the limiter's soft-limit table (1,028 bytes, read
  // every sample of every voice). Engines share 4,096 bytes of lease (docs/engines.md "L1"); this takes ~3.3 KB and
  // leaves saw its 192.
  g_rings_l1 = 0;
  if (Hot* l1 = static_cast<Hot*>(coop_l1_lease(sizeof(Hot)))) {   // zeroed, as hot_fallback is
    hot = l1;
    g_rings_l1 |= L1_HOT;
  }
  op1rings::Mode* slabs =
      static_cast<op1rings::Mode*>(coop_l1_lease(sizeof(op1rings::Mode) * kL1Modes * op1::kNumVoices));
  if (slabs) g_rings_l1 |= L1_MODES;
  const uint32_t kTableBytes = sizeof(op1rings::kSoftLimitQ30);
  int32_t* table = static_cast<int32_t*>(coop_l1_lease(kTableBytes));
  if (table) {
    for (uint32_t i = 0; i < kTableBytes / 4; ++i) table[i] = op1rings::kSoftLimitQ30[i];
    kSoftLimitTable = table;
    g_rings_l1 |= L1_TABLE;
  }
  op1rings::g_scratch = &scratch_fallback;   // Init below may touch it: not inside a note_on or render yet
  for (int32_t v = 0; v < op1::kNumVoices; ++v) {
    voices[v] = &states[v];
    voices[v]->Init(PolyModes(PlayedPoly(op1::POLY_ALL)));
    if (slabs) voices[v]->set_l1_modes(slabs + v * kL1Modes, kL1Modes);
    upsamplers[v].Init();
    smoothers[v].Init();
  }
  allocator.Init();
  coefficient_budget.Init();
  reverb_bus.Init();
  return 0;
}

// The model and polyphony words as one number.
inline int8_t Setting(const int16_t* params) {
  return static_cast<int8_t>(op1::ModelSetting(params) * op1::kNumPolyStates + op1::PolySetting(params));
}

// A new model or polyphony: the voices struck with another setting stop now (rings 0.6, above).
void FollowSetting(const int16_t* params) {
  const int8_t setting = Setting(params);
  if (setting == current_setting) return;
  current_setting = setting;
  for (int32_t u = 0; u < op1::kNumVoices; ++u) {
    if (struck_setting[u] != setting && !allocator.silent(u)) allocator.Forget(u);
  }
}

void Render(int voice, void* state, int16_t* out, const int32_t* pitch, const int16_t* params) {
  (void)state;
  const int32_t v = voice;
  if (v < 0 || v >= op1::kNumVoices) return;
  BindScratch();
  FollowSetting(params);
#if OP1RINGS_CONTROL_HALVED
  // The block clock ticks on every render, silent voices too (the order tells the blocks apart); a new block's
  // first render runs the batch.
#if OP1RINGS_CONTROL_BATCHED
  if (NewBlock(v)) {
    PROF_START(t_batch);
    Batch();
    PROF_ADD(PROF_BATCH, t_batch);
  }
#else
  NewBlock(v);
#endif
#endif
  if (allocator.silent(v) ||
      (voices[v]->asleep() && !allocator.fading(v) &&
       voices[v]->model() != op1rings::MODEL_STRING_AND_REVERB)) {
    // Its note was taken over by another voice and has faded out, or its tail has gone quiet (the shared reverb's
    // carriers keep rendering: a share of the reverb plays through them).
    for (int32_t i = 0; i < kOutSamples; ++i) out[i] = 0;
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
#if OP1RINGS_CONTROL_BATCHED
  // The batch did this voice's control already (or it isn't its turn): hold. A strike, or a turn the batch couldn't
  // take, does its own. No coefficient budget (above).
  const bool control = voices[v]->striking() || (Turn(v) && batched_block[v] != clock_block + 1);
  const bool allowed = control;
  voices[v]->set_control_hold(!control);
#elif OP1RINGS_CONTROL_HALVED
  const bool control = Turn(v) || voices[v]->striking();
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
#if OP1RINGS_CONTROL_BATCHED
  (void)demand;
  last_inc[v] = rings_inc;
  last_patch[v] = patch;
  have_last[v] = true;
#else
  coefficient_budget.Report(v, demand, fading);
#endif
  PROF_START(t_up);
  upsamplers[v].Process(out_buffer, out);
  PROF_ADD(PROF_UPSAMPLE, t_up);
  PROF_START(t_tail);
  allocator.Rendered(v);
  if (allocator.fading(v)) {
    // Linear fade-out (Q15 gain) of a voice whose note was taken over.
    int32_t gain = allocator.fade_gain(v);
    const int32_t step = allocator.fade_step(v) * kOutStep;            // the step stays per 88.2 kHz sample
    for (int32_t i = 0; i < kOutSamples; ++i) {
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
  FollowSetting(params);
  struck_setting[v] = current_setting;
  const int32_t model = op1::ModelSetting(params);
  op1::PolyState poly = PlayedPoly(op1::PolySetting(params));
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
    voices[v]->Init(PolyModes(poly));
    upsamplers[v].Init();
  }
  voices[v]->set_resolution(PolyModes(poly));
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
  COOP_ENGINE_MAGIC, COOP_PLUGIN_API, OP1RINGS_VOICE_44K ? COOP_ENGINE_F_44K : 0u, 0, Init, NoteOn, Render, NoteOff, &SCREEN_rings,
};
