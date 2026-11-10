// Fixed-point modal resonator. See modal_resonator.h.

#include "src/modal_resonator.h"
#include "src/rate_config.h"

#include "src/ffloat.h"
#include "src/fixed.h"
#include "src/fixed_tables.h"
#include "src/profile.h"
#include "src/scratch.h"


namespace op1rings {

static_assert(kMaxModalBlockSize <= kScratchSubblock, "modal scratch");

namespace {

const int32_t kOne = INT32_MAX;  // ~1.0 in Q31

// Linear interpolation in a pf table (1024 + 1 entries over [0, 0.5]).
inline int32_t InterpPf(const int32_t* table, int32_t pf) {
  if (pf >= (1 << 30)) return table[1 << kPfTableBits];
  if (pf < 0) pf = 0;
  int32_t i = pf >> 20;
  int32_t frac = (pf & 0xfffff) << 11;
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

// Linear interpolation in a 256 + 1 table over [0, 1) (x in Q31).
inline int32_t Interp8(const int32_t* table, int32_t x) {
  if (x < 0) x = 0;
  int32_t i = x >> 23;
  int32_t frac = (x & 0x7fffff) << 8;
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

// Same, for tables over [0, 0.5] (x in Q31, <= 0.5).
inline int32_t InterpHalf(const int32_t* table, int32_t x) {
  if (x >= (1 << 30)) return table[1 << kTableBits];
  return Interp8(table, x << 1);
}

// log2(v / 2^31) in Q24, for v > 0.
inline int32_t Log2Q24(int32_t v) {
  int32_t n = __builtin_clrsb(v);
  int32_t m = v << n;                    // [2^30, 2^31)
  int32_t t = m - (1 << 30);             // mantissa - 1, Q30
  int32_t i = t >> 22;
  int32_t frac = (t & 0x3fffff) << 9;
  int32_t l = kLog2MantissaQ30[i] +
      MulQ31Fast(kLog2MantissaQ30[i + 1] - kLog2MantissaQ30[i], frac);
  return (l >> 6) - ((1 + n) << 24);
}

// 2^-l for l >= 0 in Q24; result in Q31.
inline int32_t Exp2NegQ31(int32_t l) {
  int32_t n = l >> 24;
  if (n >= 31) return 0;
  int32_t frac = l & 0xffffff;
  int32_t i = frac >> 16;
  int32_t f = (frac & 0xffff) << 15;
  int32_t m = kExp2NegQ31[i] + MulQ31Fast(kExp2NegQ31[i + 1] - kExp2NegQ31[i], f);
  return m >> n;
}

inline int32_t ShiftRight(int32_t x, int32_t n) {
  return n >= 31 ? (x < 0 ? -1 : 0) : x >> n;
}

const int32_t kLog2Of500Q24 = 150420900;         // log2(500) * 2^24
const int32_t k4Log2Of10Q24 = 222930821;         // 4 log2(10) * 2^24
const int32_t k0_2Q31 = 429496730;               // 0.2
const int32_t k0_85Q31 = 1825361101;             // 0.85
const int32_t k0_15Q30 = 161061274;              // 0.15
const int32_t k0_1Q31 = 214748365;               // 0.1
const int32_t k0_25Q31 = 1 << 29;                // 0.25
const int32_t k0_02Q31 = 42949673;               // 0.02

}  // namespace

uint32_t (*profile_clock)() = nullptr;
uint32_t profile_update_cycles = 0;
uint32_t profile_mode_bank_cycles = 0;

void ModalResonator::Init() {
  frequency_ = 10713070;        // 220 Hz at 44.1 kHz, Q31
  structure_ = 1 << 29;         // 0.25
  brightness_ = 1 << 30;        // 0.5
  damping_ = 644245094;         // 0.3
  position_ = 2145336164;       // 0.999
  previous_position_ = 0;
  resolution_ = kMaxModalModes;
  coefficients_valid_ = false;
  hold_ = false;
  changed_ = false;
  weights_valid_ = false;
  weights_n_ = 0;
  num_modes_ = 0;
  previous_input_ = 0;
  for (int32_t i = 0; i < kMaxModalModes; ++i) {
    Mode& m = modes_[i];
    m.lp = m.bp = m.half_f = m.half_q = m.weight = m.weight_increment = 0;
    gain_[i] = 0;
  }
}

int32_t ModalResonator::ComputeFilters() {
  if (coefficients_valid_ &&
      computed_frequency_ == frequency_ && computed_structure_ == structure_ &&
      computed_brightness_ == brightness_ && computed_damping_ == damping_ &&
      computed_resolution_ == resolution_) {
    return num_modes_;
  }
  changed_ = true;
  if (coefficients_valid_ && hold_) return num_modes_;
  if (!profile_clock) return RecomputeFilters();
  const uint32_t start = profile_clock();
  const int32_t num_modes = RecomputeFilters();
  profile_update_cycles += profile_clock() - start;
  return num_modes;
}

int32_t ModalResonator::RecomputeFilters() {
  weights_valid_ = false;   // the gains change
  // Before the first update the TPT state is zero; after it, translate.
  const bool first_update = !coefficients_valid_;
#ifdef OP1RINGS_NO_STATE_TRANSLATION
  const bool translate_state = false;
#else
  const bool translate_state = true;
#endif

  // The partial frequencies (and their sin/cos/sec/tan) depend only on
  // frequency, structure and resolution: reuse them when only brightness or
  // damping changed.
  const bool geometry_changed = first_update ||
      computed_frequency_ != frequency_ || computed_structure_ != structure_ ||
      computed_resolution_ != resolution_;

  static constexpr FFloat k049 = FFloat::Raw(2104533975, -32) /* FFloat(0.49) */;
  static constexpr FFloat k093 = FFloat::Raw(1997159792, -31) /* FFloat(0.93) */;
  static constexpr FFloat k098 = FFloat::Raw(2104533975, -31) /* FFloat(0.98) */;
  static constexpr FFloat k1 = FFloat::Raw(1073741824, -30) /* FFloat(1.0) */;

  // Per-block values (upstream ComputeFilters()).
  FFloat stiffness = FFloat::FromQ(Interp8(kStiffnessQ28, structure_), 28);
  const FFloat f0 = FFloat::FromQ(frequency_, 31);
  FFloat harmonic = f0;
  FFloat stretch_factor = k1;
  int32_t log2_q = kLog2Of500Q24 + MulQ31Fast(damping_, k4Log2Of10Q24);
#if OP1RINGS_SAMPLE_RATE == 22050
  // Upstream's mode Q is 1 + pf q with pf the frequency as a fraction of
  // the sample rate, so a mode's decay in seconds is q / (pi fs): at half
  // the rate every mode would ring twice as long. Halve q (src/rate_config.h).
  log2_q -= 1 << 24;
#elif OP1RINGS_SAMPLE_RATE != 44100
#error "modal Q: add the rate's log2 correction"
#endif

  int32_t one_minus = kOne - structure_;
  int32_t attenuation = MulQ31Fast(one_minus, one_minus);
  attenuation = MulQ31Fast(attenuation, attenuation);
  attenuation = MulQ31Fast(attenuation, attenuation);
  int32_t brightness = MulQ31Fast(brightness_, kOne - MulQ31Fast(k0_2Q31, attenuation));
  // q_loss = b (2 - b) 0.85 + 0.15, computed in Q30.
  int32_t b2 = (brightness >> 1) * 2 - (MulQ31Fast(brightness, brightness) >> 1);
  int32_t q_loss = ShlSat(MulQ31Fast(b2, k0_85Q31) + k0_15Q30, 1);   // Q31
  int32_t s2 = (structure_ >> 1) * 2 - (MulQ31Fast(structure_, structure_) >> 1);
  int32_t q_loss_damping_rate = ShlSat(MulQ31Fast(s2, k0_1Q31), 1);    // Q31

  const int32_t u0 = previous_input_;

  int32_t num_modes = geometry_changed ? 0 : num_modes_;
  for (int32_t i = 0; i < resolution_; ++i) {
    Geometry g;
    if (geometry_changed) {
      FFloat partial_frequency = harmonic * stretch_factor;
      if (partial_frequency >= k049) {
        partial_frequency = k049;
      } else {
        num_modes = i + 1;
      }
      const int32_t pf = partial_frequency.ToQ31();
      log2_pf_[i] = Log2Q24(pf);
      g.sin = InterpPf(kSinPhiQ31, pf);
      g.cos = InterpPf(kCosPhiQ31, pf);
      g.sec = InterpPf(kSecPhiQ28, pf);
      g.tan = InterpPf(kTanPhiQ28, pf);
    } else {
      const Geometry& o = geometry_[i];
      g.sin = o.sin;
      g.cos = o.cos;
      g.sec = o.sec;
      g.tan = o.tan;
    }
    // r = 1 / (1 + pf q), from L = log2(pf q).
    int32_t l = log2_pf_[i] + log2_q;
    int32_t r_m, r_shift;
    if (l <= 0) {
      r_m = Interp8(kRecip1pQ31, Exp2NegQ31(-l));
      r_shift = 0;
    } else {
      r_shift = l >> 24;
      int32_t t_m = Exp2NegQ31(l & 0xffffff);              // 2^-frac
      r_m = MulQ31Fast(t_m, Interp8(kRecip1pQ31, ShiftRight(t_m, r_shift)));
    }
    const int32_t sc = MulQ31Fast(g.sin, g.cos);
    g.x = ShiftRight(MulQ31Fast(r_m, sc), r_shift);
    g.rho = InterpHalf(kRhoQ31, g.x);

    Mode& mode = modes_[i];
    const int32_t half_f = MulQ31Fast(g.sin, g.rho);
    const int32_t half_q = ShiftRight(MulQ31Fast(MulQ31Fast(r_m, g.cos), g.rho), 1 + r_shift);
    gain_[i] = MulQ31Fast(g.cos, g.rho) >> 1;

    // Translate the state: Chamberlin (old coefficients) -> TPT -> Chamberlin
    // (new).
    int32_t s1 = 0, s2 = 0;
    if (!first_update) {
      const Geometry& o = geometry_[i];
      // A = cos^2 - sin^2 - x lies in about [-1.4, 1]: Q30.
      int32_t a_old = (MulQ31Fast(o.cos, o.cos) >> 1) - (MulQ31Fast(o.sin, o.sin) >> 1) - (o.x >> 1);
      int32_t k1 = MulQ31Fast(MulQ31Fast(o.rho, a_old), o.sec);              // Q27
      s1 = ShlSat(MulQ31Fast(mode.bp, k1), 3) +                           // /2, Q23 -> Q27
           ShlSat(MulQ31Fast(u0 - mode.lp, o.tan), 3);
      s2 = MulQ31Fast(mode.bp, mode.half_f) + mode.lp;
    }
    if (first_update || translate_state) {
      int32_t a_new = (MulQ31Fast(g.cos, g.cos) >> 1) - (MulQ31Fast(g.sin, g.sin) >> 1) - (g.x >> 1);
      int32_t inv_b = InterpHalf(kRecip1mQ30, g.x);                   // Q30
      int32_t inv_rho = InterpHalf(kInvRhoQ30, g.x);                  // Q30
      int32_t s_sq_u0 = MulQ31Fast(MulQ31Fast(g.sin, g.sin), u0);             // Q27
      // s2 A - 2 sin cos s1 + 2 sin^2 u0, in Q26.
      int32_t sum_lp = MulQ31Fast(s2, a_new) - (MulQ31Fast(s1, ShlSat(sc, 1)) >> 1) + s_sq_u0;
      mode.lp = ShlSat(MulQ31Fast(sum_lp, inv_b), 2);                     // Q25 -> Q27
      int32_t sum_bp = MulQ31Fast(s1, g.cos) + MulQ31Fast(s2 - u0, g.sin);
      mode.bp = ShlSat(MulQ31Fast(sum_bp, MulQ31Fast(inv_b, inv_rho)), 3);
    }
    mode.half_f = half_f;
    mode.half_q = half_q;
    geometry_[i] = g;

    // Next partial (upstream order).
    if (geometry_changed) {
      stretch_factor += stiffness;
      stiffness *= stiffness.is_negative() ? k093 : k098;
      harmonic += f0;
    }
    q_loss += MulQ31Fast(q_loss_damping_rate, kOne - q_loss);
    // q only shrinks (q_loss <= 1). Q24 holds +-128: clamp well before that;
    // below 2^-64, r = 1 / (1 + pf q) is 1 anyway.
    log2_q += Log2Q24(q_loss);
    if (log2_q < -(64 << 24)) log2_q = -(64 << 24);
  }

  computed_frequency_ = frequency_;
  computed_structure_ = structure_;
  computed_brightness_ = brightness_;
  computed_damping_ = damping_;
  computed_resolution_ = resolution_;
  coefficients_valid_ = true;
  num_modes_ = num_modes;
  return num_modes;
}

// Amplitude of each mode for a pickup position (stmlib CosineOscillator,
// approximate mode, iterated across modes), times the mode's gain.
void ModalResonator::ComputeWeights(int32_t position, int32_t* weights,
                                    int32_t n) const {
  int32_t f = position - k0_25Q31;
  int32_t negative = 0;
  if (f < 0) {
    f = -f;
  } else if (f > (1 << 30)) {
    f -= 1 << 30;
  } else {
    negative = 1;
  }
  int32_t one_minus_2f = kOne - ShlSat(f, 1);
  int32_t iir = MulQ31Fast(f, one_minus_2f) << 2;   // * 16, Q29
  if (negative) iir = -iir;
  int32_t y1 = iir;                              // iir * 0.25 in Q31
  int32_t y0 = 1 << 30;                          // 0.5
  for (int32_t i = 0; i < n; ++i) {
    int32_t temp = y0;
    y0 = ShlSat(MulQ31Fast(iir, y0), 2) - y1;
    y1 = temp;
    int32_t amplitude = AddSat(temp, 1 << 30);
    weights[i] = MulQ31Fast(amplitude, gain_[i]);
  }
}

void ModalResonator::Process(const int32_t* in, int32_t* out, int32_t* aux,
                             size_t size) {
  PROF_START(t_coeffs);
  int32_t num_modes = ComputeFilters();
  PROF_ADD(PROF_COEFFS, t_coeffs);
  PROF_START(t_weights);
  int32_t n = (num_modes + 1) & ~1;
  if (n > resolution_) n = resolution_;

  // Scratch shared by all voices (src/scratch.h: L1 on the OP-1).
  int32_t* in_scaled = scratch()->modal.in_scaled;
  int32_t* half_x = scratch()->modal.half_x;
  for (size_t s = 0; s < size; ++s) {
    in_scaled[s] = in[s] >> 3;   // upstream: input * 0.125
  }

  // Weights (amplitude * gain) move from the previous pickup position to the
  // new one over the block, as upstream interpolates the position per
  // sample. Amplitude isn't linear in position, so a large jump (the first
  // block, or a fast knob turn) is split into 4 linear segments.
  const int32_t delta = position_ - previous_position_;
  const bool large_jump = delta > k0_02Q31 || delta < -k0_02Q31;
  const int32_t segments = large_jump ? 4 : 1;
  const size_t segment_size = size / segments;
  // kOne / segment_size, folded for the usual sizes (a division is a libgcc
  // call on Blackfin).
  const int32_t recip_segment_size = segment_size == 32 ? kOne / 32
      : segment_size == 8 ? kOne / 8 : kOne / static_cast<int32_t>(segment_size);
  int32_t* start = scratch_start_;
  int32_t* end = scratch_end_;
  // The last block ended at previous_position_: its weights are still right
  // unless the gains (or the number of modes) changed since.
  const bool kept = weights_valid_ && weights_n_ == n;
  if (!kept) ComputeWeights(previous_position_, start, n);
  uint32_t weights_cycles = 0, bank_cycles = 0;
  (void)weights_cycles;
  (void)bank_cycles;
  const uint32_t profile_start = profile_clock ? profile_clock() : 0;
  for (int32_t k = 0; k < segments; ++k) {
    int32_t position = k + 1 == segments
        ? position_ : previous_position_ + (delta >> 2) * (k + 1);
#ifdef RINGS_PROFILE
    const uint32_t tw = ProfileNow();
#endif
    if (kept && delta == 0) {
      for (int32_t i = 0; i < n; ++i) end[i] = start[i];
    } else {
      ComputeWeights(position, end, n);
    }
    for (int32_t i = 0; i < n; ++i) {
      modes_[i].weight = start[i];
      modes_[i].weight_increment = MulQ31Fast(end[i] - start[i], recip_segment_size);
      start[i] = end[i];
    }
    const size_t offset = k * segment_size;
#ifdef RINGS_PROFILE
    const uint32_t tb = ProfileNow();
    weights_cycles += tb - tw;
#endif
    ModeBankProcess(modes_, n, in_scaled + offset, &previous_input_,
                    out + offset, aux + offset, segment_size, half_x);
#ifdef RINGS_PROFILE
    bank_cycles += ProfileNow() - tb;
#endif
  }
  if (profile_clock) profile_mode_bank_cycles += profile_clock() - profile_start;
#ifdef RINGS_PROFILE
  g_profile[PROF_MODE_BANK] += bank_cycles;
  g_profile[PROF_WEIGHTS] += ProfileNow() - t_weights - bank_cycles;
#endif
  previous_position_ = position_;
  weights_valid_ = true;
  weights_n_ = n;
}

}  // namespace op1rings
