// petri's engine (src/petri_engine.h): the record, the voices, the eleven schedulers, the filter and the space.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include "src/petri_engine.h"

#include "src/half_rate.h"      // petri_hb::MulQ31Fast
#include "src/petri_tables.h"
#include "src/space_layout.h"
#include "coop_fx_screen.h"     // coop_scr::SinQ15 (integer only, header only)

namespace petri {

namespace {

inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int32_t Sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
inline int32_t Abs(int32_t v) { return v < 0 ? -v : v; }
// (x g) >> 15 truncated toward zero: in a feedback loop a floor's bias holds -1 forever (a limit cycle, a stuck DC).
inline int32_t MulZ(int32_t x, int32_t g) {
  const int32_t p = x * g;
  return (p + ((p >> 31) & 0x7fff)) >> 15;
}
inline int32_t MulZ16(int16_t x, int16_t g) {         // the same, one 16 x 16 product
  const int32_t p = static_cast<int32_t>(x) * g;
  return (p + ((p >> 31) & 0x7fff)) >> 15;
}

inline uint32_t Rand(Engine* e) {
  uint32_t x = e->rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  e->rng = x;
  return x;
}
inline int32_t RandQ15(Engine* e) { return static_cast<int32_t>(Rand(e) >> 17); }                // 0..32767
inline int32_t RandS15(Engine* e) { return static_cast<int32_t>(Rand(e) >> 16) - 32768; }        // -32768..32767
inline bool Chance(Engine* e, int32_t p_q15) { return RandQ15(e) < p_q15; }

// min_frames x 2^(octaves x knob), octaves in Q8: a turn's time, once a block.
inline int32_t KnobFrames(int32_t knob, int32_t min_frames, int32_t oct_q8) {
  const int32_t x = (Clamp(knob, 0, 32767) * oct_q8) >> 7;                 // Q16 octaves
  return static_cast<int32_t>((static_cast<uint32_t>(min_frames) * (Exp2Q16(x) >> 4)) >> 12);
}

// A rate (Q17) for semitones in Q8.
inline int32_t SemiRate(int32_t semi_q8) { return static_cast<int32_t>(Exp2Q16((semi_q8 * 21845) >> 10) << 1); }

// Calls a grain of `frames` of material at `rate` takes.
inline int32_t BlocksFor(int32_t frames, int32_t rate) {
  const int32_t r = Abs(rate);
  if (r < 1024) return 1;
  const int32_t b = static_cast<int32_t>((static_cast<uint32_t>(frames) << 12) / static_cast<uint32_t>(r));
  return b < 1 ? 1 : b;
}

// Frames of material `blocks` calls at `rate` read.
inline int32_t MaterialFor(int32_t blocks, int32_t rate) { return ((Abs(rate) >> 5) * blocks) >> 7; }

inline bool Idle(const Voice& v) { return v.blocks == 0 && v.fade == 0; }

inline void Kill(Voice& v) {
  if (v.blocks > 0 && v.fade == 0) v.fade = kFadeBlocks;
}

void KillAll(Engine* e) {
  for (int32_t i = 0; i < kVoices; ++i) Kill(e->v[i]);
}

// A read of `blocks` calls from `start` that the write head will not overwrite first (always, while holding).
inline bool Safe(const Engine* e, uint32_t start, int32_t blocks) {
  if (e->hold) return true;
  const uint32_t age = e->head - start;
  return age < static_cast<uint32_t>(kRecFrames) &&
         age + static_cast<uint32_t>(blocks) * kN + kMargin < static_cast<uint32_t>(kRecFrames);
}

void Spawn(Engine* e, int32_t vi, uint32_t start, int32_t rate, int32_t blocks, int32_t shape, int32_t gain,
           int32_t pan, bool flip = false, int32_t slope = 0, int32_t bus = 0) {
  Voice& v = e->v[vi];
  if (blocks < 1) blocks = 1;
  if (blocks != kForever) {
    const int32_t r1 = rate + slope * blocks;
    const int32_t material = (((Abs(rate) + Abs(r1)) >> 6) * blocks) >> 7;
    // Never past the write head: held, the slice ends before it; recording, the read stays behind it from the
    // start to the end (both move in straight lines, so the two ends are enough).
    int32_t late = -kMargin;                                            // the latest start, relative to the head
    if (!e->hold && e->reverse == flip) {                               // (backwards, the read starts at the end)
      const int32_t end = blocks * kN - material - kMargin;
      if (end < late) late = end;
    } else {
      late = -material - kMargin;
    }
    if (static_cast<int32_t>(start - e->head) > late) start = e->head + static_cast<uint32_t>(late);
    if (e->reverse != flip) {                                           // backwards: from the slice's end
      start += static_cast<uint32_t>(material);
      rate = -rate;
      slope = -slope;
    }
  }
  v.pos = start << kPosShift;
  v.rate = rate;
  v.slope = slope;
  v.blocks = blocks;
  v.ephase = 0;
  v.einc = blocks == kForever ? 0 : (1u << 24) / static_cast<uint32_t>(blocks);
  v.shape = shape;
  v.gain = Clamp(gain, 0, 32767);
  pan = Clamp(pan, -32767, 32767);
  v.pl = pan > 0 ? 32767 - pan : 32767;
  v.pr = pan < 0 ? 32767 + pan : 32767;
  v.gl = v.gr = 0;
  v.fade = 0;
  v.bus = bus;
  v.tag = 0;
  v.mod = (vi & 1) ? -1 : 1;
}

// An idle voice in lo..hi, else the one nearest its end that is not fading; -1 when all are fading.
int32_t PickVoice(const Engine* e, int32_t lo, int32_t hi) {
  int32_t best = -1, best_blocks = kForever;
  for (int32_t i = lo; i <= hi; ++i) {
    const Voice& v = e->v[i];
    if (Idle(v)) return i;
    if (v.fade == 0 && v.blocks < best_blocks) {
      best = i;
      best_blocks = v.blocks;
    }
  }
  return best;
}

// The last onsets still readable for `blocks` calls, oldest first, at most `max`; the count.
int32_t RecentOnsets(const Engine* e, uint32_t* out, int32_t max, int32_t blocks) {
  int32_t avail = e->onset_count < 8 ? e->onset_count : 8;
  if (avail > max) avail = max;
  int32_t n = 0;
  for (int32_t k = avail - 1; k >= 0; --k) {   // oldest of the `avail` most recent first
    const uint32_t f = e->onsets[(e->onset_count - 1 - k) & 7];
    if (Safe(e, f, blocks) && (e->head - f) < static_cast<uint32_t>(kRecFrames)) out[n++] = f;
  }
  return n;
}

// ---- the voices ----

}  // namespace

// The kernels' C references: the host build runs them; on the Blackfin only a test build (COOP_PETRI_REF, the
// simulator's cross-check in tests/host/petri) or COOP_PETRI_C links them, the plugin runs the .S kernels.
#if !defined(__bfin__) || defined(COOP_PETRI_C) || defined(COOP_PETRI_REF)
// The voice kernel's C reference (src/voice_bfin.S PetriVoiceAsm, bit for bit): 32 samples of one voice into wl, wr.
extern "C" void PetriVoiceC(const uint32_t* rec, uint32_t pos, int32_t rate, int32_t* wl, int32_t* wr, uint32_t gains,
                            uint32_t dgains) {
  int16_t gl = static_cast<int16_t>(gains), gr = static_cast<int16_t>(gains >> 16);
  const int16_t dl = static_cast<int16_t>(dgains), dr = static_cast<int16_t>(dgains >> 16);
  for (int32_t i = 0; i < kN; ++i) {
    const uint32_t idx = pos >> kPosShift;
    const int32_t frac = static_cast<int32_t>((pos >> 2) & 0x7fff);
    const uint32_t a = rec[idx], b = rec[idx + 1];
    const int16_t al = static_cast<int16_t>(a), ar = static_cast<int16_t>(a >> 16);
    const int16_t bl = static_cast<int16_t>(b), br = static_cast<int16_t>(b >> 16);
    const int16_t hl = static_cast<int16_t>((bl >> 1) - (al >> 1)), hr = static_cast<int16_t>((br >> 1) - (ar >> 1));
    const int16_t tl = static_cast<int16_t>((hl * frac) >> 15), tr = static_cast<int16_t>((hr * frac) >> 15);   // (T)
    const int16_t l = static_cast<int16_t>(al + static_cast<int16_t>(tl + tl));
    const int16_t r = static_cast<int16_t>(ar + static_cast<int16_t>(tr + tr));
    wl[i] += (l * gl) >> 15;
    wr[i] += (r * gr) >> 15;
    pos += static_cast<uint32_t>(rate);
    gl = static_cast<int16_t>(gl + dl);
    gr = static_cast<int16_t>(gr + dr);
  }
}

// The space kernel's C reference (src/space_bfin.S PetriSpaceAsm, bit for bit): the Blackfin's fractional MACs (each
// product 2 a b, summed in the accumulator) and their extraction (rounded at bit 16, ties to even: ASTAT RND_MOD 0,
// the default; saturated to 16 bits).
namespace {
inline int16_t Ext(int64_t acc) {
  int64_t r = acc >> 16;
  const int32_t rem = static_cast<int32_t>(acc & 0xffff);
  if (rem > 0x8000 || (rem == 0x8000 && (r & 1))) r += 1;
  return static_cast<int16_t>(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
}
inline int64_t Pr(int32_t a, int32_t b) { return 2 * static_cast<int64_t>(a) * b; }
inline int16_t Ap(int16_t* z, int32_t head, int32_t tail, int16_t v) {
  const int16_t d = z[tail / 2];
  const int16_t w = Ext(Pr(v, SP_ONE) + Pr(d, SP_G));
  z[head / 2] = w;
  return Ext(Pr(d, SP_ONE) - Pr(w, SP_G));
}
}  // namespace

extern "C" void PetriSpaceC(int16_t* z, const int16_t* x, uint32_t* y, uint32_t* lp, uint32_t decay) {
  const int32_t dl = static_cast<int16_t>(decay), dh = static_cast<int16_t>(decay >> 16);
  int16_t la = static_cast<int16_t>(*lp), lb = static_cast<int16_t>(*lp >> 16);
  for (int32_t n = 0; n < kN / 2; ++n) {
    int16_t v = x[n];
    v = Ap(z, SP_AP1_HEAD, SP_AP1_TAIL, v);
    v = Ap(z, SP_AP2_HEAD, SP_AP2_TAIL, v);
    v = Ap(z, SP_AP3_HEAD, SP_AP3_TAIL, v);
    v = Ap(z, SP_AP4_HEAD, SP_AP4_TAIL, v);
    const int16_t ta = Ext(Pr(v, SP_ONE) + Pr(z[SP_DL2_TAIL / 2], dl));
    const int16_t tb = Ext(Pr(v, SP_ONE) + Pr(z[SP_DL1_TAIL / 2], dh));
    const int32_t ea0 = ta - la, eb0 = tb - lb;
    const int16_t ea = static_cast<int16_t>(ea0 > 32767 ? 32767 : (ea0 < -32768 ? -32768 : ea0));
    const int16_t eb = static_cast<int16_t>(eb0 > 32767 ? 32767 : (eb0 < -32768 ? -32768 : eb0));
    la = Ext(Pr(la, SP_ONE) + Pr(ea, SP_DAMP));
    lb = Ext(Pr(lb, SP_ONE) + Pr(eb, SP_DAMP));
    const int16_t d5 = z[SP_AP5_TAIL / 2], d6 = z[SP_AP6_TAIL / 2];
    const int16_t wa = Ext(Pr(la, SP_ONE) - Pr(d5, SP_T)), wb = Ext(Pr(lb, SP_ONE) - Pr(d6, SP_T));
    z[SP_AP5_HEAD / 2] = wa;
    z[SP_AP6_HEAD / 2] = wb;
    const int16_t ya = Ext(Pr(d5, SP_ONE) + Pr(wa, SP_T)), yb = Ext(Pr(d6, SP_ONE) + Pr(wb, SP_T));
    z[SP_DL1_HEAD / 2] = ya;
    z[SP_DL2_HEAD / 2] = yb;
    y[n] = (static_cast<uint32_t>(static_cast<uint16_t>(yb)) << 16) | static_cast<uint16_t>(ya);
    --z;
  }
  *lp = (static_cast<uint32_t>(static_cast<uint16_t>(lb)) << 16) | static_cast<uint16_t>(la);
}

#endif  // the C references

namespace {

inline void VoiceLoop(const uint32_t* rec, uint32_t pos, int32_t rate, int32_t* wl, int32_t* wr, uint32_t gains,
                      uint32_t dgains) {
#if defined(__bfin__) && !defined(COOP_PETRI_C)
  PetriVoiceAsm(rec, pos, rate, wl, wr, gains, dgains);
#else
  PetriVoiceC(rec, pos, rate, wl, wr, gains, dgains);
#endif
}

inline uint32_t Pack(int32_t lo, int32_t hi) { return (static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) & 0xffff); }

__attribute__((noinline)) bool RenderVoices(Engine* e, int32_t mod_q24) {
  bool bus = false;
  for (int32_t vi = 0; vi < kVoices; ++vi) {
    Voice& v = e->v[vi];
    if (Idle(v)) {
      v.level = 0;
      continue;
    }
    int32_t env1;
    if (v.blocks == kForever) env1 = 32767;
    else if (v.blocks <= 1) env1 = kShapes[v.shape][64];
    else env1 = Envelope(v.shape, v.ephase + v.einc);
    int32_t g = (env1 * v.gain) >> 15;
    if (v.fade > 0) g = (g * (v.fade - 1)) >> 3;
    const int32_t gl1 = (g * v.pl) >> 15, gr1 = (g * v.pr) >> 15;
    // the effect's gentle pitch modulation, opposite on odd voices
    const int32_t off = (((v.rate >> 3) * (mod_q24 >> 9)) >> 12) * v.mod;
    const int32_t rate = v.rate + off;
    if (v.bus > 0) bus = true;
    int32_t* const ol = v.bus > 0 ? e->bus[v.bus - 1][0] : e->wl;
    int32_t* const orr = v.bus > 0 ? e->bus[v.bus - 1][1] : e->wr;
    VoiceLoop(e->rec, v.pos, rate, ol, orr, Pack(v.gl, v.gr), Pack((gl1 - v.gl) >> 5, (gr1 - v.gr) >> 5));
    v.pos += static_cast<uint32_t>(rate) * kN;
    v.gl = gl1;
    v.gr = gr1;
    v.level = g;
    v.ephase += v.einc;
    v.rate += v.slope;
    if (v.blocks != kForever && v.blocks > 0) --v.blocks;
    if (v.fade > 0 && --v.fade == 0) v.blocks = 0;
    if (v.blocks == 0) v.fade = 0;
  }
  return bus;
}

// ---- the effects (once a block, before the voices) ----

// Effect constants: pitch modulation depth (cents), its rate (Hz, Q8), the wet's makeup (Q12).
struct FxConst {
  int16_t cents;
  int16_t hz_q8;
  int16_t makeup;
};
const FxConst kFx[kNumEffects] = {   // makeup: the wet near the dry's level at the defaults (tests/host/petri takes)
  { 6, 77, 6500 },     // mosaic
  { 0, 0, 5200 },      // seq
  { 3, 51, 7500 },     // glide
  { 8, 44, 7200 },     // haze
  { 5, 33, 5700 },     // tunnel
  { 4, 128, 4700 },    // strum
  { 0, 0, 2700 },      // blocks
  { 0, 0, 5600 },      // cut
  { 4, 1280, 7000 },   // arp
  { 3, 102, 5000 },    // pattern
  { 10, 64, 6400 },    // warp
};

const int32_t kRate2 = 2 * kRateOne, kRateHalf = kRateOne / 2, kRate3_2 = kRateOne * 3 / 2;

void Mosaic(Engine* e, const Controls& c) {
  static const int32_t kRate[4] = { kRateOne, kRate2, kRateHalf, kRate3_2 };
  static const int32_t kPan[4] = { 0, -14000, 14000, -7000 };
  static const int32_t kGain[4] = { 22000, 15000, 18000, 12000 };
  Sched& s = e->s;
  const int32_t t = KnobFrames(c.time, 662, 5 * 256);                   // 30 ms .. 0.96 s
  const int32_t layers = 1 + ((c.activity * 3 + 16383) >> 15);
  const int32_t decay = 11469 + ((c.repeats * 20643) >> 15);           // 0.35 .. 0.98 a repeat
  for (int32_t k = 0; k < 4; ++k) {
    Voice& v = e->v[k];
    if (k >= layers) {
      Kill(v);
      continue;
    }
    if (!Idle(v)) continue;
    const int32_t blocks = BlocksFor(t, kRate[k]);
    const bool refresh = s.gain[k] < 3277 || Abs(s.slice[k] - t) > (t >> 3) || !Safe(e, s.grab[k], blocks);
    if (refresh) {
      s.grab[k] = e->head - static_cast<uint32_t>(t + kMargin);
      s.slice[k] = t;
      s.gain[k] = 32767;
    } else {
      s.gain[k] = (s.gain[k] * decay) >> 15;
    }
    Spawn(e, k, s.grab[k], kRate[k], BlocksFor(s.slice[k], kRate[k]), SH_TRAP, (s.gain[k] * kGain[k]) >> 15,
          (kPan[k] * (8192 + (c.activity >> 2) * 3)) >> 15);
  }
}

void Seq(Engine* e, const Controls& c) {
  static const int8_t kOrders[8][4] = { { 0, 2, 1, 3 }, { 3, 1, 2, 0 }, { 0, 0, 2, 1 }, { 1, 3, 0, 2 },
                                        { 2, 0, 3, 1 }, { 0, 3, 3, 1 }, { 3, 2, 1, 0 }, { 1, 0, 1, 2 } };
  enum { REST = 1, REV = 2, RATCHET = 4, OCT = 8 };
  Sched& s = e->s;
  const int32_t sl = Clamp(KnobFrames(c.time, 882, 538), 882, 3900);  // a step: 40 .. 177 ms
  const int32_t sb = (sl + 16) >> 5;
  const int32_t a = c.activity;
  if (s.aux[0] > 0 && --s.aux[0] == 0) {                               // a ratchet's second hit
    const int32_t vi = PickVoice(e, 0, 3);
    if (vi >= 0) Spawn(e, vi, s.aux[1], s.aux[2], s.aux[3], SH_GATE, 27000, -s.aux[4], (s.aux[5] & REV) != 0);
  }
  if (s.timer > 0) {
    --s.timer;
    return;
  }
  const int32_t j = s.step & 3;
  if (j == 0) {                                                         // a pass: the window, its order, its marks
    if (s.count <= 0 || !Safe(e, s.grab[0], 8 * sb)) {
      s.grab[0] = e->head - static_cast<uint32_t>(4 * sl + kMargin);
      s.slice[0] = sl;
      s.count = 1 + ((c.repeats * 7 + 16383) >> 15);                   // passes of one window: 1..8
    }
    --s.count;
    const int32_t pick = a < 6000 ? 0 : static_cast<int32_t>(Rand(e) & 7);
    for (int32_t k = 0; k < 4; ++k) {
      s.order[k] = kOrders[pick][k];
      int32_t f = 0;
      if (k > 0 && Chance(e, a / 6)) f |= REST;
      if (Chance(e, (a * 2) / 5)) f |= REV;
      if (Chance(e, (a * 3) / 10)) f |= RATCHET;
      if (Chance(e, a / 5)) f |= OCT;
      s.flags[k] = f;
    }
  }
  const int32_t f = s.flags[j];
  const uint32_t slice = s.grab[0] + static_cast<uint32_t>(s.order[j] * s.slice[0]);
  const int32_t pan = (j & 1) ? 6000 : -6000;
  if (!(f & REST)) {
    if (f & (RATCHET | OCT)) {                                          // two hits a step: halves, or the slice at 2x
      const int32_t rate = (f & OCT) ? kRate2 : kRateOne;
      const int32_t half = sb > 1 ? sb >> 1 : 1;
      const uint32_t at = slice;
      const int32_t vi = PickVoice(e, 0, 3);
      if (vi >= 0) Spawn(e, vi, at, rate, half + 1, SH_GATE, 27000, pan, (f & REV) != 0);
      s.aux[0] = half;
      s.aux[1] = static_cast<int32_t>(at);
      s.aux[2] = rate;
      s.aux[3] = half + 1;
      s.aux[4] = pan;
      s.aux[5] = f;
    } else {
      const int32_t vi = PickVoice(e, 0, 3);
      if (vi >= 0) Spawn(e, vi, slice, kRateOne, sb + 1, SH_GATE, 29000, pan, (f & REV) != 0);
    }
  }
  s.timer = sb - 1;
  ++s.step;
}

void Glide(Engine* e, const Controls& c) {
  static const int32_t kGain[4] = { 20000, 18000, 14000, 12000 };
  static const int32_t kPan[4] = { -12000, 12000, -5000, 5000 };
  Sched& s = e->s;
  const int32_t t = KnobFrames(c.time, 662, 5 * 256);                   // 30 ms .. 0.96 s
  const int32_t layers = 2 + ((c.activity * 2 + 16383) >> 15);         // 2..4
  const int32_t semis_q8 = 128 + ((c.activity * 11) >> 7);             // 0.5 .. 11.5 semitones a repeat
  const int32_t decay = 11469 + ((c.repeats * 20643) >> 15);
  const int32_t tb = t >> 5;
  for (int32_t k = 0; k < 4; ++k) {
    Voice& v = e->v[k];
    if (k >= layers) {
      Kill(v);
      continue;
    }
    if (!Idle(v)) continue;
    if (!s.flags[k]) {                                                  // the layers start staggered
      s.flags[k] = 1;
      s.aux[k] = (k * tb) / layers;
    }
    if (s.aux[k] > 0) {
      --s.aux[k];
      continue;
    }
    const int32_t dir = (k & 1) ? -1 : 1;
    const int32_t r0 = kRateOne, r1 = SemiRate(dir * (k >= 2 ? semis_q8 >> 1 : semis_q8));
    const int32_t blocks = BlocksFor(t, (r0 + r1) >> 1);
    const bool refresh = s.gain[k] < 3277 || Abs(s.slice[k] - t) > (t >> 3) || !Safe(e, s.grab[k], blocks);
    if (refresh) {
      s.grab[k] = e->head - static_cast<uint32_t>(t + kMargin);
      s.slice[k] = t;
      s.gain[k] = 32767;
    } else {
      s.gain[k] = (s.gain[k] * decay) >> 15;
    }
    const int32_t b = BlocksFor(s.slice[k], (r0 + r1) >> 1);
    Spawn(e, k, s.grab[k], r0, b, SH_HANN, (s.gain[k] * kGain[k]) >> 15, kPan[k], false, (r1 - r0) / b);
  }
}

void Haze(Engine* e, const Controls& c) {
  static const int32_t kGain[7] = { 0, 26000, 18500, 15100, 13100, 11700, 10700 };   // 0.8 / sqrt(overlap)
  Sched& s = e->s;
  e->fb = (c.repeats * 24576) >> 15;                                    // 0 .. 0.75
  if (s.timer > 0) {
    --s.timer;
    return;
  }
  const int32_t a = c.activity;
  const int32_t g = KnobFrames(c.time, 441, 1152);                      // a grain: 20 ms .. 0.45 s
  const int32_t gb = g >> 5 < 2 ? 2 : g >> 5;
  const int32_t ov_q8 = 256 + ((a * 5) >> 7);                           // 1..6 grains overlapping
  const int32_t interval = (gb << 8) / ov_q8;
  s.timer = ((interval * (24576 + (RandQ15(e) >> 1))) >> 15);
  if (s.timer > 0) --s.timer;
  int32_t rate = kRateOne + ((RandS15(e) * (4 + ((a * 20) >> 15))) >> 15) * 76;   // +-4..24 cents
  if (a > 19661 && Chance(e, ((a - 19661) * 3) >> 2)) rate <<= 1;
  else if (a > 26214 && Chance(e, 4000)) rate >>= 1;
  const int32_t material = MaterialFor(gb, rate);
  const int32_t spread = (a * 19845) >> 15;                             // up to 0.9 s back
  uint32_t start = e->head - static_cast<uint32_t>(material + kMargin + ((RandQ15(e) * spread) >> 15));
  if (!Safe(e, start, gb)) start = e->head - static_cast<uint32_t>(material + kMargin);
  const int32_t vi = PickVoice(e, 0, 5);
  if (vi < 0) return;
  Spawn(e, vi, start, rate, gb, SH_HANN, kGain[Clamp(ov_q8 >> 8, 1, 6)], (RandS15(e) * (9830 + ((a * 3) >> 2))) >> 15);
}

void Tunnel(Engine* e, const Controls& c) {
  static const int32_t kRate[3] = { kRateOne, kRateHalf, kRate2 };
  static const int32_t kGain[3] = { 23000, 7000, 5000 };
  Sched& s = e->s;
  const int32_t l = KnobFrames(c.time, 176, 1357);                      // the drone's period: 8 ms .. 0.31 s
  const int32_t layers = 1 + ((c.activity * 2 + 16383) >> 15);         // 1..3
  const int32_t g = Clamp(2 * l, 1323, 4410);
  const int32_t gb = (g >> 6) << 1;                                     // even: the two windows sum to one
  e->fb = 13107 + ((c.repeats * 16384) >> 15);                          // 0.4 .. 0.9
  for (int32_t k = 0; k < 3; ++k) {
    for (int32_t j = 0; j < 2; ++j) {
      const int32_t vi = 2 * k + j;
      Voice& v = e->v[vi];
      if (k >= layers) {
        Kill(v);
        s.flags[vi] = 0;
        continue;
      }
      if (!Idle(v)) continue;
      if (!s.flags[vi]) {
        s.flags[vi] = 1;
        s.aux[vi] = j ? gb >> 1 : 0;
      }
      if (s.aux[vi] > 0) {
        --s.aux[vi];
        continue;
      }
      const int32_t extra = kRate[k] > kRateOne ? MaterialFor(gb, kRate[k] - kRateOne) : 0;
      const uint32_t start = e->head - static_cast<uint32_t>(l + extra + 40);
      Spawn(e, vi, start, kRate[k], gb, SH_HANN, kGain[k], j ? 7000 : -7000);
    }
  }
}

void Strum(Engine* e, const Controls& c) {
  Sched& s = e->s;
  const int32_t spacing = KnobFrames(c.time, 331, 1024);                // 15 .. 240 ms between notes
  const int32_t spb = spacing >> 5 < 1 ? 1 : spacing >> 5;
  const int32_t notes = 2 + ((c.activity * 4 + 16383) >> 15);          // 2..6
  const int32_t restrums = (c.repeats * 6 + 16383) >> 15;              // 0..6
  const int32_t nb = Clamp(spacing * 6, 2646, 15435) >> 5;              // a note: 120 .. 700 ms
  if (e->onset_count != s.last_onsets) {
    s.last_onsets = e->onset_count;
    if (!e->hold) {
      s.n = RecentOnsets(e, s.grab, notes, nb + notes * spb + 8);
      s.step = 0;
      s.count = restrums;
      s.gain[0] = 32767;
      s.dir = 0;
      s.timer = spb;
    }
  }
  if (s.n <= 0) return;
  if (s.timer > 0) {
    --s.timer;
    return;
  }
  if (s.step < s.n) {
    const int32_t idx = s.dir ? s.n - 1 - s.step : s.step;
    const int32_t pan = s.n > 1 ? ((idx * 2 - (s.n - 1)) * 19000) / (s.n - 1) : 0;
    const int32_t vi = PickVoice(e, 0, 7);
    if (vi >= 0 && Safe(e, s.grab[idx], nb)) {
      Spawn(e, vi, s.grab[idx], kRateOne + (idx - 2) * 152, nb, SH_PERC, (s.gain[0] * 24000) >> 15, pan);
    }
    ++s.step;
    s.timer = spb - 1;
  } else if (s.count > 0) {                                             // strum again, the other way, softer
    --s.count;
    s.gain[0] = (s.gain[0] * 22938) >> 15;
    s.dir ^= 1;
    s.step = 0;
    s.timer = spb * 3;
  } else if (e->hold) {                                                 // held: keep strumming what is held
    s.gain[0] = 32767;
    s.dir ^= 1;
    s.step = 0;
    s.timer = (s.n + 3) * spb * 2;
  }
}

void Blocks(Engine* e, const Controls& c) {
  Sched& s = e->s;
  const int32_t a = c.activity;
  const int32_t sb = KnobFrames(c.time, 220, 1178);                     // a block: 10 .. 270 ms
  const int32_t sbb = sb >> 5 < 1 ? 1 : sb >> 5;
  const int32_t stutters = 2 + ((c.repeats * 14) >> 15);               // 2..16
  bool start = false;
  if (e->onset_count != s.last_onsets) {
    s.last_onsets = e->onset_count;
    if (!e->hold && (s.count == 0 || s.step >= 2)) {
      s.grab[0] = e->onsets[(e->onset_count - 1) & 7];
      start = true;
    }
  }
  if (e->hold && s.count == 0) {                                        // held: bursts on a clock from what is held
    if (s.aux[1] > 0) {
      --s.aux[1];
    } else {
      uint32_t on[8];
      const int32_t n = RecentOnsets(e, on, 8, 0);
      s.grab[0] = n > 0 ? on[Rand(e) % static_cast<uint32_t>(n)] : e->head - static_cast<uint32_t>(2 * sb + kMargin);
      s.aux[1] = 6 * sbb + (RandQ15(e) >> 11);
      start = true;
    }
  }
  if (start) {
    s.step = 0;
    s.count = stutters;
    s.timer = 0;
    s.gain[0] = 29000;
    s.slice[0] = sb;
    s.aux[0] = sb;                                                      // the length in force (it may halve)
  }
  if (s.count <= 0) return;
  if (s.timer > 0) {
    --s.timer;
    return;
  }
  int32_t rate = kRateOne, off = 0, len = s.aux[0];
  bool flip = false;
  if (s.step == 0 && !e->hold) {
    flip = e->reverse;                                                  // the first plays as it is recorded
  } else {
    if (Chance(e, a >> 1)) {
      switch (Rand(e) & 3) {
        case 0: rate = kRate2; break;
        case 1: rate = kRateHalf; break;
        case 2: flip = true; break;
        default: rate = kRate3_2; break;
      }
    }
    if (Chance(e, (a * 3) / 10)) off = (Rand(e) & 1) ? len >> 1 : len >> 2;
    if (Chance(e, a >> 2) && s.aux[0] > 882) s.aux[0] >>= 1;           // the stutter speeds up
  }
  const int32_t material = len - off;
  const int32_t blocks = BlocksFor(material, rate);
  const int32_t vi = PickVoice(e, 0, 3);
  if (vi >= 0 && Safe(e, s.grab[0], blocks)) {
    Spawn(e, vi, s.grab[0] + static_cast<uint32_t>(off), rate, blocks + 1, SH_GATE, s.gain[0],
          (s.phase ? 1 : -1) * ((a * 13107) >> 15), flip);
  }
  s.phase ^= 1;
  s.timer = blocks - 1;
  s.gain[0] = (s.gain[0] * 31130) >> 15;
  ++s.step;
  --s.count;
}

void Cut(Engine* e, const Controls& c) {
  static const int32_t kSemis[6] = { 12, -12, 7, 19, -5, 24 };
  Sched& s = e->s;
  const int32_t a = c.activity;
  if (s.aux[2] > 0) --s.aux[2];
  if (s.count > 0) {                                                    // a burst's next stutter
    if (s.timer > 0) {
      --s.timer;
      return;
    }
    const int32_t vi = PickVoice(e, 0, 3);
    if (vi >= 0) Spawn(e, vi, s.grab[0], s.aux[0], s.aux[1] + 1, SH_GATE, 29000, s.aux[3], s.aux[4] != 0);
    s.phase ^= 1;
    s.timer = s.aux[1] - 1;
    --s.count;
    return;
  }
  if (s.aux[2] > 0) return;
  // a new burst: the interval to the next from repeats (4 s .. 0.2 s), the pitch from activity's set
  const int32_t sc = KnobFrames(c.time, 441, 1101);                     // 20 .. 435 ms
  const int32_t x = -((c.repeats * 1101) >> 7);
  const int32_t interval = (static_cast<int32_t>((5512u * Exp2Q16(x)) >> 12)) >> 5;
  s.aux[2] = (interval * (22938 + (RandQ15(e) * 19660 >> 15))) >> 15;   // x 0.7 .. 1.3
  const int32_t npitch = 1 + ((a * 5 + 16383) >> 15);
  const int32_t rate = SemiRate(kSemis[Rand(e) % static_cast<uint32_t>(npitch)] << 8);
  s.grab[0] = e->head - static_cast<uint32_t>(sc + kMargin);
  s.aux[0] = rate;
  s.aux[1] = BlocksFor(sc, rate);
  s.aux[3] = (RandS15(e) * 13107) >> 15;
  s.aux[4] = Chance(e, a / 3) ? 1 : 0;
  s.count = 1 + static_cast<int32_t>(Rand(e) % static_cast<uint32_t>(1 + ((a * 3) >> 15)));
  s.timer = 0;
}

void Arp(Engine* e, const Controls& c) {
  static const int32_t kSemis[8] = { 0, 7, 12, 19, 24, 19, 12, 7 };
  Sched& s = e->s;
  if (s.timer > 0) {
    --s.timer;
    return;
  }
  const int32_t st = KnobFrames(c.time, 1323, 845);                     // a step: 60 .. 595 ms
  const int32_t stb = st >> 5;
  const int32_t nsteps = 1 + ((c.activity * 7 + 16383) >> 15);         // 1..8
  const int32_t note = (st * (13107 + (c.repeats << 1))) >> 15;         // 0.4 .. 2.4 steps long
  const int32_t nb = note >> 5 < 2 ? 2 : note >> 5;
  const int32_t rate = SemiRate(kSemis[s.step % nsteps] << 8);
  const int32_t material = MaterialFor(nb, rate);
  const uint32_t latest = e->head - static_cast<uint32_t>(material + kMargin);
  uint32_t on[3];
  const int32_t m = RecentOnsets(e, on, 3, nb + stb * 8);
  uint32_t start = m > 0 ? on[m - 1 - (s.step / nsteps) % m] : latest;
  if (static_cast<int32_t>(start - latest) > 0) start = latest;         // never ahead of what is recorded
  if (!Safe(e, start, nb)) start = latest;
  const int32_t vi = PickVoice(e, 0, 3);
  if (vi >= 0) Spawn(e, vi, start, rate, nb, SH_PERC, 24000, (s.step & 1) ? 9000 : -9000);
  ++s.step;
  s.timer = stb - 1;
}

void Pattern(Engine* e, const Controls& c) {
  static const int32_t kTap16[6] = { 16, 8, 12, 6, 14, 3 };
  static const int32_t kGain[6] = { 20000, 15000, 14000, 12000, 11000, 10000 };
  static const int32_t kPan[6] = { 0, -20000, 20000, -11000, 11000, 0 };
  const int32_t b = Clamp(KnobFrames(c.time, 2205, 973), 2205, kRecFrames - 2048);   // the bar: 0.1 .. 1.3 s
  const int32_t n = 1 + ((c.activity * 5 + 16383) >> 15);              // 1..6 taps
  int32_t sum = 0;
  for (int32_t k = 0; k < n; ++k) sum += kGain[k];
  e->fb = (((c.repeats * 27852) >> 15) * 32767) / sum;                  // the loop gain 0 .. 0.85
  const bool chunks = e->reverse || e->hold;
  for (int32_t k = 0; k < 6; ++k) {
    Voice& v = e->v[k];
    if (k >= n) {
      Kill(v);
      continue;
    }
    const int32_t d = (b * kTap16[k]) >> 4;
    if (!chunks) {
      if (!Idle(v) && v.tag != 1) {
        Kill(v);
        continue;
      }
      if (Idle(v)) {
        Spawn(e, k, e->head - static_cast<uint32_t>(d), kRateOne, kForever, SH_FLAT, kGain[k], kPan[k]);
        v.tag = 1;
        continue;
      }
      if (v.fade) continue;
      const int32_t cur = static_cast<int32_t>((e->head - (v.pos >> kPosShift)) & kRecMask);
      const int32_t err = cur - d;
      if (Abs(err) > 6000) Kill(v);                                     // a jump: fade out, start again there
      else v.rate = kRateOne + Clamp(err * 32, -(kRateOne / 3), kRateOne / 3);   // a small change: glide (tape)
    } else {
      if (!Idle(v) && v.tag == 1) {
        Kill(v);
        continue;
      }
      if (!Idle(v)) continue;
      const int32_t ch = Clamp(d, 1323, 11025);                         // reverse / held: the tap plays chunks
      Spawn(e, k, e->head - static_cast<uint32_t>(ch + kMargin), kRateOne, ch >> 5, SH_GATE, kGain[k], kPan[k]);
      v.tag = 2;
    }
  }
}

void Warp(Engine* e, const Controls& c) {
  static const int32_t kSemi[3] = { 12, -12, 7 };
  static const int32_t kLp[3] = { 24000, 6000, 12000 };
  static const int32_t kGain[3] = { 17000, 17000, 14000 };
  static const int32_t kPan[3] = { -16000, 16000, 0 };
  Sched& s = e->s;
  const int32_t d = Clamp(KnobFrames(c.time, 1323, 1075), 1323, 26000); // the longest tap: 60 ms .. 1.18 s
  const int32_t gb = 56;                                                 // 81 ms grains, two a tap
  e->fb = (((c.repeats * 26214) >> 15) * 32767) / (17000 + 17000 + 14000);   // the loop gain 0 .. 0.8
  for (int32_t k = 0; k < 3; ++k) {
    e->bus_c[k] = kLp[k];
    const int32_t rate = SemiRate((c.activity * kSemi[k]) >> 7);
    const int32_t delay = (d * (k + 1)) / 3;
    for (int32_t j = 0; j < 2; ++j) {
      const int32_t vi = 2 * k + j;
      if (!Idle(e->v[vi])) continue;
      if (!s.flags[vi]) {
        s.flags[vi] = 1;
        s.aux[vi] = j ? gb >> 1 : 0;
      }
      if (s.aux[vi] > 0) {
        --s.aux[vi];
        continue;
      }
      const int32_t extra = rate > kRateOne ? MaterialFor(gb, rate - kRateOne) : 0;
      Spawn(e, vi, e->head - static_cast<uint32_t>(delay + extra + kMargin), rate, gb, SH_HANN, kGain[k], kPan[k],
            false, 0, k + 1);
    }
  }
}

// ---- the input: level and onsets ----

void Detect(Engine* e, const int16_t* in_l, const int16_t* in_r) {
  int32_t peak = 0;
  for (int32_t i = 0; i < kN; ++i) {
    const int32_t a = Abs(in_l[i]), b = Abs(in_r[i]);
    if (a > peak) peak = a;
    if (b > peak) peak = b;
  }
  e->env_fast -= e->env_fast >> 3;
  if (peak > e->env_fast) e->env_fast = peak;
  e->level = e->env_fast;
  if (e->refractory > 0) --e->refractory;
  bool onset = e->refractory == 0 && peak > 2500 && peak > 2 * e->env_slow + 1000;
  e->env_slow += (peak - e->env_slow) >> 5;
  if (!e->hold) {
    ++e->quiet;
    if (!onset && e->quiet > 690 && e->env_slow > 1500) onset = true;   // a held sound: an onset a second
    if (onset) {
      e->onsets[e->onset_count & 7] = e->head - 110;                    // 5 ms before: the attack
      ++e->onset_count;
      e->refractory = 48;
      e->quiet = 0;
    }
  }
}

// ---- the tone section ----

__attribute__((noinline)) void FilterRun(Engine* e, int32_t knob) {
  Filter& f = e->filt;
  f.knob += ((Clamp(knob, 0, 32767) << 8) - f.knob) >> 3;
  const int32_t x = f.knob >> 8;
  const int32_t i = x >> 9, fr = x & 511;
  const int32_t* t0 = kSvf[i];
  const int32_t* t1 = kSvf[i < 64 ? i + 1 : 64];
  const int32_t a1 = t0[0] + ((t1[0] - t0[0]) >> 9) * fr;
  const int32_t a2 = t0[1] + ((t1[1] - t0[1]) >> 9) * fr;
  const int32_t a3 = t0[2] + ((t1[2] - t0[2]) >> 9) * fr;
  int32_t* w[2] = { e->wl, e->wr };
  for (int32_t c = 0; c < 2; ++c) {
    int32_t ic1 = f.ic1[c], ic2 = f.ic2[c];
    int32_t* io = w[c];
    for (int32_t n = 0; n < kN; ++n) {
      const int32_t in = Clamp(io[n], -49152, 49152) << 13;
      const int32_t v3 = in - ic2;
      const int32_t v1 = petri_hb::MulQ31Fast(a1, ic1) + petri_hb::MulQ31Fast(a2, v3);
      const int32_t v2 = ic2 + petri_hb::MulQ31Fast(a2, ic1) + petri_hb::MulQ31Fast(a3, v3);
      ic1 = 2 * v1 - ic1;
      ic2 = 2 * v2 - ic2;
      io[n] = Sat16((v2 + 4096) >> 13);
    }
    f.ic1[c] = ic1;
    f.ic2[c] = ic2;
  }
}

// The space (src/space_bfin.S, src/space_layout.h): four input allpasses, then two tank halves in a figure of eight
// (damping, allpass, delay), at 11.025 kHz (every other sample: a wash needs no more; its output is interpolated
// back), in one int16 buffer read through a pointer that steps down (no wrap: petri_engine.h kSpaceSlide).
static_assert(SP_USED_BYTES <= 2 * kSpaceSpan, "the space's lines fit its window");

inline int32_t M15(int32_t x, int32_t g) { return (x * g) >> 15; }

__attribute__((noinline)) void SpaceRun(Engine* e, int32_t knob) {
  Space& sp = e->space;
  sp.amount += (Clamp(knob, 0, 32767) - sp.amount) >> 3;
  const int32_t a = sp.amount;
  const int32_t decay = 16384 + ((a * 13763) >> 15);                    // 0.5 .. 0.92
  const int32_t dry = 32767 - (a >> 1);                                 // the wet's share
  const int32_t ret = (a * 3) >> 1;                                     // the tank's
  if (sp.q < kN / 2) {                                                  // slide the live window up
    uint32_t* const w = reinterpret_cast<uint32_t*>(sp.buf + sp.q);
    for (int32_t i = 0; i < kSpaceSpan / 2; ++i) w[i + kSpaceSlide / 2] = w[i];
    sp.q += kSpaceSlide;
  }
  int16_t xin[kN / 2];
  uint32_t y[kN / 2];
  for (int32_t n = 0; n < kN / 2; ++n)                                  // the wet is in the int16 range (FilterRun)
    xin[n] = static_cast<int16_t>((e->wl[2 * n] + e->wl[2 * n + 1] + e->wr[2 * n] + e->wr[2 * n + 1]) >> 3);
  uint32_t lp = static_cast<uint32_t>(sp.lpa);
#if defined(__bfin__) && !defined(COOP_PETRI_C)
  PetriSpaceAsm(sp.buf + sp.q, xin, y, &lp, (static_cast<uint32_t>(decay) << 16) | static_cast<uint32_t>(decay));
#else
  PetriSpaceC(sp.buf + sp.q, xin, y, &lp, (static_cast<uint32_t>(decay) << 16) | static_cast<uint32_t>(decay));
#endif
  sp.lpa = static_cast<int32_t>(lp);
  sp.q -= kN / 2;
  int32_t pa = sp.ya, pb = sp.yb;
  for (int32_t n = 0; n < kN / 2; ++n) {                                // back to 22.05 kHz: the midpoint, the sample
    int32_t ya = static_cast<int16_t>(y[n]), yb = static_cast<int16_t>(y[n] >> 16);
    if (static_cast<uint32_t>(ya + 2) <= 4u) ya = 0;                    // the rounded tank's last LSBs (a limit cycle)
    if (static_cast<uint32_t>(yb + 2) <= 4u) yb = 0;                    // never reach the output
    int32_t* const l = e->wl + 2 * n;
    int32_t* const r = e->wr + 2 * n;
    l[0] = M15(l[0], dry) + M15((pa + ya) >> 1, ret);
    r[0] = M15(r[0], dry) + M15((pb + yb) >> 1, ret);
    l[1] = M15(l[1], dry) + M15(ya, ret);
    r[1] = M15(r[1], dry) + M15(yb, ret);
    pa = ya;
    pb = yb;
  }
  sp.ya = pa;
  sp.yb = pb;
}

// Warp's buses: each a one-pole low-pass, added into the wet sum (16 x 16 products; the step rounded away from zero, so
// the state always reaches its input: no offset left behind), then cleared for the next call.
__attribute__((noinline)) void BusRun(Engine* e) {
  for (int32_t b = 0; b < kBuses; ++b) {
    const int16_t c = static_cast<int16_t>(e->bus_c[b]);
    int32_t zl = e->bus_z[b][0], zr = e->bus_z[b][1];
    int32_t* const il = e->bus[b][0];
    int32_t* const ir = e->bus[b][1];
    for (int32_t n = 0; n < kN; ++n) {
      const int32_t ql = static_cast<int16_t>(Clamp(Clamp(il[n], -32768, 32767) - zl, -32768, 32767)) * c;
      const int32_t qr = static_cast<int16_t>(Clamp(Clamp(ir[n], -32768, 32767) - zr, -32768, 32767)) * c;
      zl += (ql + ((~ql >> 31) & 0x7fff)) >> 15;
      zr += (qr + ((~qr >> 31) & 0x7fff)) >> 15;
      e->wl[n] += zl;
      e->wr[n] += zr;
    }
    e->bus_z[b][0] = zl;
    e->bus_z[b][1] = zr;
    for (int32_t n = 0; n < kN; ++n) il[n] = ir[n] = 0;
  }
}

}  // namespace

uint32_t Exp2Q16(int32_t x) {
  const int32_t n = x >> 16;
  const uint32_t f = static_cast<uint32_t>(x) & 0xffff;
  const uint32_t i = f >> 10, fr = (f & 1023) >> 2;
  const uint32_t t = kExp2Q30[i] + (((kExp2Q30[i + 1] - kExp2Q30[i]) * fr) >> 8);   // Q30, 1..2
  const int32_t sh = 14 - n;
  if (sh >= 32) return 0;
  if (sh >= 0) return t >> sh;
  if (sh < -1) return 0xffffffffu;                                      // beyond 2^17: not used
  return t << 1;
}

int32_t Envelope(int32_t shape, uint32_t ph) {
  if (ph >= (1u << 24)) return kShapes[shape][64];
  const int16_t* t = kShapes[shape];
  const int32_t i = static_cast<int32_t>(ph >> 18), fr = static_cast<int32_t>((ph >> 3) & 0x7fff);
  return t[i] + (((t[i + 1] - t[i]) * fr) >> 15);
}

void Reset(Engine* e) {
  uint32_t* w = reinterpret_cast<uint32_t*>(&e->v[0]);
  for (uint32_t i = 0; i < sizeof(e->v) / 4; ++i) w[i] = 0;
  w = reinterpret_cast<uint32_t*>(&e->s);
  for (uint32_t i = 0; i < sizeof(e->s) / 4; ++i) w[i] = 0;
  w = reinterpret_cast<uint32_t*>(&e->filt);
  for (uint32_t i = 0; i < sizeof(e->filt) / 4; ++i) w[i] = 0;
  e->space.q = kSpaceSlide;
  e->dcx[0] = e->dcx[1] = e->dcy[0] = e->dcy[1] = 0;
  e->rec_in = 0;
  e->space.lpa = e->space.lpb = 0;
  e->space.amount = 0;
  e->space.ya = e->space.yb = 0;
  w = reinterpret_cast<uint32_t*>(&e->bus[0][0][0]);
  for (uint32_t i = 0; i < sizeof(e->bus) / 4; ++i) w[i] = 0;
  for (int32_t b = 0; b < kBuses; ++b) e->bus_c[b] = e->bus_z[b][0] = e->bus_z[b][1] = 0;
  e->filt.knob = 32767 << 8;
  e->head = 0;
  e->effect = -1;
  e->rng = 0x9E3779B9u;
  e->lfo = 0;
  e->fb = 0;
  e->makeup = 4096;
  e->env_fast = e->env_slow = e->refractory = e->quiet = 0;
  for (int32_t i = 0; i < 8; ++i) e->onsets[i] = 0;
  e->onset_count = 0;
  e->level = 0;
  e->hold = e->reverse = false;
  for (int32_t i = 0; i < kN; ++i) e->wl[i] = e->wr[i] = 0;
}

int32_t ClearStep(Engine* e, int32_t at, int32_t words) {
  int32_t end = at + words;
  if (end > kClearEnd) end = kClearEnd;
  const int32_t rec_end = end < kRecFrames + 1 ? end : kRecFrames + 1;
  for (int32_t i = at; i < rec_end; ++i) e->rec[i] = 0;
  uint32_t* const sp = reinterpret_cast<uint32_t*>(e->space.buf);
  const int32_t from = at > kRecFrames + 1 ? at : kRecFrames + 1;
  for (int32_t i = from; i < end; ++i) sp[i - (kRecFrames + 1)] = 0;
  return end;
}

void Process(Engine* e, const int16_t* in_l, const int16_t* in_r, int16_t* out_l, int16_t* out_r, const Controls& c) {
  const int32_t fx = Clamp(c.effect, 0, kNumEffects - 1);
  e->hold = c.hold;
  e->reverse = c.reverse;
  if (fx != e->effect) {                                                // a new effect: the voices fade, it starts clean
    KillAll(e);
    uint32_t* w = reinterpret_cast<uint32_t*>(&e->s);
    for (uint32_t i = 0; i < sizeof(e->s) / 4; ++i) w[i] = 0;
    e->s.last_onsets = e->onset_count;
    e->effect = fx;
  }
  Detect(e, in_l, in_r);
  const FxConst& k = kFx[fx];
  e->lfo += static_cast<uint32_t>(k.hz_q8) * 24348u;
  const int32_t mod_q24 = (coop_scr::SinQ15(e->lfo) * (k.cents * 9697)) >> 15;
  e->fb = 0;
  e->makeup = k.makeup;
  const Controls cc = c;
  switch (fx) {
    case FX_MOSAIC: Mosaic(e, cc); break;
    case FX_SEQ: Seq(e, cc); break;
    case FX_GLIDE: Glide(e, cc); break;
    case FX_HAZE: Haze(e, cc); break;
    case FX_TUNNEL: Tunnel(e, cc); break;
    case FX_STRUM: Strum(e, cc); break;
    case FX_BLOCKS: Blocks(e, cc); break;
    case FX_CUT: Cut(e, cc); break;
    case FX_ARP: Arp(e, cc); break;
    case FX_PATTERN: Pattern(e, cc); break;
    default: Warp(e, cc); break;
  }
  for (int32_t i = 0; i < kN; ++i) e->wl[i] = e->wr[i] = 0;
  if (RenderVoices(e, mod_q24)) BusRun(e);

  // The record: the input (and, for the effects that have it, the wet fed back), unless held.
  if (!e->hold) {
    const uint32_t at = e->head & kRecMask;
    uint32_t* const rec = e->rec + at;
    const int32_t fb = e->fb;
    int16_t fl[kN], fr[kN];
    if (e->rec_in < 32767) {                                            // after a restart the record fades in (46 ms):
      const int32_t g0 = e->rec_in, g1 = g0 + 1024 > 32767 ? 32767 : g0 + 1024;   // a read across its start
      for (int32_t i = 0; i < kN; ++i) {                                // never meets a step
        const int32_t g = g0 + (((g1 - g0) * i) >> 5);
        fl[i] = static_cast<int16_t>((in_l[i] * g) >> 15);
        fr[i] = static_cast<int16_t>((in_r[i] * g) >> 15);
      }
      e->rec_in = g1;
      in_l = fl;
      in_r = fr;
    }
    if (fb > 0) {
      int32_t x0l = e->dcx[0], x0r = e->dcx[1], y0l = e->dcy[0], y0r = e->dcy[1];
      const int32_t* const wl = e->wl;
      const int32_t* const wr = e->wr;
      for (int32_t i = 0; i < kN; ++i) {
        // a DC blocker (~17 Hz: the voices' floors must not build an offset round the loop), then the gain toward
        // zero (a -1 never goes round for ever); 16 x 16 products
        const int32_t xl = Clamp(wl[i], -32768, 32767), xr = Clamp(wr[i], -32768, 32767);
        const int32_t yl = Clamp(xl - x0l + MulZ16(static_cast<int16_t>(y0l), 32604), -32768, 32767);
        const int32_t yr = Clamp(xr - x0r + MulZ16(static_cast<int16_t>(y0r), 32604), -32768, 32767);
        x0l = xl;
        x0r = xr;
        y0l = yl;
        y0r = yr;
        const int32_t ql = MulZ16(static_cast<int16_t>(yl), static_cast<int16_t>(fb));
        const int32_t qr = MulZ16(static_cast<int16_t>(yr), static_cast<int16_t>(fb));
        const int32_t l = Sat16(in_l[i] + ql);
        const int32_t r = Sat16(in_r[i] + qr);
        rec[i] = (static_cast<uint32_t>(l) & 0xffff) | (static_cast<uint32_t>(r) << 16);
      }
      e->dcx[0] = x0l;
      e->dcx[1] = x0r;
      e->dcy[0] = y0l;
      e->dcy[1] = y0r;
    } else {
      for (int32_t i = 0; i < kN; ++i)
        rec[i] = (static_cast<uint32_t>(in_l[i]) & 0xffff) | (static_cast<uint32_t>(in_r[i]) << 16);
    }
    if (at == 0) e->rec[kRecFrames] = e->rec[0];
    e->head += kN;
  }

  // The tone section: makeup, the filter, the space; out as int16.
  const int32_t mk = e->makeup;
  for (int32_t i = 0; i < kN; ++i) {
    const int32_t pl = Clamp(e->wl[i], -131072, 131071) * mk;            // toward zero, as the loops
    const int32_t pr = Clamp(e->wr[i], -131072, 131071) * mk;
    e->wl[i] = (pl + ((pl >> 31) & 0xfff)) >> 12;
    e->wr[i] = (pr + ((pr >> 31) & 0xfff)) >> 12;
  }
  FilterRun(e, c.filter);
  SpaceRun(e, c.space);
  for (int32_t i = 0; i < kN; ++i) {
    out_l[i] = static_cast<int16_t>(Sat16(e->wl[i]));
    out_r[i] = static_cast<int16_t>(Sat16(e->wr[i]));
  }
}

}  // namespace petri
