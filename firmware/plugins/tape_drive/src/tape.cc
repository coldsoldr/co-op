// coop: copied from te-op1-cfw ports/fx/src/tape; only the include paths changed.
//
// TAPE: control, C reference kernels and Process (src/tape/tape.h).

#include "src/tape.h"

#include "src/tape_tables.h"

namespace op1fx {

namespace {

// --- The Blackfin's 16-bit arithmetic, as the C references model it -------

inline int32_t Sat16(int32_t x) {
  return x > 32767 ? 32767 : (x < -32768 ? -32768 : x);
}
inline int32_t Lo(int32_t w) { return static_cast<int16_t>(w & 0xffff); }
inline int32_t Hi(int32_t w) { return w >> 16; }
inline int32_t Pack(int32_t hi, int32_t lo) {
  return static_cast<int32_t>((static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) & 0xffffu));
}
// A fractional product into a data register: 2ab, -1 x -1 saturated.
inline int64_t Prod(int32_t a, int32_t b) {
  int64_t p = 2 * static_cast<int64_t>(a) * b;
  return p > 0x7fffffff ? 0x7fffffff : p;
}
// The same into an accumulator: exact (-1 x -1 is +2^31 there).
inline int64_t ProdA(int32_t a, int32_t b) { return 2 * static_cast<int64_t>(a) * b; }
// An accumulator's high half, rounded to nearest with ties to even (the
// default, unbiased rounding: RND_MOD = 0) and saturated.
inline int32_t Ext(int64_t acc) {
  int64_t r = (acc + 0x8000) >> 16;
  if ((acc & 0xffff) == 0x8000) r &= ~static_cast<int64_t>(1);
  return r > 32767 ? 32767 : (r < -32768 ? -32768 : static_cast<int32_t>(r));
}
// A 16 x 16 fractional multiply into a half register.
inline int32_t MulR(int32_t a, int32_t b) { return Ext(Prod(a, b)); }
// An accumulator's high half, truncated (T) and saturated.
inline int32_t ExtT(int64_t acc) {
  const int64_t r = acc >> 16;
  return r > 32767 ? 32767 : (r < -32768 ? -32768 : static_cast<int32_t>(r));
}
// x << s, saturated (s >= 0).
inline int32_t ShlSat(int32_t x, int32_t s) { return Sat16(x * (1 << s)); }

// The head bump's low-passes: 1 - exp(-2 pi f / 44100), Q15.
const int32_t kBumpKa = 830;      // 180 Hz
const int32_t kBumpKb = 209;      // 45 Hz

// The wobble: depths in Q8 samples (the delay itself in Q16).
const int32_t kWowMax = 118 << 8;        // wow 0.5 Hz (x 0.5 .. 2 by the drift)
const int32_t kFlutterMax = 1203;        // 4.7 samples; flutter 7 Hz (x 0.9 .. 1.1)
const int32_t kDelayMax = 250 << 16;

// A value known to fit 16 bits, typed so that the Blackfin's 16 x 16
// multiply does the product.
inline int16_t Q15(int32_t x) { return static_cast<int16_t>(x); }

// The unipolar sine of a phase (Q32 turns): (1 + sin) / 2, Q15 (0..32767).
inline int32_t Uni(uint32_t ph) {
  const int32_t i = static_cast<int32_t>(ph >> 24);
  const int32_t fr = static_cast<int32_t>((ph >> 9) & 0x7fff);
  const int32_t a = kTapeSine[i];
  const int32_t s = a + ((Q15(kTapeSine[i + 1] - a) * Q15(fr)) >> 15);
  return (s + 32767) >> 1;
}

// A phase increment given per 64-sample call, for a call of n samples.
inline uint32_t PerCall(uint32_t inc64, int32_t n) {
  return (inc64 >> 6) * static_cast<uint32_t>(n);
}

int32_t Clamp15(int32_t x) { return x < 0 ? 0 : (x > 32767 ? 32767 : x); }

// One knob's smoother (Q15 << 8): 1/8 per update (every other call), snapping within 8 LSB;
// true once it's there.
inline bool Smooth(int32_t* s, int32_t target, bool snap) {
  const int32_t t = Clamp15(target) << 8;
  const int32_t d = t - *s;
  if (snap || (d < 2048 && d > -2048)) {
    *s = t;
    return true;
  }
  *s += d >> 3;
  return false;
}

// 1/8 of the way per call, at most lim, at least 1.
inline void Slew(int32_t* v, int32_t target, int32_t lim) {
  const int32_t d = target - *v;
  int32_t s = d >> 3;
  if (s == 0) s = d;
  if (s > lim) s = lim;
  if (s < -lim) s = -lim;
  *v += s;
}

}  // namespace

// --- C reference kernels ---------------------------------------------------

// Each sample: write at widx (one down), then read at pos (one down plus
// the step): x0 at idx = pos >> 16, x1 one older; weights (Q14)
// (65535 - f) >> 2 and f >> 2, f = pos's low half (they sum to 16383 or
// 16382: the output is at half scale).
void TapeDelayRunC(TapeDelayKernel* k, int32_t* ring, const int16_t* l, const int16_t* r,
                   int16_t* wl, int16_t* wr, int32_t n) {
  int32_t widx = k->widx;
  uint32_t pos = k->pos;
  const uint32_t inc = static_cast<uint32_t>(k->step) - 65536u;
  for (int32_t i = 0; i < n; ++i) {
    widx = (widx - 1) & (TAPE_RING - 1);
    if (r) {
      ring[widx] = ring[widx + TAPE_RING] = Pack(r[i], l[i]);
    } else {
      ring[widx] = ring[widx + TAPE_RING] = l[i] & 0xffff;   // (0 in the high half)
    }
    pos += inc;
    const int32_t idx = static_cast<int32_t>((pos >> 16) & (TAPE_RING - 1));
    const int32_t f = static_cast<int32_t>(pos & 0xffff);
    const int32_t w1 = f >> 2, w0 = (f ^ 0xffff) >> 2;
    const int32_t x0 = ring[idx], x1 = ring[idx + 1];
    wl[i] = static_cast<int16_t>(Ext(ProdA(Lo(x0), w0) + ProdA(Lo(x1), w1)));
    if (r) wr[i] = static_cast<int16_t>(Ext(ProdA(Hi(x0), w0) + ProdA(Hi(x1), w1)));
  }
  k->widx = widx;
  k->pos = pos;
}

namespace {

// The two allpass sections of one lane: y1 = x1 + c1 (x - s1),
// y2 = s1 + c2 (y1 - s2). The second rounds its product towards zero: its
// larger coefficients (c2, c3) would otherwise sustain limit cycles of up
// to 3 LSB in silence; the first's (c0, c1 < 0.3) can't. As the kernels do
// it: d = y1 - s2, f = d < 0 ? -1 : 0, y2 = trunc(2 c d + 2 (f - s1) x -1.0),
// all in one accumulator.
inline void Allpasses(int32_t x, int32_t x1, int32_t* s1, int32_t* s2, int32_t c1, int32_t c2,
                      int32_t* y2) {
  const int32_t y1 = Sat16(x1 + MulR(Sat16(x - *s1), c1));
  const int32_t d = Sat16(y1 - *s2);
  const int32_t t = Sat16((d < 0 ? -1 : 0) - *s1);
  *y2 = ExtT(ProdA(d, c2) + ProdA(t, -32768));
  *s1 = y1;
  *s2 = *y2;
}

}  // namespace

void TapeUpRunC(TapeHbKernel* k, const int16_t* x, int32_t* u, int32_t n) {
  int32_t x1 = Lo(k->x);
  int32_t s1l = Lo(k->s1), s1h = Hi(k->s1), s2l = Lo(k->s2), s2h = Hi(k->s2);
  const int32_t c1l = Lo(k->c1), c1h = Hi(k->c1), c2l = Lo(k->c2), c2h = Hi(k->c2);
  u[-1] = Pack(s2l, u[-1]);   // scratch, as the kernel's store stream starts
  for (int32_t i = 0; i < n; ++i) {
    const int32_t xi = x[i];
    const int32_t prev_odd = s2h;
    int32_t even, odd;
    Allpasses(xi, x1, &s1l, &s2l, c1l, c2l, &even);
    Allpasses(xi, x1, &s1h, &s2h, c1h, c2h, &odd);
    x1 = xi;
    u[i] = Pack(even, prev_odd);
  }
  k->x = Pack(x1, x1);
  k->s1 = Pack(s1h, s1l);
  k->s2 = Pack(s2h, s2l);
}

void TapeSatRunC(const TapeSatKernel* k, int32_t* u, int32_t n) {
  const int32_t h = Hi(k->hsh), sh = Lo(k->hsh);
  const int32_t k1 = Hi(k->k12), k2 = Lo(k->k12), k0 = Hi(k->k0), one = Lo(k->k0);
  const int32_t j = k->j;
  u[-1] = 0;   // scratch (the kernel stores each result during the next)
  for (int32_t i = 0; i < n; ++i) {
    int32_t out[2];
    for (int32_t lane = 0; lane < 2; ++lane) {
      const int32_t v = lane ? Hi(u[i]) : Lo(u[i]);
      const int32_t z = ShlSat(MulR(v, h), sh);
      const int32_t xq = MulR(z, z);
      const int32_t x2 = MulR(xq, xq);
      const int32_t t = Ext(ProdA(xq, k1) + ProdA(x2, k2) + ProdA(k0, one));
      out[lane] = ShlSat(MulR(z, t), j);
    }
    u[i] = Pack(out[1], out[0]);
  }
}

void TapeDownRunC(TapeHbKernel* k, const int32_t* u, int16_t* y, int32_t n) {
  int32_t x1l = Lo(k->x), x1h = Hi(k->x);
  int32_t s1l = Lo(k->s1), s1h = Hi(k->s1), s2l = Lo(k->s2), s2h = Hi(k->s2);
  y[-1] = static_cast<int16_t>(Sat16(s2l + s2h));   // scratch, as the kernel's
  const int32_t c1l = Lo(k->c1), c1h = Hi(k->c1), c2l = Lo(k->c2), c2h = Hi(k->c2);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t ql = Lo(u[i]);   // the previous odd sample
    const int32_t qh = Hi(u[i]);   // this pair's even one
    int32_t a, b;
    Allpasses(ql, x1l, &s1l, &s2l, c1l, c2l, &a);
    Allpasses(qh, x1h, &s1h, &s2h, c1h, c2h, &b);
    x1l = ql;
    x1h = qh;
    y[i] = static_cast<int16_t>(Sat16(a + b));
  }
  k->x = Pack(x1h, x1l);
  k->s1 = Pack(s1h, s1l);
  k->s2 = Pack(s2h, s2l);
}

namespace {

int32_t Sat32(int64_t x) {
  return x > 0x7fffffff ? 0x7fffffff : (x < -0x7fffffffLL - 1 ? -0x7fffffff - 1 : static_cast<int32_t>(x));
}

// A one-pole with an offset Q16 state in a data register: its high half is
// the rounded output; e = x - that, s += 2 e k (fractional product).
inline void Pole32(int32_t* s, int32_t x, int32_t k) {
  *s = Sat32(static_cast<int64_t>(*s) + Prod(Sat16(x - Hi(*s)), k));
}

}  // namespace

void TapeToneRunC(TapeToneKernel* k, int16_t* w, int16_t* io, int32_t n) {
  int64_t a0 = k->a0, a1 = k->a1;
  int32_t lpa = Lo(k->lp), lpb = Hi(k->lp), s1 = k->s1, s2 = k->s2;
  const int32_t ka = Lo(k->kab), kb = Hi(k->kab), kk = Lo(k->gk), gb = Hi(k->gk);
  int32_t m = Lo(k->mix);
  const int32_t step = Hi(k->mix);
  w[-1] = static_cast<int16_t>(Hi(s2));   // scratch: the kernel's stores lag one
  for (int32_t i = 0; i < n; ++i) {
    const int32_t x = w[i];
    const int32_t dry = io[i];
    // The bump: two one-poles from x in the accumulators.
    a0 += ProdA(Sat16(x - lpa), ka);
    a1 += ProdA(Sat16(x - lpb), kb);
    lpa = Ext(a0);
    lpb = Ext(a1);
    const int32_t y = Sat16(x + MulR(Sat16(lpa - lpb), gb));
    // HF loss.
    Pole32(&s1, y, kk);
    Pole32(&s2, Hi(s1), kk);
    m = Sat16(m + step);
    // dry (1 - m) + wet m: each partial sum stays in range.
    io[i] = static_cast<int16_t>(Sat16(Sat16(dry - MulR(dry, m)) + MulR(Hi(s2), m)));
    w[i] = static_cast<int16_t>(Hi(s2));
  }
  k->a0 = Sat32(a0);   // (a 32-bit move from the accumulator saturates)
  k->a1 = Sat32(a1);
  k->lp = Pack(lpb, lpa);
  k->s1 = s1;
  k->s2 = s2;
  k->mix = Pack(step, m);
}

void TapeLaneRunC(TapeLaneKernel* k, const TapeSatKernel* sat, int16_t* w, int32_t* u, int16_t* io,
                  int32_t n) {
  TapeUpRunC(&k->up, w, u, n);
  TapeSatRunC(sat, u, n);
  TapeDownRunC(&k->down, u, w, n);
  TapeToneRunC(&k->tone, w, io, n);
}

// --- TapeSat ---------------------------------------------------------------

void TapeSat::Init() {
  sat_ = age_ = wob_ = 0;
  mix_ = 0;
  aw_ = af_ = 0;
  sat_done_ = age_done_ = wob_done_ = -1;
  gain_done_ = 0x7fffffff;
  aw_t_ = af_t_ = 0;
  ph_wow_ = ph_flut_ = 0xc0000000u;   // the sines' minimum: the delay starts at 0
  ph_rnd_ = 0;
  rnd_inc_ = 779136;                  // 0.125 Hz, per 64 samples
  rng_ = 0x2545f491u;
  started_ = false;
  settled_ = false;
  lfo_call_ = true;
  last_.saturate = last_.wobble = last_.age = last_.mix = -1;
  last_.gain_oct = 0x7fffffff;
  mono_ = false;
  use_asm_ = true;
  dk_.pos = 0;
  dk_.step = 0;
  dk_.widx = 0;
  sk_.hsh = sk_.k12 = sk_.k0 = sk_.j = 0;
  for (int32_t lane = 0; lane < 2; ++lane) {
    TapeLaneKernel* lk = &lane_[lane];
    lk->up.x = lk->up.s1 = lk->up.s2 = 0;
    lk->down.x = lk->down.s1 = lk->down.s2 = 0;
    // Up: low = the even path (c0, c2), high = the odd one (c1, c3). Down:
    // its input pairs are (even | previous odd), so the other way round.
    lk->up.c1 = Pack(kTapeHalfband[1], kTapeHalfband[0]);
    lk->up.c2 = Pack(kTapeHalfband[3], kTapeHalfband[2]);
    lk->down.c1 = Pack(kTapeHalfband[0], kTapeHalfband[1]);
    lk->down.c2 = Pack(kTapeHalfband[2], kTapeHalfband[3]);
    TapeToneKernel* t = &lk->tone;
    t->a0 = t->a1 = t->lp = 0;
    t->s1 = t->s2 = 0x8000;
    t->kab = Pack(kBumpKb, kBumpKa);
    t->gk = Pack(0, 32767);
    t->mix = 0;
  }
  for (int32_t i = 0; i < TAPE_RING_WORDS; ++i) ring_[i] = 0;
}

// 32-bit throughout: the control runs on the Blackfin every call.

// The wobble knob's depths; true once they have reached their targets.
bool TapeSat::Depths(int32_t n) {
  if (wob_ != wob_done_) {
    wob_done_ = wob_;
    const int32_t w = wob_ >> 8;   // Q15
    const int32_t t = w >= 16384 ? 32767 : 2 * w;
    const int32_t fade = w >= 16384 ? 2 * w - 32767 : 0;   // Q15: the flutter half
    // Wow: rises over the bottom half (squared), then gives way to flutter
    // (to 0.4 of its depth at the top). Depths in Q8 samples.
    const int32_t tt = (Q15(t) * Q15(t)) >> 15;
    aw_t_ = (((Q15(kWowMax) * Q15(tt)) >> 15) * Q15(32767 - ((Q15(19661) * Q15(fade)) >> 15))) >> 15;
    af_t_ = (Q15(kFlutterMax) * Q15(fade)) >> 15;
  }
  if (!started_) {
    aw_ = aw_t_;
    af_ = af_t_;
  } else {
    // 1/8 per update, at most 0.2 samples per 64 (a turn's pitch glide stays
    // under about 11 cents).
    const int32_t lim = (51 * n) >> 6;
    Slew(&aw_, aw_t_, lim);
    Slew(&af_, af_t_, lim);
  }
  return aw_ == aw_t_ && af_ == af_t_;
}

// The wobble's LFOs, every other call: the delay two calls of n on, and
// the step there (for this call and the next).
void TapeSat::Lfo(int32_t n) {
  const int32_t span = 2 * n;
  // (The products are 16 x 16 where the Blackfin can do them in one.)
  // The drift (Beads): a slow sine whose rate is redrawn (0.05..0.25 Hz)
  // once a cycle; it speeds the wow up (x 0.5..2) and makes it shallower.
  const uint32_t prev = ph_rnd_;
  ph_rnd_ += PerCall(rnd_inc_, span);
  if (prev < 0x40000000u && ph_rnd_ >= 0x40000000u) {
    rng_ = rng_ * 1664525u + 1013904223u;
    const int32_t hz = 3277 + static_cast<int32_t>(((rng_ >> 16) * 13107u) >> 16);   // Q16
    rnd_inc_ = static_cast<uint32_t>(hz * 6087) >> 6;   // x 64 2^32 / 44100 / 2^16
  }
  const int32_t rm = Uni(ph_rnd_);
  const int32_t rm2 = (Q15(rm) * Q15(rm)) >> 15;
  const int32_t fm = 4096 + ((3 * rm2) >> 3);                       // Q13: 0.5 + 1.5 rm^2
  ph_wow_ += PerCall(static_cast<uint32_t>(12174 * Q15(fm)) >> 5, span);   // 0.5 Hz x fm
  const int32_t ff = 14746 + ((Q15(3277) * Q15(rm)) >> 15);         // Q14: 0.9 + 0.2 rm
  ph_flut_ += PerCall(static_cast<uint32_t>(10652 * Q15(ff)) >> 2, span);  // 7 Hz x ff
  const int32_t amount = 16384 - ((Q15(6554) * Q15(rm)) >> 15);     // Q14: 1 - 0.4 rm

  // The delay (Q8): wow (1 + sin) x amount + flutter (1 + sin).
  const int32_t dw = (((Q15(aw_) * Q15(Uni(ph_wow_))) >> 15) * Q15(amount)) >> 13;
  const int32_t df = (Q15(af_) * Q15(Uni(ph_flut_))) >> 14;
  int32_t d = (dw + df) << 8;   // Q16
  if (d > kDelayMax) d = kDelayMax;
  int32_t diff = (d - delay_q16()) >> 1;   // over 2n: half of it per n
  if (diff > (1 << 21)) diff = 1 << 21;   // at most 32 samples a call
  if (diff < -(1 << 21)) diff = -(1 << 21);
  dk_.step = n == 64 ? diff >> 6 : (diff * (kTapeRecip[n] >> 6)) >> 10;
}

// The control runs at half the call rate, in two halves on alternate calls
// (the knobs' smoothing on one, the wobble's LFOs on the other), so that
// no call pays for both; the mix ramps every call.
void TapeSat::Control(const TapeParams& p, int32_t n) {
  const bool snap = !started_;
  if (snap) {
    Knobs(p, 2 * n, true);
    lfo_call_ = true;
  }
  if (lfo_call_) {
    Lfo(n);
  } else {
    // Still knobs and settled smoothers: nothing to do.
    const bool same = p.saturate == last_.saturate && p.wobble == last_.wobble && p.age == last_.age &&
                      p.gain_oct == last_.gain_oct;
    if (!same || !settled_) Knobs(p, 2 * n, false);
  }
  lfo_call_ = !lfo_call_;
  Mix(p, n, snap);
  started_ = true;
}

void TapeSat::Knobs(const TapeParams& p, int32_t n, bool snap) {
  last_ = p;
  bool there = Smooth(&sat_, p.saturate, snap);
  there &= Smooth(&age_, p.age, snap);
  there &= Smooth(&wob_, p.wobble, snap);

  // Saturation: drive g = 2^e / 2, e = -2 + 6 s. z = 2 g u 2^-c (Q15, c <= 0
  // keeps small drives' precision), as (u h) << shift, h in [0.5, 1).
  if (sat_ != sat_done_ || p.gain_oct != gain_done_) {
    sat_done_ = sat_;
    gain_done_ = p.gain_oct;
    const int32_t s = sat_ >> 8;
    // The input gain adds to the exponent, kept within the knob's own range.
    int32_t e = -131072 + 12 * s + p.gain_oct;
    if (e < -131072) e = -131072;
    if (e > 262144) e = 262144;
    const int32_t fl = e >> 16, fr = e & 0xffff;
    const int32_t i = fr >> 11, wgt = fr & 2047;
    int32_t h = kTapeExp2[i] + (((kTapeExp2[i + 1] - kTapeExp2[i]) * wgt) >> 11);
    if (h > 32767) h = 32767;
    const int32_t shift = fl >= 0 ? fl + 1 : 0;
    const int32_t c = fl >= 0 ? 0 : fl + 1;   // -1 or 0
    const int32_t mi = s >> 10, mw = s & 1023;
    const int32_t m = kTapeMakeup[mi] + (((kTapeMakeup[mi + 1] - kTapeMakeup[mi]) * mw) >> 10);
    // T = M R 2^(c-1) 2^-j = K0 + K1 X + K2 X^2, X = z^2 (Q15).
    int32_t k0 = m << (2 + c);
    int32_t j = 0;
    while (k0 > 32767) {
      k0 >>= 1;
      ++j;
    }
    const int32_t k1 = -(((k0 * 21845) + 16384) >> 15) >> (-2 * c);
    const int32_t k2 = (((k0 * 6554) + 16384) >> 15) >> (-4 * c);
    sk_.hsh = Pack(h, shift);
    sk_.k12 = Pack(k1, k2);
    sk_.k0 = Pack(k0, 32767);
    sk_.j = j;
  }

  // Age: the HF loss's coefficient, the bump's gain a (2 - a).
  if (age_ != age_done_) {
    age_done_ = age_;
    const int32_t a = age_ >> 8;
    const int32_t ai = a >> 10, aw = a & 1023;
    const int32_t k = kTapeAgeK[ai] + (((kTapeAgeK[ai + 1] - kTapeAgeK[ai]) * aw) >> 10);
    int32_t gb = (a * (65536 - a)) >> 15;
    if (gb > 32767) gb = 32767;
    lane_[0].tone.gk = lane_[1].tone.gk = Pack(gb, k);
  }

  settled_ = Depths(n) && there;
}

// Mix: 1/8 per call, ramped per sample; within n LSB it lands exactly.
void TapeSat::Mix(const TapeParams& p, int32_t n, bool snap) {
  {
    const int32_t target = Clamp15(p.mix);
    int32_t step = 0;
    if (snap) {
      mix_ = target;
    } else {
      const int32_t d = target - mix_;
      if (d < n && d > -n) {
        mix_ = target;
      } else {
        int32_t next = mix_ + (d >> 3);
        if (d > 0 && next < mix_ + n) next = mix_ + n;
        if (d < 0 && next > mix_ - n) next = mix_ - n;
        step = n == 64 ? (next - mix_) >> 6 : ((next - mix_) * kTapeRecip[n]) >> 16;
      }
    }
    lane_[0].tone.mix = lane_[1].tone.mix = Pack(step, mix_);
    mix_ += step * n;
  }
}

void TapeSat::CopyLeftToRight() {
  for (int32_t i = 0; i < TAPE_RING_WORDS; ++i) ring_[i] = Pack(ring_[i], ring_[i]);
  lane_[1] = lane_[0];
}

void TapeSat::Lane(int32_t lane, int16_t* io, int32_t n) {
  int16_t* w = w_[lane] + 2;   // w[-1] scratch, w[n] read
  int32_t* u = u_[lane] + 1;   // u[-1] scratch, u[n] read
#if defined(__bfin__)
  if (use_asm_) {
    TapeLaneAsm(&lane_[lane], &sk_, w, u, io, n);
    return;
  }
#endif
  TapeLaneRunC(&lane_[lane], &sk_, w, u, io, n);
}

void TapeSat::Process(int16_t* l, int16_t* r, int32_t n, const TapeParams& p) {
  if (n <= 0) return;
  if (r && mono_ && started_) CopyLeftToRight();
  mono_ = r == nullptr;
  Control(p, n);
  bool done = false;
#if defined(__bfin__)
  if (use_asm_) {
    // The kernel's write index mustn't wrap within a run.
    for (int32_t at = 0; at < n;) {
      int32_t run = n - at;
      if (dk_.widx == 0) dk_.widx = TAPE_RING;   // (the same slot: 256 & 255 = 0)
      if (run > dk_.widx) run = dk_.widx;
      if (r) {
        TapeDelayRunAsm(&dk_, ring_, l + at, r + at, w_[0] + 2 + at, w_[1] + 2 + at, run);
      } else {
        TapeDelayRunMonoAsm(&dk_, ring_, l + at, w_[0] + 2 + at, run);
      }
      at += run;
    }
    done = true;
  }
#endif
  if (!done) TapeDelayRunC(&dk_, ring_, l, r, w_[0] + 2, w_[1] + 2, n);
  Lane(0, l, n);
  if (r) Lane(1, r, n);
}

}  // namespace op1fx
