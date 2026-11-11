// ENSEMBLE: chorus -> string ensemble in fixed point. See ensemble.h and te-op1-cfw docs/ensemble-port.md.
//
// coop copy of te-op1-cfw ports/fx/src/ensemble/ensemble.cc: the namespace is coop_ensemble, the tables the upstream
// built from constexpr double maths are integer literals (src/ensemble_tables.cc, gen_tables.py: coop allows no
// float or double anywhere, guide/plugin-guidelines.md 2.1), the assembly kernels are EnsSide*Asm, and the C side
// kernel (the tests' reference) is compiled for the host only. Otherwise unchanged; tests/host/ensemble checks it
// against the upstream bit for bit.

#include "src/ensemble.h"
#include "src/ensemble_tables.h"

namespace coop_ensemble {

namespace {

static_assert(EN_BASE + EN_SLOW_MAX + EN_FAST_MAX + 2 <= EN_REACH, "reach");
static_assert(EN_BASE - EN_SLOW_MAX - EN_FAST_MAX - 1 > 0, "shortest delay");
static_assert(EN_REACH < EN_SIZE, "mirror");

inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// Q15 sine of a phase, linearly interpolated from the 1024-point table.
inline int16_t Sine(uint32_t phase) {
  const int32_t i = static_cast<int32_t>(phase >> 22);
  const int32_t f = static_cast<int32_t>((phase >> 6) & 0xffff);
  const int32_t a = kSine[i], b = kSine[i + 1];
  return static_cast<int16_t>(a + (((b - a) * f) >> 16));
}

// 65536 / n for even n (n = 2 j).
constexpr int32_t kRecip[kEnsembleMaxBlock / 2 + 1] = {
  0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341,
  2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130,
  1092, 1057, 1024,
};
// log2(n) for even n = 2 j, -1 if n isn't a power of two.
constexpr int8_t kShift[kEnsembleMaxBlock / 2 + 1] = {
  -1, 1, 2, -1, 3, -1, -1, -1, 4, -1, -1, -1, -1, -1, -1, -1, 5,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6,
};

// delta / n, floored (a shift for the usual n = 32).
inline int32_t Ramp(int32_t delta, int32_t n) {
  const int32_t shift = kShift[n >> 1];
  if (shift >= 0) return delta >> shift;
  return static_cast<int32_t>((static_cast<int64_t>(delta) * kRecip[n >> 1]) >> 16);
}

// An eighth of the way each call (~11.6 ms), snapped when close (Q15 << 8).
inline int32_t Smooth(int32_t s, int32_t target_q15) {
  const int32_t t = target_q15 << 8;
  const int32_t d = t - s;
  if (d < 2048 && d > -2048) return t;
  return s + (d >> 3);
}

const uint32_t kThird = 1431655765u;     // 2^32 / 3
const uint32_t kTwoThirds = 2863311531u;

// --- The Blackfin's 16-bit MAC arithmetic (as plate.cc) ---

inline int64_t Fr(int32_t a, int32_t b) {
  if (a == -32768 && b == -32768) return 0x7fffffff;
  return static_cast<int64_t>(a * b) * 2;
}
inline int32_t Sat16(int64_t x) {
  return static_cast<int32_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}
// The accumulator's high half, rounded as the MAC does by default
// (unbiased: ties to even), saturated.
inline int32_t Rnd(int64_t acc) {
  int64_t q = acc >> 16;
  const int32_t r = static_cast<int32_t>(acc & 0xffff);
  if (r > 0x8000 || (r == 0x8000 && (q & 1))) ++q;
  return Sat16(q);
}
inline int32_t Lo(int32_t x) { return static_cast<int16_t>(x & 0xffff); }
inline int32_t Hi(int32_t x) { return static_cast<int16_t>(static_cast<uint32_t>(x) >> 16); }
inline int32_t Pack(int32_t lo, int32_t hi) {
  return (lo & 0xffff) | static_cast<int32_t>(static_cast<uint32_t>(hi) << 16);
}

// The line read pos (Q16) samples back from q: two taps, weights 32767 - f
// and f.
inline int32_t Tap(const int16_t* q, int32_t pos) {
  const int32_t ip = pos >> 16, f = (pos & 0xffff) >> 1;
  return Rnd(Fr(q[ip], 32767 - f) + Fr(q[ip + 1], f));
}

}  // namespace

uint32_t EnsembleRateIncrement(int32_t rate) {
  const int32_t x = Clamp(rate, 0, 32767);
  const int32_t i = x >> 10, f = x & 1023;
  return kRateTable[i] +
         static_cast<uint32_t>((static_cast<int32_t>(kRateTable[i + 1] - kRateTable[i]) * f) >> 10);
}

int32_t EnsembleSlowCap(int32_t rate) {
  const int32_t x = Clamp(rate, 0, 32767);
  const int32_t i = x >> 10, f = x & 1023;
  return kCapTable[i] + (((kCapTable[i + 1] - kCapTable[i]) * f) >> 10);
}

// One copy (1, 0, 0) -> two (1/2, 1/2, 0) -> three (1/3, 1/3, 1/3),
// linearly; the sum stays 1.
void EnsembleGains(int32_t voices, int32_t* g) {
  const int32_t v = Clamp(voices, 0, 32767);
  if (v <= 16384) {
    const int32_t t = v << 1;                     // Q15, 0..32768
    g[0] = 32767 - (t >> 1);
    g[1] = t >> 1;
    g[2] = 0;
  } else {
    const int32_t t = (v - 16384) << 1;           // 0..32766
    g[0] = g[1] = 16384 - ((t * 5461) >> 15);     // 1/2 - t/6
    g[2] = (t * 10923) >> 15;                     // t/3
  }
}

// --- The side kernel (C reference; ensemble_bfin.S matches it). Host builds only: the device runs the assembly ---

#ifndef __bfin__

void EnsembleSideRunC(const EnsembleSideKernel* k, int16_t* out, int32_t n, int32_t voices) {
  int32_t pos0 = k->pos[0], pos1 = k->pos[1], pos2 = k->pos[2];
  const int32_t st0 = k->step[0], st1 = k->step[1], st2 = k->step[2];
  int32_t g1 = Lo(k->g12), g2 = Hi(k->g12), g3 = Lo(k->g3);
  const int32_t s1 = Lo(k->g12_step), s2 = Hi(k->g12_step), s3 = Hi(k->g3);
  for (int32_t i = 0; i < n; ++i) {
    const int16_t* a = k->line_a - i;
    int64_t acc = Fr(Tap(a, pos0), g1);
    if (voices == 3) {
      acc += Fr(Tap(a, pos1), g2);
      acc += Fr(Tap(k->line_b - i, pos2), g3);
    }
    out[i] = static_cast<int16_t>(Rnd(acc));
    pos0 += st0;
    pos1 += st1;
    pos2 += st2;
    g1 = static_cast<int16_t>(g1 + s1);
    g2 = static_cast<int16_t>(g2 + s2);
    g3 = static_cast<int16_t>(g3 + s3);
  }
}
#endif

// --- Control ---

void EnsembleControl::Init() {
  started_ = false;
  settled_ = false;
  rate_ = depth_ = voices_ = 0;
  slow_phase_ = fast_phase_ = 0;
  inc_ = 0;
  a_ = f_ = 0;
  for (int32_t j = 0; j < 6; ++j) pos_[j] = EN_BASE << 16;
  g_[0] = 32767;
  g_[1] = g_[2] = 0;
  last_.rate = last_.depth = last_.voices = -1;
}

void EnsembleControl::Update(const EnsembleParams& p, int32_t n, EnsembleCoefs* c) {
  const bool first = !started_;
  // Still knobs and settled smoothers and gains: the previous call's
  // increments, amplitudes and gains hold (only the LFOs move). Exactly what
  // the full path would compute.
  const bool fast = settled_ && p.rate == last_.rate && p.depth == last_.depth &&
                    p.voices == last_.voices;
  if (!fast) {
    const int32_t rate_t = Clamp(p.rate, 0, 32767);
    const int32_t depth_t = Clamp(p.depth, 0, 32767);
    const int32_t voices_t = Clamp(p.voices, 0, 32767);
    if (first) {
      started_ = true;
      rate_ = rate_t << 8;
      depth_ = depth_t << 8;
      voices_ = voices_t << 8;
    } else {
      rate_ = Smooth(rate_, rate_t);
      depth_ = Smooth(depth_, depth_t);
      voices_ = Smooth(voices_, voices_t);
    }
    const int32_t rate = rate_ >> 8, depth = depth_ >> 8, voices = voices_ >> 8;
    inc_ = EnsembleRateIncrement(rate);
    a_ = (depth * EnsembleSlowCap(rate)) >> 15;                         // Q8
    f_ = (((depth * voices) >> 15) * (EN_FAST_MAX * 256)) >> 15;        // Q8

    // Gains: ramped per sample (steps rounded towards zero, so a ramp never
    // overshoots); a remainder under one step per sample snaps.
    int32_t gt[3];
    EnsembleGains(voices, gt);
    bool still = true;
    for (int32_t j = 0; j < 3; ++j) {
      const int32_t d = gt[j] - g_[j];
      if (first || (d < n && d > -n)) {
        g_[j] = gt[j];
        c->g0[j] = gt[j];
        c->g_step[j] = 0;
      } else {
        const int32_t step = d > 0 ? Ramp(d, n) : -Ramp(-d, n);
        c->g0[j] = g_[j];
        c->g_step[j] = step;
        g_[j] += step * n;
        still = false;
      }
    }
    c->one_voice = c->g0[1] == 0 && c->g0[2] == 0 && c->g_step[1] == 0 && c->g_step[2] == 0;
    settled_ = still && rate_ == rate_t << 8 && depth_ == depth_t << 8 && voices_ == voices_t << 8;
    last_ = p;
  } else {
    for (int32_t j = 0; j < 3; ++j) {
      c->g0[j] = g_[j];
      c->g_step[j] = 0;
    }
    c->one_voice = g_[1] == 0 && g_[2] == 0;
  }

  // LFOs: free-running; the delays at the end of the call, ramped to.
  slow_phase_ += inc_ * static_cast<uint32_t>(n);
  fast_phase_ += kEnsembleFastIncrement * static_cast<uint32_t>(n);
  const int16_t a = static_cast<int16_t>(a_), f = static_cast<int16_t>(f_);   // <= 20480: 16 x 16 products
  c->slow_amp = a;
  c->fast_amp = f;
  c->slow_phase = slow_phase_;
  c->fast_phase = fast_phase_;
  int32_t target[6];
  const uint32_t offset[3] = { 0u, kThird, kTwoThirds };
  for (int32_t k = 0; k < 3; ++k) {
    const int32_t m = ((a * Sine(slow_phase_ + offset[k])) >> 7) +
                      ((f * Sine(fast_phase_ + offset[k])) >> 7);                  // Q16
    target[k] = (EN_BASE << 16) + m;
    target[3 + k] = (EN_BASE << 16) - m;
  }
  if (first) {
    for (int32_t j = 0; j < 6; ++j) {
      pos_[j] = c->pos0[j] = target[j];
      c->pos_step[j] = 0;
    }
    return;
  }
  const int32_t shift = kShift[n >> 1];
  for (int32_t j = 0; j < 6; ++j) {
    const int32_t d = target[j] - pos_[j];
    const int32_t step =
        shift >= 0 ? d >> shift
                   : static_cast<int32_t>((static_cast<int64_t>(d) * kRecip[n >> 1]) >> 16);
    c->pos0[j] = pos_[j];
    c->pos_step[j] = step;
    pos_[j] += step * n;
  }
}

// --- The ensemble ---

void Ensemble::Init() {
  control_.Init();
  // Both lines a word at a time (word aligned, EN_WORDS even): half the stores of the upstream's halfword loop, as
  // this runs in the audio interrupt (the first render after init, src/half_rate.h).
  typedef uint32_t __attribute__((may_alias)) Word;
  Word* wl = reinterpret_cast<Word*>(line_l_);
  Word* wr = reinterpret_cast<Word*>(line_r_);
  for (int32_t i = 0; i < EN_WORDS / 2; ++i) wl[i] = wr[i] = 0;
  ptr_ = EN_SIZE;
}

namespace {

// m samples into a line from its write pointer down (no wrap: the caller
// splits); the ones below EN_REACH also EN_SIZE further on.
void WriteRun(int16_t* line, const int16_t* x, int32_t ptr, int32_t m) {
  int16_t* q = line + ptr - 1;
  for (int32_t i = 0; i < m; ++i) q[-i] = x[i];
  int32_t i0 = ptr - EN_REACH;
  if (i0 < 0) i0 = 0;
  int16_t* mq = q + EN_SIZE;
  for (int32_t i = i0; i < m; ++i) mq[-i] = x[i];
}

}  // namespace

void Ensemble::Side(EnsembleSideKernel* k, int16_t* out, int32_t m) {
#if defined(__bfin__)
  if (coefs_.one_voice) {
    EnsSide1Asm(k, out, m);
  } else {
    EnsSide3Asm(k, out, m);
  }
#else
  EnsembleSideRunC(k, out, m, coefs_.one_voice ? 1 : 3);
#endif
}

void Ensemble::Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l,
                       int16_t* wet_r, int32_t n, const EnsembleParams& params) {
  control_.Update(params, n, &coefs_);
  const EnsembleCoefs& c = coefs_;
  int16_t* const other = in_r ? line_r_ : line_l_;   // the right input's line (mono: the left)
  int32_t done = 0;
  while (done < n) {
    int32_t m = n - done;
    if (m > ptr_) m = ptr_;
    WriteRun(line_l_, in_l + done, ptr_, m);
    if (in_r) WriteRun(line_r_, in_r + done, ptr_, m);

    EnsembleSideKernel k;
    const int32_t g1 = c.g0[0] + c.g_step[0] * done;
    const int32_t g2 = c.g0[1] + c.g_step[1] * done;
    const int32_t g3 = c.g0[2] + c.g_step[2] * done;
    k.g12 = Pack(g1, g2);
    k.g12_step = Pack(c.g_step[0], c.g_step[1]);
    k.g3 = Pack(g3, c.g_step[2]);
    // Left: voices 1, 2 from the left line, 3 from the right.
    k.line_a = line_l_ + ptr_ - 1;
    k.line_b = other + ptr_ - 1;
    for (int32_t j = 0; j < 3; ++j) {
      k.pos[j] = c.pos0[j] + c.pos_step[j] * done;
      k.step[j] = c.pos_step[j];
    }
    Side(&k, wet_l + done, m);
    if (wet_r) {
      // Right: the sweeps inverted; voices 1, 2 from the right line, 3 from
      // the left.
      k.line_a = other + ptr_ - 1;
      k.line_b = line_l_ + ptr_ - 1;
      for (int32_t j = 0; j < 3; ++j) {
        k.pos[j] = c.pos0[3 + j] + c.pos_step[3 + j] * done;
        k.step[j] = c.pos_step[3 + j];
      }
      Side(&k, wet_r + done, m);
    }
    ptr_ -= m;
    if (ptr_ == 0) ptr_ = EN_SIZE;
    done += m;
  }
}

}  // namespace coop_ensemble
