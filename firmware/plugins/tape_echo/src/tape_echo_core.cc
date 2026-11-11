// TAPE ECHO: a multi-head tape delay in fixed point. See tape_echo.h. (Named _core so its object does not clash with
// the plugin glue, ../tape_echo.cc.)
//
// From te-op1-cfw ports/fx/src/tape_echo/tape_echo.cc (5aa314b). What changed for coop, nothing else:
// - the compile-time floating-point tables are integer literals (src/tape_echo_tables.h, generated from the original
//   by tests/host/tape_echo/gen_tables.cc): fixed point only, even at compile time (guidelines §2.1);
// - the mono kernels and paths, TIME's inverse (TapeEchoTimeWord, for kennel's tempo lock) and the test accessors
//   are gone; the C reference kernel is built only off the Blackfin (or with COOP_TAPE_ECHO_C);
// - Init leaves the tape alone; ClearTape zeroes it in slices.
// The stereo arithmetic is the original's, instruction for instruction (tests/host/tape_echo checks it bit for bit).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The wow and flutter model is
// Mutable Instruments Beads' (Emilie Gillet, MIT, ../LICENSE-MIT).

#include "src/tape_echo.h"

#include "src/tape_echo_tables.h"

namespace op1fx {

namespace {

// x 0..32767; every table's steps x 1023 fit 32 bits.
inline int32_t Lookup(const int32_t* t, int32_t x) {
  const int32_t i = x >> 10;
  const int32_t f = x & 1023;
  return t[i] + (((t[i + 1] - t[i]) * f) >> 10);
}

// 0.5 + 0.5 sin(2 pi phase), Q15 (0..32767): Beads' SineOscillator.
inline int32_t UnipolarSine(uint32_t phase) {
  const int16_t* sine = tables::kSine;
  const int32_t i = static_cast<int32_t>(phase >> 24);
  const int32_t f = static_cast<int32_t>((phase >> 8) & 0xffff);
  const int32_t s = sine[i] + (((sine[i + 1] - sine[i]) * f) >> 16);
  return (s + 32767) >> 1;
}

// x / 3 for 0 <= x < 49152 (a multiply: the Blackfin has no divider).
constexpr int32_t Third(int32_t x) { return (x * 43691) >> 17; }
constexpr bool ThirdExact() {
  for (int32_t x = 0; x < 49152; ++x) {
    if (Third(x) != x / 3) return false;
  }
  return true;
}
static_assert(ThirdExact(), "x / 3");
inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// 65536 / n for even n (n = 2 j).
constexpr int32_t kRecip[kTapeEchoMaxBlock / 2 + 1] = {
  0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341,
  2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130,
  1092, 1057, 1024,
};
// log2(n) for even n = 2 j, -1 if n isn't a power of two.
constexpr int8_t kShift[kTapeEchoMaxBlock / 2 + 1] = {
  -1, 1, 2, -1, 3, -1, -1, -1, 4, -1, -1, -1, -1, -1, -1, -1, 5,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6,
};
// delta / n per sample (floored; the caller tracks start + step n).
inline int32_t Ramp(int32_t delta, int32_t n) {
  const int32_t shift = kShift[n >> 1];
  if (shift >= 0) return delta >> shift;
  return static_cast<int32_t>((static_cast<int64_t>(delta) * kRecip[n >> 1]) >> 16);
}

// Smoothing per call (Q15 << 8): an eighth of the way (~12 ms), snapped when close.
inline int32_t Smooth(int32_t s, int32_t target_q15) {
  const int32_t t = target_q15 << 8;
  const int32_t d = t - s;
  if (d < 2048 && d > -2048) return t;
  return s + (d >> 3);
}

inline int32_t Lo(int32_t x) { return static_cast<int16_t>(x & 0xffff); }
inline int32_t Hi(int32_t x) { return static_cast<int16_t>(static_cast<uint32_t>(x) >> 16); }
inline int32_t Pack(int32_t lo, int32_t hi) {
  return (lo & 0xffff) | static_cast<int32_t>(static_cast<uint32_t>(hi) << 16);
}

// Normalised head weights by the number of active heads: out (1, 0.6, 0.45) and feedback (summing to 1).
const int32_t kOutNorm[4] = { 0, 32767, 19661, 14746 };
const int32_t kFbNorm[4] = { 0, 32767, 16384, 10923 };
const int32_t kHeadRamp = 4096;                 // per call: a pattern change takes 8 calls
const int32_t kFeedbackTop = 18022;             // 1.1 / 2, Q15 (the record sum is at half scale)
const int32_t kFeedbackHold = 16384;            // 1 / 2
const int32_t kGainRamp = 0x08000000;           // input gain per call: a gate in 16 calls
const int32_t kLowCutHold = 93;                 // 10 Hz

// Time glide.
const int32_t kVelocityMax = 24576;             // 0.75 sample per sample
const int32_t kAccel = 32;                      // per sample, per sample (Q15): 0 -> max in 24 ms
const int32_t kGlideShift = 11;                 // 2048 samples (93 ms)
const int32_t kTimeMin = 663 << 15;             // multiples of 3 samples
const int32_t kTimeMax = 33075 << 15;

// Wow and flutter (Beads, at 22050 Hz): depths in Q15 samples, rates per sample (2^32 = one cycle).
const int32_t kWowDepth2 = 193;                 // 2 x 96.5 samples (Beads' 105 at 24 kHz)
const int32_t kFlutterDepth2 = 31;              // 2 x 15.5 (17)
const uint32_t kWowInc = 44800;                 // 0.23 Hz
const uint32_t kFlutterInc = 387618;            // 1.99 Hz
const uint32_t kRandInc0 = 24348;               // 0.125 Hz
const uint32_t kRandIncMin = 9739;              // 0.05 Hz
const uint32_t kRandIncRange = 38957;           // + 0.2 Hz x random

static_assert(TE_TAPE_SIZE > 33075 + 96 + 16 + 2 + 1, "tape length");
static_assert(663 / 3 > kTapeEchoMaxBlock + 2, "head 1's shortest delay");
static_assert(sizeof(TapeEchoKernel) == TEK_SIZE, "kernel block");

// A per-sample ramp from *v towards target over n samples: returns the step and sets *start (the kernel's first
// value); *v becomes the value after n samples. Within n of the target it lands.
inline int32_t RampTo(int32_t* v, int32_t target, int32_t n, int32_t* start) {
  const int32_t d = target - *v;
  if (d < n && d > -n) {
    *v = *start = target;
    return 0;
  }
  *start = *v;
  const int32_t s = Ramp(d, n);
  *v += s * n;
  return s;
}

// Head h's read index (unwrapped) at sample i of a run from p.
inline int32_t ReadIndex(const TapeEchoKernel* k, int32_t h, int32_t p, int32_t i) {
  return p - 1 - i + ((k->pos[h] + i * k->step[h]) >> 15);
}

}  // namespace

const uint8_t kTapeEchoPatternHeads[kTapeEchoNumPatterns] = { 4, 6, 7, 5 };
// The table the assembly reads by its (mangled) name.
const int16_t kTapeEchoSatTable[257] = {
#define S(i) tables::kSat[i]
#define S8(i) S(i), S(i + 1), S(i + 2), S(i + 3), S(i + 4), S(i + 5), S(i + 6), S(i + 7)
#define S64(i) S8(i), S8(i + 8), S8(i + 16), S8(i + 24), S8(i + 32), S8(i + 40), S8(i + 48), S8(i + 56)
  S64(0), S64(64), S64(128), S64(192), S(256),
#undef S64
#undef S8
#undef S
};

int32_t TapeEchoSaturate(int32_t x) {
  const int32_t i = (x >> 8) + 128;
  const int32_t f = x & 255;
  const int32_t a = kTapeEchoSatTable[i];
  return a + (((kTapeEchoSatTable[i + 1] - a) * f) >> 8);
}

// --- The run kernel's C reference (tape_echo_bfin.S matches it); host builds only ---

#if !defined(__bfin__) || defined(COOP_TAPE_ECHO_C)
namespace {

inline int64_t Fr(int32_t a, int32_t b) {
  if (a == -32768 && b == -32768) return 0x7fffffff;
  return static_cast<int64_t>(a * b) * 2;
}
inline int32_t Sat16(int64_t x) {
  return static_cast<int32_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}
inline int32_t Sat32(int64_t x) {
  return static_cast<int32_t>(x > INT32_MAX ? INT32_MAX : (x < INT32_MIN ? INT32_MIN : x));
}
// The accumulator's high half, rounded (unbiased: ties to even), saturated.
inline int32_t Rnd(int64_t acc) {
  int64_t q = acc >> 16;
  const int32_t r = static_cast<int32_t>(acc & 0xffff);
  if (r > 0x8000 || (r == 0x8000 && (q & 1))) ++q;
  return Sat16(q);
}

}  // namespace

void TapeEchoRunC(TapeEchoKernel* k, int32_t* x, int32_t* tape, int32_t p, int32_t n) {
  int32_t pos[3], step[3], w[3], ws[3];
  for (int32_t h = 0; h < 3; ++h) {
    pos[h] = k->pos[h];
    step[h] = k->step[h];
    w[h] = k->w[h];
    ws[h] = k->ws[h];
  }
  int32_t gain = k->gain;
  const int32_t gstep = k->gstep;
  int32_t hp_l = k->hp_l, hp_r = k->hp_r;
  int32_t lp_l = Lo(k->lp), lp_r = Hi(k->lp);
  const int32_t khp = Lo(k->k), kloss = Hi(k->k);

  for (int32_t i = 0; i < n; ++i) {
    --p;
    int32_t hl[3], hr[3];
    for (int32_t h = 0; h < 3; ++h) {
      const int32_t d = pos[h];
      pos[h] += step[h];
      const int32_t ip = d >> 15, f = d & 0x7fff;
      int32_t idx = p + ip;
      if (idx >= TE_TAPE_SIZE) idx -= TE_TAPE_SIZE;
      const int32_t a = tape[idx], b = tape[idx + 1];
      hl[h] = Rnd(Fr(Lo(a), 32767 - f) + Fr(Lo(b), f));
      hr[h] = Rnd(Fr(Hi(a), 32767 - f) + Fr(Hi(b), f));
    }
    int64_t al = 0, ar = 0, fl = 0, fr = 0;
    for (int32_t h = 0; h < 3; ++h) {
      al += Fr(hl[h], Lo(w[h]));
      ar += Fr(hr[h], Lo(w[h]));
      fl += Fr(hl[h], Hi(w[h]));
      fr += Fr(hr[h], Hi(w[h]));
      w[h] = Pack(Lo(w[h]) + Lo(ws[h]), Hi(w[h]) + Hi(ws[h]));
    }
    const int32_t in = x[i];
    fl += Fr(Lo(in), Hi(gain));
    fr += Fr(Hi(in), Hi(gain));
    gain += gstep;
    x[i] = Pack(Rnd(al), Rnd(ar));
    const int32_t sl = Rnd(fl), sr = Rnd(fr);

    const int32_t yl = Sat16(sl - (hp_l >> 16));
    hp_l = Sat32(static_cast<int64_t>(hp_l) + Fr(yl, khp));
    lp_l = Sat16(lp_l + Rnd(Fr(Sat16(yl - lp_l), kloss)));
    const int32_t yr = Sat16(sr - (hp_r >> 16));
    hp_r = Sat32(static_cast<int64_t>(hp_r) + Fr(yr, khp));
    lp_r = Sat16(lp_r + Rnd(Fr(Sat16(yr - lp_r), kloss)));
    tape[p] = Pack(TapeEchoSaturate(lp_l), TapeEchoSaturate(lp_r));
  }
  for (int32_t h = 0; h < 3; ++h) {
    k->pos[h] = pos[h];
    k->w[h] = w[h];
  }
  k->gain = gain;
  k->hp_l = hp_l;
  k->hp_r = hp_r;
  k->lp = Pack(lp_l, lp_r);
}
#endif

// --- Runs ---

int32_t TapeEchoPrepareRun(TapeEchoKernel* k, int32_t p, int32_t m) {
  for (int32_t h = 0; h < 3; ++h) {
    if (ReadIndex(k, h, p, 0) < TE_TAPE_SIZE) {
      k->adj[h] = 0;   // and it stays below: the index never rises
      continue;
    }
    k->adj[h] = -4 * TE_TAPE_SIZE;
    if (ReadIndex(k, h, p, m - 1) >= TE_TAPE_SIZE) continue;
    // The first sample below the wrap ends the run: in (lo, hi].
    int32_t lo = 0, hi = m - 1;
    while (hi - lo > 1) {
      const int32_t mid = (lo + hi) >> 1;
      if (ReadIndex(k, h, p, mid) >= TE_TAPE_SIZE) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    m = hi;
  }
  return m;
}

// --- The echo ---

void TapeEcho::Init() {
  for (int32_t i = 0; i < kTapeEchoMaxBlock; ++i) x_[i] = 0;
  // (The original's `k_ = TapeEchoKernel()`, as a loop: GCC made that a memset call, which a plugin can't import.)
  int32_t* kw = reinterpret_cast<int32_t*>(&k_);
  for (int32_t i = 0; i < TEK_SIZE / 4; ++i) kw[i] = 0;
  t_ = t_target_ = kTimeMin;
  v_ = 0;
  wow_phase_ = flutter_phase_ = rand_phase_ = 0;
  rand_inc_ = kRandInc0;
  rng_ = 1;
  mod_ = 0;
  fb_ = age_ = 0;
  for (int32_t h = 0; h < 3; ++h) cur_out_[h] = cur_fb_[h] = w_out_[h] = w_fb_[h] = 0;
  gain_ = 0;
  ptr_ = TE_TAPE_SIZE;
  started_ = false;
  settled_ = false;
  last_ = TapeEchoParams();
  last_time_ = last_age_ = -1;
  last_hold_ = false;
  inv_drive_ = gain_target_ = coefs_ = 0;
}

int32_t TapeEcho::ClearTape(int32_t from, int32_t count) {
  int32_t end = from + count;
  if (end > TE_TAPE_PAIRS) end = TE_TAPE_PAIRS;
  for (int32_t i = from; i < end; ++i) tape_[i] = 0;
  return end;
}

void TapeEcho::Control(const TapeEchoParams& p, int32_t n) {
  // Still knobs and everything settled: only the wow and flutter move.
  const bool fast = settled_ && p.time == last_.time && p.feedback == last_.feedback &&
                    p.age == last_.age && p.heads == last_.heads && p.hold == last_.hold;
  last_ = p;
  const int32_t age = age_ >> 8;
  bool first = false;
  int32_t fb = fb_ >> 8;
  if (!fast) {
    const int32_t time = Clamp(p.time, 0, 32767);
    const int32_t age_knob = Clamp(p.age, 0, 32767);
    const int32_t fb_knob = Clamp(p.feedback, 0, 32767);
    const int32_t pattern = Clamp(p.heads, 0, kTapeEchoNumPatterns - 1);

    // Head 3's target delay, in whole multiples of 3 samples.
    if (time != last_time_) {
      last_time_ = time;
      const int32_t samples = (Lookup(tables::kTime, time) + 8) >> 4;
      t_target_ = Clamp(Third(samples + 1) * 3 << 15, kTimeMin, kTimeMax);
    }

    const int32_t fb_target = p.hold ? kFeedbackHold : (fb_knob * kFeedbackTop) >> 15;
    const int32_t heads = kTapeEchoPatternHeads[pattern];
    const int32_t count = (heads & 1) + ((heads >> 1) & 1) + ((heads >> 2) & 1);
    int32_t to[3], tf[3];
    for (int32_t h = 0; h < 3; ++h) {
      to[h] = (heads >> h) & 1 ? kOutNorm[count] : 0;
      tf[h] = (heads >> h) & 1 ? kFbNorm[count] : 0;
    }

    first = !started_;
    if (first) {
      started_ = true;
      t_ = t_target_;
      v_ = 0;
      fb_ = fb_target << 8;
      age_ = age_knob << 8;
      for (int32_t h = 0; h < 3; ++h) {
        cur_out_[h] = to[h];
        cur_fb_[h] = tf[h];
      }
    } else {
      fb_ = Smooth(fb_, fb_target);
      age_ = Smooth(age_, age_knob);

      // The glide: a velocity that follows the error (93 ms), limited in speed and acceleration.
      const int32_t e = t_target_ - t_;
      int32_t vt = e >> kGlideShift;
      if (vt == 0 && e != 0) vt = e > 0 ? 1 : -1;
      vt = Clamp(vt, -kVelocityMax, kVelocityMax);
      const int32_t a = kAccel * n;
      v_ += Clamp(vt - v_, -a, a);
      int32_t t = t_ + v_ * n;
      if ((e >= 0 && v_ >= 0 && t >= t_target_) || (e <= 0 && v_ <= 0 && t <= t_target_)) {
        t = t_target_;
        v_ = 0;
      }
      if (t < kTimeMin || t > kTimeMax) {
        t = Clamp(t, kTimeMin, kTimeMax);
        v_ = 0;
      }
      t_ = t;

      for (int32_t h = 0; h < 3; ++h) {
        cur_out_[h] += Clamp(to[h] - cur_out_[h], -kHeadRamp, kHeadRamp);
        cur_fb_[h] += Clamp(tf[h] - cur_fb_[h], -kHeadRamp, kHeadRamp);
      }
    }
    fb = fb_ >> 8;
    const int32_t age_now = age_ >> 8;

    if (age_now != last_age_ || p.hold != last_hold_) {
      last_age_ = age_now;
      last_hold_ = p.hold;
      inv_drive_ = Lookup(tables::kInvDrive, age_now);
      gain_target_ = p.hold ? 0 : (16384 + (age_now >> 1)) << 16;   // drive / 2, Q31
      // Hold keeps the loop (slowly) degrading: a quarter of the tape loss and a 10 Hz low cut.
      if (p.hold) {
        coefs_ = Pack(kLowCutHold, Lookup(tables::kLoss, age_now >> 2));
      } else {
        coefs_ = Pack(Lookup(tables::kLowCut, age_now), Lookup(tables::kLoss, age_now));
      }
    }

    bool settled = t_ == t_target_ && v_ == 0 && fb_ == fb_target << 8 && age_ == age_knob << 8;
    if (first) {
      for (int32_t h = 0; h < 3; ++h) {
        w_out_[h] = (cur_out_[h] * inv_drive_) >> 15;
        w_fb_[h] = (cur_fb_[h] * fb) >> 15;
      }
      gain_ = gain_target_;
    }
    for (int32_t h = 0; h < 3; ++h) {
      int32_t so0, sf0;
      const int32_t so = RampTo(&w_out_[h], (cur_out_[h] * inv_drive_) >> 15, n, &so0);
      const int32_t sf = RampTo(&w_fb_[h], (cur_fb_[h] * fb) >> 15, n, &sf0);
      k_.w[h] = Pack(so0, sf0);
      k_.ws[h] = Pack(so, sf);
      settled = settled && so == 0 && sf == 0 && cur_out_[h] == to[h] && cur_fb_[h] == tf[h];
    }
    const int32_t gain_end = gain_ + Clamp(gain_target_ - gain_, -kGainRamp, kGainRamp);
    k_.gstep = RampTo(&gain_, gain_end, n, &k_.gain);
    settled = settled && k_.gstep == 0 && gain_ == gain_target_;
    k_.k = coefs_;
    settled_ = settled;
  } else {
    for (int32_t h = 0; h < 3; ++h) k_.ws[h] = 0;
    k_.gstep = 0;
  }
  const int32_t wow_age = fast ? age : age_ >> 8;

  // Wow and flutter (Beads' WowAndFlutter::Process, once per call), in 32 bits.
  const uint32_t previous = rand_phase_;
  rand_phase_ += rand_inc_ * static_cast<uint32_t>(n);
  if (previous < 0x40000000u && rand_phase_ >= 0x40000000u) {
    rng_ = rng_ * 1664525u + 1013904223u;
    rand_inc_ = kRandIncMin + static_cast<uint32_t>(((rng_ >> 16) * kRandIncRange) >> 16);
  }
  const int32_t rm = UnipolarSine(rand_phase_);                              // Q15
  const int32_t fm = 16384 + ((3 * ((rm * rm) >> 15)) >> 1);                 // 0.5 + 1.5 rm^2
  const int32_t amount = ((32768 - ((rm * 13107) >> 15)) * wow_age) >> 15;   // (1 - 0.4 rm) age
  wow_phase_ += (((kWowInc * static_cast<uint32_t>(n)) >> 7) * static_cast<uint32_t>(fm)) >> 8;
  flutter_phase_ += kFlutterInc * static_cast<uint32_t>(n);
  const int32_t depth = (UnipolarSine(wow_phase_) * kWowDepth2 +
                         UnipolarSine(flutter_phase_) * kFlutterDepth2) >> 1;
  mod_ = ((depth >> 7) * amount) >> 8;

  // The heads' delays at the call's end; the read offset scales every head, as a tape speed change does.
  const int32_t d3 = t_ + mod_;
  const int32_t q = Third(d3 >> 15);
  const int32_t rest = d3 - q * 3 * 32768;
  const int32_t third = (q << 15) + ((rest * 10923) >> 15);
  const int32_t end[3] = { third, 2 * third, d3 };
  for (int32_t h = 0; h < 3; ++h) {
    if (first) k_.pos[h] = end[h];
    k_.step[h] = Ramp(end[h] - k_.pos[h], n);
  }
}

void TapeEcho::Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l, int16_t* wet_r,
                       int32_t n, const TapeEchoParams& p) {
  Control(p, n);
  for (int32_t i = 0; i < n; ++i) x_[i] = Pack(in_l[i], in_r[i]);

  // Runs end where the write position wraps or a head's read does; then the mirror pair.
  int32_t done = 0;
  while (done < n) {
    int32_t m = ptr_;
    if (m > n - done) m = n - done;
    m = TapeEchoPrepareRun(&k_, ptr_, m);
#if defined(__bfin__) && !defined(COOP_TAPE_ECHO_C)
    TapeEchoRunAsm(&k_, x_ + done, tape_, tape_ + ptr_, m);
#else
    TapeEchoRunC(&k_, x_ + done, tape_, ptr_, m);
#endif
    ptr_ -= m;
    if (ptr_ == 0) ptr_ = TE_TAPE_SIZE;
    done += m;
  }
  tape_[TE_TAPE_SIZE] = tape_[0];

  for (int32_t i = 0; i < n; ++i) {
    wet_l[i] = static_cast<int16_t>(x_[i]);
    wet_r[i] = static_cast<int16_t>(x_[i] >> 16);
  }
}

}  // namespace op1fx
