// FOLD: control, C reference kernels, Process (fold.h, te-op1-cfw
// docs/fold-port.md). coop copy: the C kernels are built for the host only
// (FOLD_C_KERNELS); the Blackfin build runs the assembly.

#include "src/fold.h"

#include "src/fold_tables.h"

#if !defined(__bfin__) || defined(OP1FX_NO_ASM)
#define FOLD_C_KERNELS 1
#else
#define FOLD_C_KERNELS 0
#endif

namespace op1fx {

namespace {

const int32_t kSmoothShift = 2;     // knobs: 1/4 of the way per call
const int32_t kSmoothSnap = 4;      // ... and snap when this close
const int32_t kDcCoef = 47;         // DC blocker: 1 - exp(-2 pi 10 / 44100), Q15

#if FOLD_C_KERNELS
inline int32_t Sat16(int32_t x) {
  return x > 32767 ? 32767 : (x < -32768 ? -32768 : x);
}
#endif

// A 33-point knob table at k (0..32767).
inline int32_t Curve(const int32_t* t, int32_t k) {
  const int32_t i = k >> 10, f = k & 1023;
  return t[i] + (((t[i + 1] - t[i]) * f) >> 10);
}

// (to - from) / n towards zero, so a ramp never overshoots its end. 32-bit
// only (a 64-bit multiply is a libgcc call on the Blackfin): |d| x 1/n in
// two 16-bit halves, exactly floor(|d| x recip / 2^16).
inline int32_t Step(int32_t from, int32_t to, int32_t n) {
  if (from == to) return 0;
  const uint32_t a = to < from ? static_cast<uint32_t>(from) - static_cast<uint32_t>(to)
                               : static_cast<uint32_t>(to) - static_cast<uint32_t>(from);
  const uint32_t r = static_cast<uint32_t>(kFoldRecip[n]);
  const uint32_t s = (a >> 16) * r + (((a & 0xffff) * r) >> 16);
  return to < from ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
}

#if FOLD_C_KERNELS
// The MAC's (S2RND) extraction of a 40-bit accumulator: x 2, rounded to the
// high half (unbiased), saturated.
inline int32_t S2Rnd(int64_t acc) {
  const int64_t x = acc * 2;
  int64_t r = x >> 16;
  const int32_t rem = static_cast<int32_t>(x & 0xffff);
  if (rem > 0x8000 || (rem == 0x8000 && (r & 1))) ++r;
  if (r > 32767) r = 32767;
  if (r < -32768) r = -32768;
  return static_cast<int32_t>(r);
}

// One sample of the tone kernels, one channel. sc: Q14; mix: the packed
// ramp value; a, kdc: Q15.
inline int16_t ToneSample(FoldChannel* ch, int32_t w, int32_t sc, uint32_t mix, int32_t a, int32_t kdc,
                          int32_t dry) {
  const int32_t wv = static_cast<int16_t>(w - sc);
  const int32_t d1 = static_cast<int16_t>(wv - ch->t1);
  ch->t1 = static_cast<int16_t>(ch->t1 + FoldMulFrac(d1, a));
  const int32_t d2 = static_cast<int16_t>(ch->t1 - ch->t2);
  ch->t2 = static_cast<int16_t>(ch->t2 + FoldMulFrac(d2, a));
  const int32_t e = Sat16(ch->t2 - (ch->z >> 16));
  ch->z += 2 * e * kdc;
  const int32_t lm = static_cast<int16_t>(mix >> 16), mm = static_cast<int16_t>(mix & 0xffff);
  const int64_t acc = static_cast<int64_t>(dry) * mm + 2 * static_cast<int64_t>(e) * lm;
  return static_cast<int16_t>(S2Rnd(acc));
}
#endif  // FOLD_C_KERNELS

}  // namespace

int32_t FoldSinQ15(uint32_t phase) {
  return FoldInterp(kFoldSin[phase >> 24], static_cast<int32_t>((phase >> 9) & 0x7fff));
}

#if FOLD_C_KERNELS
void FoldShapeRunC(const FoldShapeBlock* k, FoldChannel* ch, const int16_t* x, int16_t* w,
                   int32_t stride, int32_t n) {
  uint32_t g = static_cast<uint32_t>(k->g), c = static_cast<uint32_t>(k->c);
  int32_t up = ch->u_prev;
  for (int32_t i = 0; i < n; ++i) {
    g += static_cast<uint32_t>(k->g_step);
    c += static_cast<uint32_t>(k->c_step);
    const int32_t u = x[i] * (static_cast<int32_t>(g) >> 16) + static_cast<int32_t>(c);
    const uint32_t s = static_cast<uint32_t>(u) + static_cast<uint32_t>(up);
    const int32_t d = u - up;
    up = u;
    // sin(pi (u + u')): phase (u + u') << 4 in Q32 turns, i.e. bits 20..27
    // of the sum pick the segment, bits 5..19 are the fraction.
    const int32_t sn = FoldInterp(k->sin_tab[(s >> 20) & 255], static_cast<int32_t>((s >> 5) & 0x7fff));
    // sinc(pi (u - u')): 128 segments per turn of |u - u'|.
    // |u - u'| < 2^30 + 2^25, so the segment is at most 1056 (of 1064).
    const int32_t ad = d < 0 ? -d : d;
    const int32_t sk = FoldInterp(k->sinc_tab[ad >> 20], (ad >> 5) & 0x7fff);
    w[i * stride] = static_cast<int16_t>(FoldMulFrac(sn, sk));
  }
  ch->u_prev = up;
}

void FoldToneRunC(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w, int16_t* io,
                  int32_t n) {
  uint32_t sc = static_cast<uint32_t>(k->sc), mix = static_cast<uint32_t>(k->mix);
  const int32_t a = static_cast<int16_t>(k->coef & 0xffff), kdc = k->coef >> 16;
  for (int32_t i = 0; i < n; ++i) {
    sc += static_cast<uint32_t>(k->sc_step);
    mix += static_cast<uint32_t>(k->mix_step);
    io[i] = ToneSample(ch, w[i], static_cast<int32_t>(sc) >> 16, mix, a, kdc, io[i]);
  }
}

void FoldToneStereoRunC(const FoldToneBlock* k, FoldChannel* ch, const int16_t* w, int16_t* l,
                        int16_t* r, int32_t n) {
  uint32_t sc = static_cast<uint32_t>(k->sc), mix = static_cast<uint32_t>(k->mix);
  const int32_t a = static_cast<int16_t>(k->coef & 0xffff), kdc = k->coef >> 16;
  for (int32_t i = 0; i < n; ++i) {
    sc += static_cast<uint32_t>(k->sc_step);
    mix += static_cast<uint32_t>(k->mix_step);
    const int32_t s = static_cast<int32_t>(sc) >> 16;
    l[i] = ToneSample(&ch[0], w[2 * i], s, mix, a, kdc, l[i]);
    r[i] = ToneSample(&ch[1], w[2 * i + 1], s, mix, a, kdc, r[i]);
  }
}
#endif  // FOLD_C_KERNELS

void Fold::Init() {
  for (int32_t i = 0; i < 2; ++i) {
    ch_[i].u_prev = 0;
    ch_[i].t1 = ch_[i].t2 = 0;
    ch_[i].z = 0;
  }
  for (int32_t i = 0; i < 5; ++i) knob_[i] = last_[i] = -1;
  g_ = c_ = sc_ = lm_ = mm_ = 0;
  shape_.sin_tab = kFoldSin;
  shape_.sinc_tab = kFoldSinc;
  first_ = true;
}

void Fold::Control(int32_t n, const FoldParams& p) {
  const int32_t target[5] = { p.fold, p.symmetry, p.tone, p.mix, p.gain };
  for (int32_t i = 0; i < 5; ++i) {
    int32_t t = target[i];
    if (t < 0) t = 0;
    if (t > 32767) t = 32767;
    if (first_) {
      knob_[i] = t;
    } else {
      const int32_t d = t - knob_[i];
      if (d <= kSmoothSnap && d >= -kSmoothSnap) {
        knob_[i] = t;
      } else {
        knob_[i] += d >> kSmoothShift;
      }
    }
  }
  const int32_t fold = knob_[0], sym = knob_[1], tone = knob_[2], mix = knob_[3], gain = knob_[4];
  if (!first_ && fold == last_[0] && sym == last_[1] && tone == last_[2] && mix == last_[3] &&
      gain == last_[4]) {
    // Settled: the ramps stand still at the last targets.
    shape_.g = g_;
    shape_.g_step = 0;
    shape_.c = c_;
    shape_.c_step = 0;
    tone_.sc = sc_;
    tone_.sc_step = 0;
    tone_.mix = static_cast<int32_t>((static_cast<uint32_t>(lm_) << 16) | static_cast<uint32_t>(mm_));
    tone_.mix_step = 0;
    return;
  }
  for (int32_t i = 0; i < 5; ++i) last_[i] = knob_[i];

  // The input gain (Q14) scales the folder's gain, within the knob's own range.
  int32_t gk = (Curve(kFoldGain, fold) * gain) >> 14;
  const int32_t gmin = Curve(kFoldGain, 0), gmax = Curve(kFoldGain, 32767);
  if (gk < gmin) gk = gmin;
  if (gk > gmax) gk = gmax;
  const int32_t g = gk << 16;
  const int32_t c = (sym - 16384) << 10;   // +-1/8 turn, Q27
  // What the folder makes of silence at this offset: u = u' = c, so
  // sin(2 pi c) x sinc(0) (16383).
  const int32_t sc = FoldMulFrac(FoldSinQ15(static_cast<uint32_t>(c) << 5), 16383) << 16;
  const int32_t lm = (Curve(kFoldLevel, fold) * mix) >> 15;
  const int32_t mm = 32767 - mix;
  if (first_) {
    g_ = g;
    c_ = c;
    sc_ = sc;
    lm_ = lm;
    mm_ = mm;
    first_ = false;
  }

  shape_.g = g_;
  shape_.g_step = Step(g_, g, n);
  shape_.c = c_;
  shape_.c_step = Step(c_, c, n);
  tone_.sc = sc_;
  tone_.sc_step = Step(sc_, sc, n);
  tone_.mix = static_cast<int32_t>((static_cast<uint32_t>(lm_) << 16) | static_cast<uint32_t>(mm_));
  // Both halves step towards zero and the low half stays in 0..32767, so one
  // 32-bit add per sample ramps both.
  tone_.mix_step = static_cast<int32_t>((static_cast<uint32_t>(Step(lm_, lm, n)) << 16) +
                                        static_cast<uint32_t>(Step(mm_, mm, n)));
  tone_.coef = (Curve(kFoldTone, tone) & 0xffff) | (kDcCoef << 16);
  // The next call starts exactly at the targets.
  g_ = g;
  c_ = c;
  sc_ = sc;
  lm_ = lm;
  mm_ = mm;
}

bool Fold::Process(int16_t* l, int16_t* r, int32_t n, const FoldParams& p) {
  if (n <= 0) return false;
  if (n > 64) n = 64;
#if !FOLD_C_KERNELS
  n &= ~1;   // the assembly takes even n (coop passes 64; the wrapper never passes odd)
  if (!n) return false;
#endif
  Control(n, p);
  // Mix settled at 0: the input passes untouched (bit-exact); the kernels
  // still run, into a sink, so the folder and filters stay warm.
  const bool bypass = tone_.mix == 32767 && tone_.mix_step == 0;
  int16_t* out_l = bypass ? sink_ : l;
  int16_t* out_r = bypass ? sink_ + 64 : r;
#if !FOLD_C_KERNELS
  if (r) {
    FoldShapeRunAsm(&shape_, &ch_[0], l, w_, 2, n);
    FoldShapeRunAsm(&shape_, &ch_[1], r, w_ + 1, 2, n);
    FoldToneStereoRunAsm(&tone_, ch_, w_, out_l, out_r, n);
  } else {
    FoldShapeRunAsm(&shape_, &ch_[0], l, w_, 1, n);
    FoldToneRunAsm(&tone_, &ch_[0], w_, out_l, n);
  }
#else
  if (r) {
    FoldShapeRunC(&shape_, &ch_[0], l, w_, 2, n);
    FoldShapeRunC(&shape_, &ch_[1], r, w_ + 1, 2, n);
    FoldToneStereoRunC(&tone_, ch_, w_, out_l, out_r, n);
  } else {
    FoldShapeRunC(&shape_, &ch_[0], l, w_, 1, n);
    FoldToneRunC(&tone_, &ch_[0], w_, out_l, n);
  }
#endif
  return !bypass;
}

}  // namespace op1fx
