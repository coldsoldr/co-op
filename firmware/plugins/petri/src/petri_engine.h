// petri's engine at 22.05 kHz: a continuously recorded stereo buffer, up to eight voices that play it back (each a
// grain or loop player: start, rate and direction, length, envelope, gain, pan), eleven effects that schedule the
// voices once a block, then the shared tone section on the wet path (the filter, the space). The plugin glue
// (petri.cc) runs it between the halfband decimator and interpolator (src/half_rate.h) and mixes the result with the
// untouched dry input.
//
// Fixed point only. The per-sample loops are the voices (RenderVoice), the record write, the filter and the space;
// everything a scheduler decides (where a grain starts, its rate, its envelope step) is worked out once a block.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#ifndef COOP_PETRI_ENGINE_H_
#define COOP_PETRI_ENGINE_H_

#include <stdint.h>

namespace petri {

const int32_t kN = 32;                  // samples a call at 22.05 kHz (the 64-sample bus block, 2:1)
const int32_t kRecBits = 15;
const int32_t kRecFrames = 1 << kRecBits;   // the span of a position: frame numbers count modulo 32,768
const int32_t kRecMask = kRecFrames - 1;
// The record itself is a ring of kRecLen frames (0.2: 1.363 s at 22.05 kHz, was the whole 32,768; sized so the state
// fits tape_echo's 134,784-byte position buffer). A frame `age` behind the write head is at (hp - age) mod kRecLen;
// the first kRecGuard frames are mirrored after the ring's end, so a voice's 32 samples never wrap inside the kernel.
#ifndef COOP_PETRI_REC_LEN
#define COOP_PETRI_REC_LEN 30048            // a multiple of kN; the host test also builds the old 32,768
#endif
const int32_t kRecLen = COOP_PETRI_REC_LEN;
const int32_t kRecGuard = 288;              // a multiple of kN, over a call's reach at kRateMax (8 x 32 + 2 frames)
const int32_t kRecWords = kRecLen + kRecGuard;
const int32_t kPosShift = 32 - kRecBits;    // a voice's position: frames in the top 15 bits, the fraction below (Q17)
const int32_t kRateOne = 1 << kPosShift;    // a rate of 1 (Q17 frames a sample)
const int32_t kRateMax = 8 * kRateOne;      // a voice's rate is held to this (the effects reach 4x; kRecGuard)
const int32_t kRebase = kRecFrames / 2;     // the frame number the kernel is given: a call never wraps its 15 bits
const int32_t kVoices = 8;
const int32_t kBuses = 3;                   // voices may sum into a filtered bus (warp)
// The space's lines (at 11.025 kHz) in one int16 buffer read through a pointer that steps down one a sample: every
// access is the pointer plus a constant, no wrap. The pointer starts at kSpaceSlide; when it nears the bottom the
// kSpaceSpan live entries are copied up by kSpaceSlide (once every 64 calls; 0.2: 1024, was 4096 every 256 calls).
const int32_t kSpaceSpan = 3968;            // the lines' entries (petri_engine.cc checks they fit)
const int32_t kSpaceSlide = 1024;           // a multiple of kN / 2
const int32_t kSpaceSize = kSpaceSlide + kSpaceSpan + 16;
const int32_t kForever = 0x7fffffff;        // a voice's blocks: a tap that plays until it is faded out
const int32_t kFadeBlocks = 8;              // a voice faded out (killed) over 8 calls (11.6 ms)
const int32_t kMargin = 96;                 // frames a read keeps away from the write head

enum Effect {
  FX_MOSAIC, FX_SEQ, FX_GLIDE, FX_HAZE, FX_TUNNEL, FX_STRUM, FX_BLOCKS, FX_CUT, FX_ARP, FX_PATTERN, FX_WARP,
  kNumEffects
};

enum Shape { SH_HANN, SH_TRAP, SH_GATE, SH_PERC, SH_SWELL, SH_FLAT, kNumShapes };

struct Controls {
  int32_t time, activity, repeats;      // the turns, 0..32767 (mix is the glue's)
  int32_t filter, space;                // words 6, 7: 0..32767
  int32_t effect;                       // word 5: 0..10
  bool hold, reverse;                   // the latches
};

struct Voice {
  uint32_t pos;       // Q17 frames in the buffer (wraps with the uint32)
  int32_t rate;       // Q17 frames a sample, signed (negative: backwards)
  int32_t slope;      // added to the rate once a block (glide)
  int32_t blocks;     // calls left; 0 idle; kForever a tap
  uint32_t ephase;    // the envelope's phase, Q24 over the grain
  uint32_t einc;      // its step a call
  int32_t shape;
  int32_t gain;       // Q15
  int32_t pl, pr;     // the pan's gains, Q15
  int32_t gl, gr;     // the gains reached at the end of the last call (Q15): the next call ramps from them
  int32_t fade;       // calls left of a fade-out, 0 none
  int32_t bus;        // 0: the wet sum; 1..3 a filtered bus (warp's taps: a one-pole low-pass each)
  int32_t tag;        // the scheduler's mark (pattern: 1 a continuous tap)
  int32_t mod;        // the pitch modulation's sign for this voice (+1, -1)
  int32_t level;      // the last call's envelope x gain (Q15): the display
};

// What the schedulers keep (zeroed on an effect change).
struct Sched {
  int32_t timer, step, count, phase, dir, last_onsets;
  int32_t n;                // layers, notes or taps in use
  uint32_t grab[8];         // a layer's slice (first frame) or a note's start
  int32_t slice[8];         // its frames
  int32_t gain[8];          // Q15
  int32_t aux[8];
  int32_t order[8];
  int32_t flags[8];
};

struct Space {
  int16_t buf[kSpaceSize];
  int32_t q;                // the read / write pointer's index (decrements; kSpaceSlide after a reset)
  int32_t lpa, lpb;         // lpa: the tank's two damping states (packed: right high, left low); lpb unused
  int32_t amount;           // the smoothed knob, Q15
  int32_t ya, yb;           // the last outputs (the half-rate space's interpolation)
};

struct Filter {
  int32_t ic1[2], ic2[2];   // the TPT SVF's states, per channel (the signal << 13)
  int32_t knob;             // the smoothed knob, Q15 << 8
};

struct Engine {
  uint32_t rec[kRecWords];        // L in the low half, R in the high half; [kRecLen + i] mirrors [i], i < kRecGuard
  Space space;
  Voice v[kVoices];
  Sched s;
  Filter filt;
  uint32_t head;                  // the next frame written (frames, counting; positions are head & kRecMask)
  int32_t hp;                     // where it is written: head mod kRecLen
  int32_t effect;                 // the effect in force
  uint32_t rng;
  uint32_t lfo;                   // the pitch modulation's phase
  int32_t fb;                     // the feedback into the record (Q15) the current effect asked for
  int32_t makeup;                 // the wet's gain before the tone section (Q12)
  int32_t dcx[2], dcy[2];         // the feedback path's DC blocker
  int32_t rec_in;                 // the record's fade-in after a restart (Q15, 32767 = done)
  // the input follower and onset detector
  int32_t env_fast, env_slow, refractory, quiet;
  uint32_t onsets[8];             // frames of the last onsets (a ring)
  int32_t onset_count;            // onsets seen (the ring's write count)
  int32_t level;                  // the input level (0..32767): the display
  bool hold, reverse;
  int32_t wl[kN], wr[kN];         // the wet sum a call (int32: the voices add here)
  int32_t bus[kBuses][2][kN];     // the filtered buses' sums a call
  int32_t bus_c[kBuses];          // their one-pole coefficients (Q15)
  int32_t bus_z[kBuses][2];       // their states
};

// Reset everything but the two large buffers (cleared by ClearStep over the next calls).
void Reset(Engine* e);
// Clear the next `words` words of the record and the space from `at` (0 = start); returns the next, or kClearEnd.
const int32_t kClearEnd = kRecWords + kSpaceSize / 2;
int32_t ClearStep(Engine* e, int32_t at, int32_t words);
// One call: in[2][kN] the decimated input, out[2][kN] the wet (int16).
void Process(Engine* e, const int16_t* in_l, const int16_t* in_r, int16_t* out_l, int16_t* out_r, const Controls& c);
// 0.3, the idle path: true when the last Process (hold off) wrote kN frames of zeros to the record.
bool WroteSilence(const Engine* e);
// 0.3, the idle path (../petri.cc): a call on a record of zeros, with a silent input (zero_l, zero_r: kN zeros) and
// hold off. Everything Process does but the samples: the detector, the effect's scheduler, the voices' positions,
// envelopes and ends, the write head, the two smoothed tone controls; the voices' kernel, the record's write, the
// filter and the space are not run (their inputs are zeros), and there is no output (it is silence).
void ProcessIdle(Engine* e, const int16_t* zero_l, const int16_t* zero_r, const Controls& c);
// True when warp's three filtered buses are at rest (their states 0: a bus voice then adds nothing to a silent sum).
bool BusAtRest(const Engine* e);

// The grain envelope (exposed for the host test): shape at phase Q24 (0..1 << 24), Q15.
int32_t Envelope(int32_t shape, uint32_t phase_q24);
// 2^x, x in Q16 octaves (-14..14), Q16.
uint32_t Exp2Q16(int32_t x_q16);

}  // namespace petri

// The voice kernel (src/voice_bfin.S) and its C reference (petri_engine.cc): 32 samples of one voice into wl, wr.
extern "C" void PetriVoiceAsm(const uint32_t* rec, uint32_t pos, int32_t rate, int32_t* wl, int32_t* wr, uint32_t gains,
                              uint32_t dgains);
// The space kernel (src/space_bfin.S) and its C reference: 16 samples of the tank at 11.025 kHz.
extern "C" void PetriSpaceAsm(int16_t* z, const int16_t* x, uint32_t* y, uint32_t* lp, uint32_t decay);
extern "C" void PetriSpaceC(int16_t* z, const int16_t* x, uint32_t* y, uint32_t* lp, uint32_t decay);
extern "C" void PetriVoiceC(const uint32_t* rec, uint32_t pos, int32_t rate, int32_t* wl, int32_t* wr, uint32_t gains,
                            uint32_t dgains);

namespace petri {

}  // namespace petri

#endif  // COOP_PETRI_ENGINE_H_
