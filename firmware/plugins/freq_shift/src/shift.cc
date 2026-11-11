// SHIFT: control and the C reference kernels (see shift.h). coop port:
// upstream's te-op1-cfw ports/fx/src/shift/shift.cc (5aa314b, MIT) with
// FreqShift::Init and ::Process changed for a Hilbert pair per channel
// (shift.h), no memset, and the C kernels compiled only where they run.

#include "src/shift.h"

namespace op1fx {

namespace {

// 2^16 / n, n = 0..64 (0 unused).
struct RecipTable {
  int32_t v[SH_MAX_BLOCK + 1];
  constexpr RecipTable() : v() {
    for (int32_t i = 1; i <= SH_MAX_BLOCK; ++i) v[i] = 65536 / i;
  }
};
constexpr RecipTable kRecip;

// 2000 Hz in phase steps: 2000 x 2^32 / 44100, rounded.
const int64_t kInc2kHz = 194783097;
// The feedback knob's top: 0.93 in Q15.
const int32_t kFeedbackMax = 30474;
// Q15 change of the mode gain m per sample: 256 samples end to end.
const int32_t kModeStep = 128;

inline int32_t Sat32(int64_t x) {
  return x > 2147483647LL ? 2147483647 : (x < -2147483648LL ? -2147483647 - 1 : static_cast<int32_t>(x));
}
inline int32_t Sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }
// (min/max: the Blackfin's MIN and MAX instructions, no branches)
inline int32_t Min(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t Max(int32_t a, int32_t b) { return a > b ? a : b; }
inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi) { return Max(lo, Min(x, hi)); }
inline int32_t Lo(uint32_t w) { return static_cast<int16_t>(w & 0xffff); }
inline int32_t Hi(uint32_t w) { return static_cast<int16_t>(w >> 16); }
inline uint32_t Pack(int32_t lo, int32_t hi) {
  return (static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) & 0xffff);
}

// 32-bit Hilbert data (int16 x 2^10) -> int16: x 2^6 saturated, then the
// high half rounded (half up), saturated.
inline int32_t Round16(int32_t x) {
  const int64_t v = static_cast<int64_t>(Sat32(static_cast<int64_t>(x) << 6)) + 0x8000;
  return Sat16(static_cast<int32_t>(v >> 16));
}

#ifdef SHIFT_HAVE_C
// One allpass in z^-2, in place: y = sat(a sat(x + y[n-2]) >> 15 - x[n-2]).
void Section(int32_t* buf, int32_t n, int32_t a, int32_t* st) {
  int32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
  for (int32_t i = 0; i < n; ++i) {
    const int32_t x = buf[i];
    const int32_t t = Sat32(static_cast<int64_t>(x) + y2);
    const int32_t p = static_cast<int32_t>((static_cast<int64_t>(a) * t) >> 15);
    const int32_t y = Sat32(static_cast<int64_t>(p) - x2);
    buf[i] = y;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
  }
  st[0] = x1;
  st[1] = x2;
  st[2] = y1;
  st[3] = y2;
}

#endif

// x / 2^15 rounded to nearest, ties to even (the MAC's default, unbiased
// rounding of its high half), saturated to 16 bits.
inline int32_t Rnd15(int32_t x) {
  int32_t q = x >> 15;
  const int32_t r = x & 0x7fff;
  if (r > 0x4000 || (r == 0x4000 && (q & 1))) ++q;
  return Sat16(q);
}

// g x / 2^15 rounded towards zero (g >= 0).
inline int32_t Scale0(int32_t g, int32_t x) { return (g * x + ((x >> 15) & 32767)) >> 15; }

// The oscillator at a phase: sin (low half) and cos (high half), from
// kShiftOsc with linear interpolation (step x frac, floor).
inline void SinCos(const uint32_t* tab, uint32_t phase, int32_t* s, int32_t* c) {
  const uint32_t* e = tab + (phase >> 23);
  const int32_t frac = static_cast<int32_t>((phase >> 8) & 0x7fff);
  *s = Lo(e[0]) + ((Lo(e[512]) * frac) >> 15);
  *c = Hi(e[0]) + ((Hi(e[512]) * frac) >> 15);
}

// The per-sample step that takes value to target in n samples.
inline int32_t Step(int32_t value, int32_t target, int32_t n) {
  // Rounded towards zero, so n steps never pass the target. (Every delta
  // here fits 32 bits: phase steps differ by < 2^29, the rest by < 2^16.)
  if (value == target) return 0;
  if (n == SH_MAX_BLOCK) {
    const int32_t delta = target - value;
    return delta < 0 ? -((-delta) >> 6) : delta >> 6;
  }
  const int64_t delta = static_cast<int64_t>(target) - value;
  const int64_t step = ((delta < 0 ? -delta : delta) * kRecip.v[n]) >> 16;
  return static_cast<int32_t>(delta < 0 ? -step : step);
}

}  // namespace

#ifdef SHIFT_HAVE_C
void ShiftHilbertC(ShiftHilbertKernel* k, const int16_t* l, const int16_t* r, uint32_t* iq,
                   int32_t* buf, int32_t n) {
  int32_t* bi = buf;
  int32_t* bq = buf + SH_MAX_BLOCK;
  for (int32_t i = 0; i < n; ++i) {
    bi[i] = r ? (static_cast<int32_t>(l[i]) + r[i]) * 512 : static_cast<int32_t>(l[i]) * 1024;
    bq[i] = i ? bi[i - 1] : k->xh[0];
  }
  const int32_t xn[3] = {bi[n - 1], bi[n - 2], n > 2 ? bi[n - 3] : k->xh[0]};
  // Each path's sections in turn; a section's input history is the
  // previous section's output history (the path's input's, for the first).
  for (int32_t path = 0; path < 2; ++path) {
    int32_t x1 = k->xh[path], x2 = k->xh[path + 1];
    for (int32_t s = 0; s < 4; ++s) {
      const int32_t j = 2 * s + path;
      int32_t st[4] = {x1, x2, k->y[j][0], k->y[j][1]};
      x1 = k->y[j][0];
      x2 = k->y[j][1];
      Section(path ? bq : bi, n, k->coef[j], st);
      k->y[j][0] = st[2];
      k->y[j][1] = st[3];
    }
  }
  k->xh[0] = xn[0];
  k->xh[1] = xn[1];
  k->xh[2] = xn[2];
  int32_t m = k->m;
  const bool unity = m == 32768 && k->dm == 0;
  for (int32_t i = 0; i < n; ++i) {
    const int32_t vi = Round16(bi[i]);
    const int32_t vq = Round16(bq[i]);
    const int32_t mq = unity ? vq : (vq * (m < 32767 ? m : 32767)) >> 15;
    m += k->dm;
    iq[i] = Pack(vi, mq);
  }
  k->m = m;
}

void ShiftChannelC(ShiftChannelKernel* k, const uint32_t* iq, int16_t* io, int32_t n,
                   int32_t write, uint32_t* scratch) {
  (void)scratch;   // the assembly's: the oscillator, then the wet signal
  uint32_t phase = k->phase;
  int32_t inc = k->inc, g = k->g, mq = k->mix, mq0 = 32767 - k->mix;
  int32_t lr = Lo(k->lp), li = Hi(k->lp);
  int32_t pos = k->pos;
  for (int32_t i = 0; i < n; ++i) {
    int32_t s, c;
    SinCos(k->osc, phase, &s, &c);
    phase += static_cast<uint32_t>(inc);
    inc += k->dinc;

    const uint32_t fb = k->delay[pos >> 2];
    // Feedback: g fb rounded towards zero (ceil for fb < 0), so the
    // loop's quantisation can't sustain itself: silence in, silence out.
    const int32_t ai = Sat16(Lo(iq[i]) + Scale0(g, Lo(fb)));
    const int32_t aq = Sat16(Hi(iq[i]) + Scale0(g, Hi(fb)));
    g += k->dg;
    // Rotation and loop low-pass: rounded as the MAC does (ties to even).
    const int32_t wr = Rnd15(ai * c - aq * s);
    const int32_t wi = Rnd15(ai * s + aq * c);
    lr = Rnd15(wr * SH_LP_K + lr * SH_LP_K2);
    li = Rnd15(wi * SH_LP_K + li * SH_LP_K2);
    k->delay[pos >> 2] = Pack(lr, li);
    pos = (pos + 4) & (4 * SH_DELAY - 1);

    // Mix: weights 32767 - mix and mix (Q15), floor.
    if (write) io[i] = static_cast<int16_t>((io[i] * mq0 + wr * mq) >> 15);
    mq += k->dmix;
    mq0 -= k->dmix;
  }
  k->phase = phase;
  k->inc = inc;
  k->g = g;
  k->mix = mq;
  k->lp = Pack(lr, li);
  k->pos = pos;
}
#endif  // SHIFT_HAVE_C

int32_t ShiftIncrement(int32_t d) {
  const int64_t d3 = static_cast<int64_t>(d) * d * d;   // |d3| <= 2^45
  return static_cast<int32_t>(((d3 >> 15) * kInc2kHz) >> 30);
}

namespace {

// Word-wise clear and compare (no libc: memset / memcmp are not called).
void ZeroWords(void* p, size_t bytes) {
  uint32_t* w = static_cast<uint32_t*>(p);
  for (size_t i = 0; i < bytes / 4; ++i) w[i] = 0;
}
bool SameWords(const void* a, const void* b, size_t bytes) {
  const uint32_t* x = static_cast<const uint32_t*>(a);
  const uint32_t* y = static_cast<const uint32_t*>(b);
  uint32_t d = 0;
  for (size_t i = 0; i < bytes / 4; ++i) d |= x[i] ^ y[i];
  return d == 0;
}

}  // namespace

void FreqShift::Init() {
  ZeroWords(hk_, sizeof(hk_));
  for (int32_t c = 0; c < 2; ++c) {
    for (int32_t j = 0; j < 8; ++j) hk_[c].coef[j] = kShiftHilbert[j];
    hk_[c].m = 32768;
    ZeroWords(&ck_[c], sizeof(ck_[c]));
    ck_[c].osc = kShiftOsc;
  }
  ZeroWords(buf_, sizeof(buf_));
  ZeroWords(iq_, sizeof(iq_));
  d_[0] = d_[1] = 0;
  inc_[0] = inc_[1] = 0;
  synced_ = 1;   // both pairs start from the same (zero) state
  agree_ = 0;
  pairs_run_ = 0;
  syncs_exact_ = syncs_forced_ = 0;
#if defined(__bfin__)
  use_asm_ = true;
#else
  use_asm_ = false;
#endif
}

// One channel's Hilbert pair over x (upstream's mono call): its mode-gain
// step towards m_end, the kernel, then m at m_end.
void FreqShift::Hilbert(int32_t c, const int16_t* x, int32_t n, int32_t m_end) {
  ShiftHilbertKernel* k = &hk_[c];
  k->dm = Step(k->m, m_end, n);
#ifdef __bfin__
#ifdef SHIFT_HAVE_C
  if (!use_asm_) ShiftHilbertC(k, x, nullptr, iq_[c], buf_, n);
  else
#endif
  ShiftHilbertAsm(k, x, nullptr, iq_[c], buf_, n);
#else
  ShiftHilbertC(k, x, nullptr, iq_[c], buf_, n);
#endif
  k->m = m_end;
}

uint32_t FreqShift::Process(int16_t* l, int16_t* r, int32_t n, const ShiftParams& p, bool same) {
  if (n <= 0) return 0;
  if (n > SH_MAX_BLOCK) n = SH_MAX_BLOCK;

  const int32_t d = ShiftKnobToD(Clamp(p.shift, 0, 32767));
  const int32_t s = ShiftSpreadToD(Clamp(p.spread, 0, 32767));
  // The phase steps (L: shift + spread, R: shift - spread), recomputed
  // only when their position changes.
  const int32_t dc[2] = {Clamp(d + s, -32768, 32767), Clamp(d - s, -32768, 32767)};
  for (int32_t c = 0; c < 2; ++c) {
    if (dc[c] != d_[c]) {
      d_[c] = dc[c];
      inc_[c] = ShiftIncrement(dc[c]);
    }
  }
  const int32_t g = (Clamp(p.feedback, 0, 32767) * kFeedbackMax) >> 15;
  const int32_t mix = Clamp(p.mix, 0, 32767);

  // The mode gain: towards 0 (ring) or 32768 (shift), 128 a sample at most.
  // Both pairs' m take the same steps every block, so the left pair's m
  // stands for both.
  const int32_t m_target = p.ring ? 0 : 32768;
  const int32_t m_max = kModeStep * n;
  const int32_t m_end = hk_[0].m + Clamp(m_target - hk_[0].m, -m_max, m_max);

  // The Hilbert pairs. The right pair's state is its own unless synced_,
  // when it is (logically) the left one's.
  const uint32_t* iq[2] = {iq_[0], iq_[1]};
  if (same && synced_) {
    Hilbert(0, l, n, m_end);
    iq[1] = iq_[0];
    pairs_run_ = 1;
  } else {
    if (synced_) {
      hk_[1] = hk_[0];
      synced_ = 0;
    }
    Hilbert(0, l, n, m_end);
    Hilbert(1, r, n, m_end);
    pairs_run_ = 2;
    if (!same) {
      agree_ = 0;
    } else if (SameWords(&hk_[0], &hk_[1], sizeof(hk_[0]))) {
      synced_ = 1;   // exactly the left pair from here on: one pair is the same sound
      agree_ = 0;
      ++syncs_exact_;
    } else if (SameWords(iq_[0], iq_[1], 4 * n)) {
      if (++agree_ >= kShiftAgreeBlocks) {
        synced_ = 1;   // the right pair takes the left one's state at its next run
        agree_ = 0;
        ++syncs_forced_;
      }
    } else {
      agree_ = 0;
    }
  }

  // The input gain: the analytic signal, both halves, saturating.
  if (p.gain != 16384) {
    const int32_t gq = Clamp(p.gain, 0, 65536);
    const int32_t pairs = iq[1] == iq[0] ? 1 : 2;
    for (int32_t c = 0; c < pairs; ++c) {
      uint32_t* v = iq_[c];
      for (int32_t i = 0; i < n; ++i) {
        const int32_t vi = Sat16((Lo(v[i]) * gq) >> 14);
        const int32_t vq = Sat16((Hi(v[i]) * gq) >> 14);
        v[i] = Pack(vi, vq);
      }
    }
  }

  uint32_t written = 0;
  for (int32_t c = 0; c < 2; ++c) {
    ShiftChannelKernel* k = &ck_[c];
    k->dinc = Step(k->inc, inc_[c], n);
    k->dg = Step(k->g, g, n);
    k->dmix = Step(k->mix, mix, n);
    const int32_t write = !(k->mix == 0 && mix == 0);
    int16_t* io = c ? r : l;
#ifdef __bfin__
#ifdef SHIFT_HAVE_C
    if (!use_asm_) ShiftChannelC(k, iq[c], io, n, write, reinterpret_cast<uint32_t*>(buf_));
    else
#endif
    ShiftChannelAsm(k, iq[c], io, n, write, reinterpret_cast<uint32_t*>(buf_));
#else
    ShiftChannelC(k, iq[c], io, n, write, reinterpret_cast<uint32_t*>(buf_));
#endif
    k->inc = inc_[c];
    k->g = g;
    k->mix = mix;
    if (write) written |= 1u << c;
  }
  return written;
}

}  // namespace op1fx
