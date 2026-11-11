// PLATE: Dattorro's plate in fixed point. See plate.h; te-op1-cfw
// ports/fx/src/plate/plate.cc (9ced415) with its constexpr double maths
// replaced by the integer tables it produced (tests/host/cs_plate checks
// every entry against the upstream build), the C kernels host only.

#include "plate.h"

namespace csplate {

namespace {

// --- Tank lengths per size step ---
// Dattorro's lengths at 29761 Hz, scaled to 22050 Hz and by
// 0.6^((7 - s) / 7) for size step s, rounded (upstream Scaled(length, s)).
// Order: a1 (672), a3 (908); tails D4 (3163), D1 (4453), A2 (1800), D2
// (3720), D3 (4217), A4 (2656); left taps 266 2974 1913 1996 1990 187 1066;
// right taps 353 3627 1228 2673 2111 335 121.
constexpr PlateSizeTable kPlateSizesC[kPlateNumSizes] = {
  {299, 404, {1406, 1980, 800, 1654, 1875, 1181},
   {118, 1322, 850, 887, 885, 83, 474}, {157, 1612, 546, 1188, 938, 149, 54}},
  {321, 434, {1513, 2129, 861, 1779, 2017, 1270},
   {127, 1422, 915, 954, 952, 89, 510}, {169, 1734, 587, 1278, 1009, 160, 58}},
  {346, 467, {1627, 2291, 926, 1914, 2169, 1366},
   {137, 1530, 984, 1027, 1024, 96, 548}, {182, 1866, 632, 1375, 1086, 172, 62}},
  {372, 502, {1750, 2464, 996, 2058, 2333, 1470},
   {147, 1646, 1059, 1104, 1101, 103, 590}, {195, 2007, 679, 1479, 1168, 185, 67}},
  {400, 540, {1883, 2651, 1071, 2214, 2510, 1581},
   {158, 1770, 1139, 1188, 1185, 111, 635}, {210, 2159, 731, 1591, 1257, 199, 72}},
  {430, 581, {2025, 2851, 1153, 2382, 2700, 1701},
   {170, 1904, 1225, 1278, 1274, 120, 683}, {226, 2322, 786, 1711, 1352, 214, 77}},
  {463, 625, {2179, 3067, 1240, 2562, 2905, 1829},
   {183, 2048, 1318, 1375, 1371, 129, 734}, {243, 2498, 846, 1841, 1454, 231, 83}},
  {498, 673, {2343, 3299, 1334, 2756, 3124, 1968},
   {197, 2203, 1417, 1479, 1474, 139, 790}, {262, 2687, 910, 1980, 1564, 248, 90}},
};

// The layout (plate_layout.h) is the largest size's lengths.
constexpr const PlateSizeTable& kLargest = kPlateSizesC[kPlateNumSizes - 1];
static_assert(kLargest.a1 == PL_A1_LEN && kLargest.a3 == PL_A3_LEN && kLargest.tail[0] == PL_D4_LEN &&
              kLargest.tail[1] == PL_D1_LEN && kLargest.tail[2] == PL_A2_LEN &&
              kLargest.tail[3] == PL_D2_LEN && kLargest.tail[4] == PL_D3_LEN &&
              kLargest.tail[5] == PL_A4_LEN, "tank lengths");
static_assert(PL_I2 == PL_I1 + PL_I1_LEN + 1 && PL_I4 == PL_I2 + PL_I2_LEN + 1 &&
              PL_I3 == PL_I4 + PL_I4_LEN + 1, "input bases");
static_assert(PL_IN_MAX_LINE >= PL_I1_LEN && PL_IN_MAX_LINE >= PL_I2_LEN &&
              PL_IN_MAX_LINE >= PL_I4_LEN, "input: the longest line is last");
static_assert(PL_A2 == PL_A1 + PL_A1_REACH + 1 && PL_D2 == PL_A2 + PL_A2_LEN + 1 &&
              PL_A3 == PL_D2 + PL_D2_LEN + 1 && PL_D3 == PL_A3 + PL_A3_REACH + 1 &&
              PL_A4 == PL_D3 + PL_D3_LEN + 1 && PL_D4 == PL_A4 + PL_A4_LEN + 1 &&
              PL_D1 == PL_D4 + PL_D4_LEN + 1, "tank bases");
static_assert(PL_TANK_MAX_LINE >= PL_A1_REACH && PL_TANK_MAX_LINE >= PL_A2_LEN &&
              PL_TANK_MAX_LINE >= PL_D2_LEN && PL_TANK_MAX_LINE >= PL_A3_REACH &&
              PL_TANK_MAX_LINE >= PL_D3_LEN && PL_TANK_MAX_LINE >= PL_A4_LEN &&
              PL_TANK_MAX_LINE >= PL_D4_LEN, "tank: the longest line is last");
static_assert(PL_TANK_WORDS <= 32768, "tank buffer");
// Immediate offsets in the assembly (W[P + uimm16m2]).
static_assert(2 * PL_TANK_REACH <= 65534, "tank offsets");
// The smallest modulated delay stays positive.
static_assert(kPlateSizesC[0].a1 - PL_EXC_MAX > kPlateMaxBlock, "modulation depth");
// Excursion: at most (32767 * 1517) >> 10 in Q11 (23.7 samples: Dattorro's
// 16 at 29761 Hz, x 2), inside the reserve.
static_assert(((32767 * 1517) >> 10) < (PL_EXC_MAX << 11), "excursion reserve");

}  // namespace

const PlateSizeTable kPlateSizes[kPlateNumSizes] = {
  kPlateSizesC[0], kPlateSizesC[1], kPlateSizesC[2], kPlateSizesC[3],
  kPlateSizesC[4], kPlateSizesC[5], kPlateSizesC[6], kPlateSizesC[7],
};
const int8_t kPlateTapSign[7] = { 1, 1, -1, 1, -1, -1, -1 };

namespace {

// Lines the taps of each side read (the bases), in Dattorro's order:
// left: D3 D3 A4 D4 D1 A2 D2; right: D1 D1 A2 D2 D3 A4 D4.
const int32_t kTapBaseL[7] = { PL_D3, PL_D3, PL_A4, PL_D4, PL_D1, PL_A2, PL_D2 };
const int32_t kTapBaseR[7] = { PL_D1, PL_D1, PL_A2, PL_D2, PL_D3, PL_A4, PL_D4 };
const int32_t kTailBase[6] = { PL_D4, PL_D1, PL_A2, PL_D2, PL_D3, PL_A4 };

// --- Parameter tables (Q15, 33 points over 0..32767) ---

struct Table33 {
  int16_t v[33];
};

// Damping low-pass coefficient k = 1 - exp(-2 pi fc / fs) x 32767, rounded:
// fc from 26.7 kHz (k = 0.9995: Dattorro's damping 0.0005) down to 600 Hz,
// log (fc = 600 x 44.5^(1 - i / 32)).
const Table33 kDampTable = {{
  32751, 32729, 32686, 32608, 32479, 32278, 31984, 31578, 31045, 30373, 29559,
  28606, 27526, 26334, 25049, 23694, 22293, 20868, 19440, 18030, 16652, 15321,
  14047, 12837, 11697, 10631, 9638, 8719, 7872, 7095, 6385, 5738, 5149,
}};

// Low cut: the high-pass's one-pole coefficient (1 - exp(-2 pi fc / fs)) x
// 32768, rounded, fc = 20 x 25^(i / 32) Hz.
const Table33 kLowCutTable = {{
  186, 206, 228, 252, 278, 307, 340, 375, 415, 459, 507,
  560, 619, 683, 755, 834, 921, 1016, 1122, 1239, 1367, 1508,
  1664, 1835, 2023, 2229, 2456, 2705, 2978, 3277, 3604, 3961, 4351,
}};

inline int32_t Lookup(const Table33& t, int32_t x) {
  const int32_t i = x >> 10;
  const int32_t f = x & 1023;
  return t.v[i] + (((t.v[i + 1] - t.v[i]) * f) >> 10);
}

inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// 65536 / n for even n (n = 2 j).
constexpr int32_t kRecip[kPlateMaxBlock / 2 + 1] = {
  0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341,
  2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130,
  1092, 1057, 1024,
};

// log2(n) for even n = 2 j, -1 if n isn't a power of two.
constexpr int8_t kShift[kPlateMaxBlock / 2 + 1] = {
  -1, 1, 2, -1, 3, -1, -1, -1, 4, -1, -1, -1, -1, -1, -1, -1, 5,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6,
};

// delta / n, per sample (floored; the caller tracks start + step n). A
// shift for the usual n (32): no 64-bit product.
inline int32_t Ramp(int32_t delta, int32_t n) {
  const int32_t shift = kShift[n >> 1];
  if (shift >= 0) return delta >> shift;
  return static_cast<int32_t>((static_cast<int64_t>(delta) * kRecip[n >> 1]) >> 16);
}

// Parabolic sine of a phase: 4 x (1 - |x|), Q15.
inline int32_t Sine(uint32_t phase) {
  const int32_t x = static_cast<int32_t>(phase) >> 16;    // Q15, [-1, 1)
  const int32_t a = x < 0 ? -x : x;
  const int32_t y = (x * (32768 - a)) >> 13;
  return Clamp(y, -32767, 32767);
}

// Smoothing per call (Q15 << 8): an eighth of the way each call (~12 ms),
// snapped when close.
inline int32_t Smooth(int32_t s, int32_t target_q15) {
  const int32_t t = target_q15 << 8;
  const int32_t d = t - s;
  if (d < 2048 && d > -2048) return t;
  return s + (d >> 3);
}

const uint32_t kLfoIncrement = 194783;            // 1 Hz at 22050 Hz (2^32 / 22050)
const int32_t kInputGain = 0x20000000;            // 0.25, Q31: the tank's headroom
const int32_t kOutGain = 19661;                   // 0.6 (x 2 at the output: the makeup)
const int32_t kDd1 = 22938;                       // decay diffusion 1: 0.70
const int32_t kPreSlew = 16384;                   // pre-delay glide: <= 1/4 sample per sample

inline int32_t Pack(int32_t lo, int32_t hi) {
  return (lo & 0xffff) | static_cast<int32_t>(static_cast<uint32_t>(hi) << 16);
}

#if !defined(__bfin__)
// --- The Blackfin's 16-bit MAC arithmetic (the C references, host only) ---

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
#endif

}  // namespace

#if !defined(__bfin__)

// --- Run kernels (C references; plate_bfin.S matches them) ---

#define WRITE(index, value)                       \
  {                                               \
    p[index] = static_cast<int16_t>(value);       \
    p[(index) + alias] = static_cast<int16_t>(value); \
  }
// acc += prev rs; write the rounded acc; acc = prev + written ws.
#define ALLPASS(base, tail, rs, ws)                           \
  {                                                           \
    const int32_t previous = p[tail];                         \
    acc += Fr(previous, rs);                                  \
    const int32_t w = Rnd(acc);                               \
    WRITE(base, w)                                            \
    acc = (static_cast<int64_t>(previous) << 16) + Fr(w, ws); \
  }

void PlateInputRunC(PlateInputKernel* k, const int16_t* x, int16_t* out, int16_t* p, int32_t n,
                    int32_t alias_bytes) {
  const int32_t alias = alias_bytes / 2;
  const int32_t mg1 = Lo(k->g1), g1 = Hi(k->g1);
  const int32_t mg2 = Lo(k->g2), g2 = Hi(k->g2);
  const int32_t khp = Lo(k->khp);
  int32_t gain = k->gain;
  const int32_t gstep = k->gstep;
  int32_t hp = k->hp;
  for (int32_t i = 0; i < n; ++i) {
    --p;
    // Low cut: y = x - lp; lp += k y.
    const int32_t y = Sat16(static_cast<int32_t>(x[i]) - (hp >> 16));
    hp = Sat32(static_cast<int64_t>(hp) + Fr(y, khp));
    int64_t acc = Fr(y, gain >> 16);
    gain += gstep;
    ALLPASS(PL_I1, PL_I1 + PL_I1_LEN, mg1, g1)
    ALLPASS(PL_I2, PL_I2 + PL_I2_LEN, mg1, g1)
    ALLPASS(PL_I3, PL_I3 + PL_I3_LEN, mg2, g2)
    ALLPASS(PL_I4, PL_I4 + PL_I4_LEN, mg2, g2)
    out[i] = static_cast<int16_t>(Rnd(acc));
  }
  k->gain = gain;
  k->hp = hp;
}

void PlateTankRunC(PlateTankKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p,
                   int32_t n, int32_t alias_bytes) {
  const int32_t alias = alias_bytes / 2;
  const int32_t decay = Lo(k->decay_klp), klp = Hi(k->decay_klp);
  const int32_t dd1 = Lo(k->dd1), mdd1 = Hi(k->dd1);
  const int32_t mdd2 = Lo(k->dd2), dd2 = Hi(k->dd2);
  const int32_t gout = Lo(k->gout), mgout = Hi(k->gout);
  int32_t lp_1 = Lo(k->lp), lp_2 = Hi(k->lp);
  int32_t pos1 = k->pos1, pos2 = k->pos2;
  const int32_t step1 = k->step1, step2 = k->step2;
  int32_t o[PT_NUM_OFF];
  for (int32_t j = 0; j < PT_NUM_OFF; ++j) o[j] = k->off[j] / 2;
  int32_t gl[7];
  for (int32_t j = 0; j < 7; ++j) gl[j] = kPlateTapSign[j] > 0 ? gout : mgout;

  for (int32_t i = 0; i < n; ++i) {
    --p;
    const int64_t in = static_cast<int64_t>(x[i]) << 16;

    // Left: + decay D4, A1 (modulated, -dd1), D1, damping, x decay, A2, D2.
    int64_t acc = in + Fr(p[o[PT_D4T]], decay);
    {
      const int32_t ip = pos1 >> 16, f = (pos1 & 0xffff) >> 1;
      pos1 += step1;
      const int32_t m = Rnd(Fr(p[PL_A1 + ip], 32767 - f) + Fr(p[PL_A1 + ip + 1], f));
      acc += Fr(m, dd1);
      const int32_t w = Rnd(acc);
      WRITE(PL_A1, w)
      acc = (static_cast<int64_t>(m) << 16) + Fr(w, mdd1);
    }
    WRITE(PL_D1, Rnd(acc))
    lp_1 = Sat16(lp_1 + Rnd(Fr(Sat16(p[o[PT_D1T]] - lp_1), klp)));
    acc = Fr(lp_1, decay);
    ALLPASS(PL_A2, o[PT_A2T], mdd2, dd2)
    WRITE(PL_D2, Rnd(acc))

    // Right: + decay D2, A3, D3, damping, x decay, A4, D4.
    acc = in + Fr(p[o[PT_D2T]], decay);
    {
      const int32_t ip = pos2 >> 16, f = (pos2 & 0xffff) >> 1;
      pos2 += step2;
      const int32_t m = Rnd(Fr(p[PL_A3 + ip], 32767 - f) + Fr(p[PL_A3 + ip + 1], f));
      acc += Fr(m, dd1);
      const int32_t w = Rnd(acc);
      WRITE(PL_A3, w)
      acc = (static_cast<int64_t>(m) << 16) + Fr(w, mdd1);
    }
    WRITE(PL_D3, Rnd(acc))
    lp_2 = Sat16(lp_2 + Rnd(Fr(Sat16(p[o[PT_D3T]] - lp_2), klp)));
    acc = Fr(lp_2, decay);
    ALLPASS(PL_A4, o[PT_A4T], mdd2, dd2)
    WRITE(PL_D4, Rnd(acc))

    // Output taps, x 0.6, x 2.
    acc = 0;
    for (int32_t j = 0; j < 7; ++j) acc += Fr(p[o[PT_TL + j]], gl[j]);
    l[i] = static_cast<int16_t>(Rnd(acc * 2));
    if (r) {
      acc = 0;
      for (int32_t j = 0; j < 7; ++j) acc += Fr(p[o[PT_TR + j]], gl[j]);
      r[i] = static_cast<int16_t>(Rnd(acc * 2));
    }
  }
  k->lp = Pack(lp_1, lp_2);
  k->pos1 = pos1;
  k->pos2 = pos2;
}

// The tank with its size-dependent reads crossfaded: every tail and both
// modulated allpasses read at the old size (k) and the new one (xk), mixed
// by the new one's weight; no taps.
void PlateXfadeRunC(PlateTankKernel* k, PlateXfadeKernel* xk, const int16_t* x, int16_t* p,
                    int32_t n, int32_t alias_bytes) {
  const int32_t alias = alias_bytes / 2;
  const int32_t decay = Lo(k->decay_klp), klp = Hi(k->decay_klp);
  const int32_t dd1 = Lo(k->dd1), mdd1 = Hi(k->dd1);
  const int32_t mdd2 = Lo(k->dd2), dd2 = Hi(k->dd2);
  int32_t lp_1 = Lo(k->lp), lp_2 = Hi(k->lp);
  int32_t pos1 = k->pos1, pos2 = k->pos2, xpos1 = xk->pos1, xpos2 = xk->pos2;
  int32_t xf = xk->xf;
  int32_t o[6], xo[6];
  for (int32_t j = 0; j < 6; ++j) {
    o[j] = k->off[j] / 2;
    xo[j] = xk->off[j] / 2;
  }
#define TAP_AT(base, pos) \
  Rnd(Fr(p[(base) + ((pos) >> 16)], 32767 - (((pos) & 0xffff) >> 1)) + \
      Fr(p[(base) + ((pos) >> 16) + 1], ((pos) & 0xffff) >> 1))
  for (int32_t i = 0; i < n; ++i) {
    --p;
    const int32_t b = xf >> 16, a = 32767 - b;
    xf += xk->xf_step;
#define TAIL(j) Rnd(Fr(p[o[j]], a) + Fr(p[xo[j]], b))
    const int64_t in = static_cast<int64_t>(x[i]) << 16;

    int64_t acc = in + Fr(TAIL(PT_D4T), decay);
    {
      const int32_t m = Rnd(Fr(TAP_AT(PL_A1, pos1), a) + Fr(TAP_AT(PL_A1, xpos1), b));
      pos1 += k->step1;
      xpos1 += xk->step1;
      acc += Fr(m, dd1);
      const int32_t w = Rnd(acc);
      WRITE(PL_A1, w)
      acc = (static_cast<int64_t>(m) << 16) + Fr(w, mdd1);
    }
    WRITE(PL_D1, Rnd(acc))
    lp_1 = Sat16(lp_1 + Rnd(Fr(Sat16(TAIL(PT_D1T) - lp_1), klp)));
    acc = Fr(lp_1, decay);
    {
      const int32_t previous = TAIL(PT_A2T);
      acc += Fr(previous, mdd2);
      const int32_t w = Rnd(acc);
      WRITE(PL_A2, w)
      acc = (static_cast<int64_t>(previous) << 16) + Fr(w, dd2);
    }
    WRITE(PL_D2, Rnd(acc))

    acc = in + Fr(TAIL(PT_D2T), decay);
    {
      const int32_t m = Rnd(Fr(TAP_AT(PL_A3, pos2), a) + Fr(TAP_AT(PL_A3, xpos2), b));
      pos2 += k->step2;
      xpos2 += xk->step2;
      acc += Fr(m, dd1);
      const int32_t w = Rnd(acc);
      WRITE(PL_A3, w)
      acc = (static_cast<int64_t>(m) << 16) + Fr(w, mdd1);
    }
    WRITE(PL_D3, Rnd(acc))
    lp_2 = Sat16(lp_2 + Rnd(Fr(Sat16(TAIL(PT_D3T) - lp_2), klp)));
    acc = Fr(lp_2, decay);
    {
      const int32_t previous = TAIL(PT_A4T);
      acc += Fr(previous, mdd2);
      const int32_t w = Rnd(acc);
      WRITE(PL_A4, w)
      acc = (static_cast<int64_t>(previous) << 16) + Fr(w, dd2);
    }
    WRITE(PL_D4, Rnd(acc))
#undef TAIL
  }
#undef TAP_AT
  k->lp = Pack(lp_1, lp_2);
  k->pos1 = pos1;
  k->pos2 = pos2;
  xk->pos1 = xpos1;
  xk->pos2 = xpos2;
  xk->xf = xf;
}

#undef ALLPASS
#undef WRITE

// The two-tap interpolation of the tank kernels (Rnd(Fr + Fr)) in 32 bits:
// round(acc / 2^15), ties to even.
void PlatePreRunC(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x, int32_t w,
                  int32_t d, int32_t step, int32_t n) {
  for (int32_t i = 0; i < n; ++i, ++w) {
    ring[w] = static_cast<int16_t>((l[i] + r[i]) >> 1);
    const int32_t ip = d >> 16, f = (d & 0xffff) >> 1;
    d += step;
    int32_t r0 = w - ip;
    r0 += (r0 >> 31) & PL_PRE_WORDS;
    int32_t r1 = r0 - 1;
    r1 += (r1 >> 31) & PL_PRE_WORDS;
    const int32_t acc = ring[r0] * (32767 - f) + ring[r1] * f;
    x[i] = static_cast<int16_t>((acc + 0x3fff + ((acc >> 15) & 1)) >> 15);
  }
}
#endif  // !__bfin__

// The kernels: the assembly on the Blackfin, the C references elsewhere.
#if defined(__bfin__)
#define PLATE_PRE PlatePreRunAsm
#define PLATE_INPUT PlateInputRunAsm
#define PLATE_TANK PlateTankRunAsm
#define PLATE_XFADE PlateXfadeRunAsm
#else
#define PLATE_PRE PlatePreRunC
#define PLATE_INPUT PlateInputRunC
#define PLATE_TANK PlateTankRunC
#define PLATE_XFADE PlateXfadeRunC
#endif

// --- Control ---

void PlateControl::Init() {
  started_ = false;
  steady_ = false;
  lfo_phase_ = 0;
  size_ = size_new_ = kPlateNumSizes - 1;
  fade_ = kFadeOne;
  xf_ = 0;
  xfading_ = false;
  gain_ = 0;
  pre_ = 0;
  mod_[0] = mod_[1] = xmod_[0] = xmod_[1] = 0;
  decay_ = damp_ = diffusion_ = motion_ = lowcut_ = 0;
}

// A modulated allpass's delay at a size and an LFO phase, Q16 samples.
int32_t PlateControl::ModTarget(int32_t size, int32_t j) const {
  const PlateSizeTable& t = kPlateSizes[size];
  const int32_t len = j ? t.a3 : t.a1;
  const int32_t lfo = Sine(lfo_phase_ + (j ? 0x40000000u : 0u));     // A3 in quadrature
  return (len << 16) + (((motion_ >> 8) * lfo) >> 10);               // excursion Q11
}

namespace {

bool SameParams(const PlateParams& a, const PlateParams& b) {
  return a.decay == b.decay && a.damp == b.damp && a.predelay == b.predelay && a.size == b.size &&
         a.diffusion == b.diffusion && a.motion == b.motion && a.lowcut == b.lowcut &&
         a.freeze == b.freeze;
}

}  // namespace

// Modulation: a 1 Hz parabolic sine, A3 in quadrature; ramped per sample to
// the value at the end of the call.
void PlateControl::UpdateMod(int32_t n, PlateCoefs* c) {
  lfo_phase_ += kLfoIncrement * static_cast<uint32_t>(n);
  for (int32_t j = 0; j < 2; ++j) {
    c->mod0[j] = mod_[j];
    c->mod_step[j] = Ramp(ModTarget(size_, j) - mod_[j], n);
    mod_[j] += c->mod_step[j] * n;
  }
}

void PlateControl::Update(const PlateParams& p, int32_t n, PlateCoefs* c) {
  if (steady_ && SameParams(p, last_)) {
    *c = cached_;
    UpdateMod(n, c);
    return;
  }
  const int32_t decay_t = p.freeze ? 32767 : 6554 + ((Clamp(p.decay, 0, 32767) * 25559) >> 15);
  const int32_t klp_t = p.freeze ? 32767 : Lookup(kDampTable, Clamp(p.damp, 0, 32767));
  const int32_t diff_t = Clamp(p.diffusion, 0, 32767);
  const int32_t exc_t = (Clamp(p.motion, 0, 32767) * 1517) >> 10;     // Q11 samples, <= 23.7
  const int32_t khp_t = Lookup(kLowCutTable, Clamp(p.lowcut, 0, 32767));
  const int32_t size_t_ = Clamp(p.size, 0, kPlateNumSizes - 1);
  // Integer samples: a still pre-delay reads without interpolation.
  const int32_t pre_t = ((Clamp(p.predelay, 0, 32767) * 8820) + 32768) & ~0xffff;
  const int32_t gain_t = p.freeze ? 0 : kInputGain;

  const bool first = !started_;
  if (first) {
    started_ = true;
    decay_ = decay_t << 8;
    damp_ = klp_t << 8;
    diffusion_ = diff_t << 8;
    motion_ = exc_t << 8;
    lowcut_ = khp_t << 8;
    size_ = size_new_ = size_t_;
    fade_ = kFadeOne;
    gain_ = gain_t;
    pre_ = pre_t;
  } else {
    decay_ = Smooth(decay_, decay_t);
    damp_ = Smooth(damp_, klp_t);
    diffusion_ = Smooth(diffusion_, diff_t);
    motion_ = Smooth(motion_, exc_t);
    lowcut_ = Smooth(lowcut_, khp_t);
  }

  const int32_t decay = decay_ >> 8;
  c->decay = decay;
  c->dd1 = kDd1;
  c->dd2 = Clamp(decay + 4915, 8192, 16384);                       // decay + 0.15 in [0.25, 0.5]
  c->klp = damp_ >> 8;
  const int32_t diffusion = diffusion_ >> 8;
  c->g1 = (diffusion * 24576) >> 15;                               // 0.75 x
  c->g2 = (diffusion * 20480) >> 15;                               // 0.625 x
  c->khp = lowcut_ >> 8;

  // Input gain: to its target within 4 calls (freeze gates it).
  int32_t gain_end = gain_t;
  if (gain_end > gain_ + kInputGain / 4) gain_end = gain_ + kInputGain / 4;
  if (gain_end < gain_ - kInputGain / 4) gain_end = gain_ - kInputGain / 4;
  c->gain0 = gain_;
  c->gain_step = Ramp(gain_end - gain_, n);
  gain_ += c->gain_step * n;

  // Pre-delay: glides an eighth of the way each call, <= 1/4 sample per sample.
  int32_t d = pre_t - pre_;
  int32_t pre_end;
  if (d < 4096 && d > -4096) {
    pre_end = pre_t;
  } else {
    d >>= 3;
    const int32_t slew = kPreSlew * n;
    pre_end = pre_ + Clamp(d, -slew, slew);
  }
  c->pre0 = pre_;
  c->pre_step = Ramp(pre_end - pre_, n);
  pre_ += c->pre_step * n;
  if (pre_end == pre_t && pre_ != pre_t && c->pre_step == 0) pre_ = pre_t;

  // Size: the wet fades out; muted, the reads crossfade to the new size
  // (kXfadeSamples); the wet fades back in. (Upstream's test-only switch
  // to change sizes at once is gone.)
  const bool jump = first;
  if (!xfading_ && fade_ == 0 && size_ != size_t_) {
    xfading_ = true;
    size_new_ = size_t_;
    xf_ = 0;
    xmod_[0] = ModTarget(size_new_, 0);
    xmod_[1] = ModTarget(size_new_, 1);
  }
  const bool muting = size_ != size_t_ || xfading_;
  int32_t fade_end;
  if (muting) {
    fade_end = fade_ > kFadeOne / 4 ? fade_ - kFadeOne / 4 : 0;
  } else {
    fade_end = fade_ < kFadeOne - kFadeOne / 8 ? fade_ + kFadeOne / 8 : kFadeOne;
  }
  c->fade0 = fade_;
  c->fade_step = Ramp(fade_end - fade_, n);
  fade_ += c->fade_step * n;
  if (fade_end == 0 || fade_end == kFadeOne) fade_ = fade_end;   // within n LSBs
  c->size = size_;
  c->size_new = size_new_;
  c->xfade = xfading_;

  // Modulation: a 1 Hz parabolic sine, A3 in quadrature; ramped per sample
  // to the value at the end of the call.
  if (jump) {
    mod_[0] = ModTarget(size_, 0);
    mod_[1] = ModTarget(size_, 1);
  }
  const int32_t size_before = size_;
  UpdateMod(n, c);
  if (xfading_) {
    for (int32_t j = 0; j < 2; ++j) {
      c->xmod0[j] = xmod_[j];
      c->xmod_step[j] = Ramp(ModTarget(size_new_, j) - xmod_[j], n);
      xmod_[j] += c->xmod_step[j] * n;
    }
    const int32_t per_call = (kFadeOne / kXfadeSamples) * n;
    const int32_t xf_end = xf_ < kFadeOne - per_call ? xf_ + per_call : kFadeOne;
    c->xf0 = xf_;
    c->xf_step = Ramp(xf_end - xf_, n);
    xf_ += c->xf_step * n;
    if (xf_end == kFadeOne) {
      // Done: the new size from the next call on.
      size_ = size_new_;
      mod_[0] = xmod_[0];
      mod_[1] = xmod_[1];
      xfading_ = false;
    }
  } else {
    c->xmod0[0] = c->xmod0[1] = c->xmod_step[0] = c->xmod_step[1] = 0;
    c->xf0 = c->xf_step = 0;
  }

  // Settled: the next calls with the same knobs take the previous
  // coefficients (bit-identical: every smoother sits on its target, every
  // ramp's step is 0).
  steady_ = !xfading_ && size_ == size_t_ && size_before == size_ && fade_ == kFadeOne &&
            c->fade_step == 0 && c->gain_step == 0 && c->pre_step == 0 && gain_ == gain_t &&
            pre_ == pre_t && decay_ == decay_t << 8 && damp_ == klp_t << 8 &&
            diffusion_ == diff_t << 8 && motion_ == exc_t << 8 && lowcut_ == khp_t << 8;
  if (steady_) {
    last_ = p;
    cached_ = *c;
  }
}

// --- The reverb ---

namespace {

void SizeOffsets(int32_t size, int32_t* off) {
  const PlateSizeTable& t = kPlateSizes[size];
  for (int32_t j = 0; j < 6; ++j) off[j] = 2 * (kTailBase[j] + t.tail[j]);
  for (int32_t j = 0; j < 7; ++j) {
    off[PT_TL + j] = 2 * (kTapBaseL[j] + t.tap_l[j]);
    off[PT_TR + j] = 2 * (kTapBaseR[j] + t.tap_r[j]);
  }
}

}  // namespace

void PlateReverb::ClearBuffers() {
  for (int32_t i = 0; i < PL_PRE_WORDS; ++i) pre_[i] = 0;
  for (int32_t i = 0; i < PL_IN_WORDS; ++i) in_buf_[i] = 0;
  for (int32_t i = 0; i < PL_TANK_WORDS; ++i) tank_[i] = 0;
  for (int32_t i = 0; i < kPlateMaxBlock; ++i) x_[i] = 0;
}

// Upstream's Init() without the buffer loops (76 KB: not on the audio
// thread in one go, guidelines §2.3; the glue clears them beforehand).
void PlateReverb::Reset() {
  control_.Init();
  ik_.hp = 0;
  tk_.lp = 0;
  tank_size_ = -1;
  in_ptr_ = PL_IN_SIZE;
  tank_ptr_ = PL_TANK_SIZE;
  pre_w_ = 0;
}

// The stereo sum into the pre-delay ring, and the delayed signal into x_;
// the write's wrap splits the run.
void PlateReverb::Pre(const int16_t* in_l, const int16_t* in_r, int32_t n) {
  int32_t w = pre_w_;
  int32_t d = coefs_.pre0;
  const int32_t step = coefs_.pre_step;
  int32_t i = 0;
  while (i < n) {
    int32_t m = PL_PRE_WORDS - w;
    if (m > n - i) m = n - i;
    PLATE_PRE(in_l + i, in_r + i, pre_, x_ + i, w, d, step, m);
    d += step * m;
    w += m;
    if (w == PL_PRE_WORDS) w = 0;
    i += m;
  }
  pre_w_ = w;
}

void PlateReverb::Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l,
                          int16_t* wet_r, int32_t n, const PlateParams& params) {
  control_.Update(params, n, &coefs_);
  const PlateCoefs& c = coefs_;
  if (c.size != tank_size_) {
    tank_size_ = c.size;
    SizeOffsets(c.size, tk_.off);
  }
  tk_.decay_klp = Pack(c.decay, c.klp);
  tk_.dd1 = Pack(c.dd1, -c.dd1);
  tk_.dd2 = Pack(-c.dd2, c.dd2);
  tk_.gout = Pack(kOutGain, -kOutGain);
  tk_.pos1 = c.mod0[0];
  tk_.step1 = c.mod_step[0];
  tk_.pos2 = c.mod0[1];
  tk_.step2 = c.mod_step[1];
  if (c.xfade) {
    int32_t off[PT_NUM_OFF];
    SizeOffsets(c.size_new, off);
    for (int32_t j = 0; j < 6; ++j) xk_.off[j] = off[j];
    xk_.xf = c.xf0;
    xk_.xf_step = c.xf_step;
    xk_.pos1 = c.xmod0[0];
    xk_.step1 = c.xmod_step[0];
    xk_.pos2 = c.xmod0[1];
    xk_.step2 = c.xmod_step[1];
  }
  ik_.g1 = Pack(-c.g1, c.g1);
  ik_.g2 = Pack(-c.g2, c.g2);
  ik_.khp = c.khp;
  ik_.gain = c.gain0;
  ik_.gstep = c.gain_step;

  Pre(in_l, in_r, n);

  // Input diffusers: runs end where p wraps and where the mirroring starts.
  int32_t done = 0;
  while (done < n) {
    const int32_t next = in_ptr_ - 1;
    int32_t m = next + 1;
    const bool alias = next < PL_IN_MAX_LINE;
    if (!alias && m > next - PL_IN_MAX_LINE + 1) m = next - PL_IN_MAX_LINE + 1;
    if (m > n - done) m = n - done;
    const int32_t alias_bytes = alias ? 2 * PL_IN_SIZE : 0;
    PLATE_INPUT(&ik_, x_ + done, x_ + done, in_buf_ + in_ptr_, m, alias_bytes);
    in_ptr_ -= m;
    if (in_ptr_ == 0) in_ptr_ = PL_IN_SIZE;
    done += m;
  }

  // Tank.
  done = 0;
  while (done < n) {
    const int32_t next = tank_ptr_ - 1;
    int32_t m = next + 1;
    const bool alias = next < PL_TANK_MAX_LINE;
    if (!alias && m > next - PL_TANK_MAX_LINE + 1) m = next - PL_TANK_MAX_LINE + 1;
    if (m > n - done) m = n - done;
    const int32_t alias_bytes = alias ? 2 * PL_TANK_SIZE : 0;
    int16_t* const p = tank_ + tank_ptr_;
    if (c.xfade) {
      PLATE_XFADE(&tk_, &xk_, x_ + done, p, m, alias_bytes);
    } else {
      PLATE_TANK(&tk_, x_ + done, wet_l + done, wet_r + done, p, m, alias_bytes);
    }
    tank_ptr_ -= m;
    if (tank_ptr_ == 0) tank_ptr_ = PL_TANK_SIZE;
    done += m;
  }

  // The size change's fade; muted during the crossfade.
  if (c.xfade) {
    for (int32_t i = 0; i < n; ++i) wet_l[i] = 0;
    for (int32_t i = 0; i < n; ++i) wet_r[i] = 0;
  } else if (c.fade0 != PlateControl::kFadeOne || c.fade_step != 0) {
    int32_t fade = c.fade0;
    for (int32_t i = 0; i < n; ++i) {
      const int32_t g = fade >> 16;
      fade += c.fade_step;
      wet_l[i] = static_cast<int16_t>((wet_l[i] * g) >> 15);
      wet_r[i] = static_cast<int16_t>((wet_r[i] * g) >> 15);
    }
  }
}

#undef PLATE_PRE
#undef PLATE_INPUT
#undef PLATE_TANK
#undef PLATE_XFADE

}  // namespace csplate
