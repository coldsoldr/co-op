// Fixed-point Rings reverb. See reverb_fixed.h.
//
// Per sample, upstream's Reverb::Process in the same order, with the
// FxEngine context (accumulator, previous read) in locals.

#include "src/reverb_fixed.h"

#include "src/fixed.h"
#include "src/fixed_tables.h"

#include "src/rate_config.h"

namespace op1rings {

// The assembly kernel's parameters and state (src/reverb_bfin.S).
struct ReverbKernel {
  int32_t gain;       // 0
  int32_t krt;        // 4
  int32_t klp;        // 8
  int32_t amount;     // 12
  int32_t lp_1;       // 16
  int32_t lp_2;       // 20
  int32_t d2_offset;  // 24: bytes from p to del2's modulated tap
  int32_t d2_frac;    // 28: Q31
  int32_t d1_offset;  // 32
  int32_t d1_frac;    // 36
  int32_t wet_1;      // 40: scratch
  int32_t kap;        // 44
};

extern "C" void ReverbKernelAsm(ReverbKernel* k, int32_t* in_out, int16_t* p, int32_t size,
                                int32_t alias_bytes);

namespace {

const int32_t kMask = ReverbFixed::kSize - 1;

// Compile-time constants are integer literals (coop: no float or double in a plugin file; defloat.py made them
// from upstream's Q(x, n) = x * 2^n rounded, at OP1RINGS_SAMPLE_RATE 22050).
static_assert(OP1RINGS_SAMPLE_RATE == 22050, "the literals are for 22.05 kHz (defloat.py)");

// Upstream's delay lines: lengths, and bases packed with one sample between
// (FxEngine::DelayLine).
// At 22.05 kHz every length halves, the modulated taps and their depth too
// (src/rate_config.h), so the room keeps its size and its modulation in
// seconds; src/reverb_bfin.S derives the same numbers.
const int32_t kDiv = OP1RINGS_REVERB_DIV;
const int32_t kAp1 = 150 / kDiv, kAp2 = 214 / kDiv, kAp3 = 319 / kDiv, kAp4 = 527 / kDiv;
const int32_t kDel1Tap = 4460 / kDiv, kDel1Mod = 40 / kDiv;   // del1's read: tap + mod x lfo, interpolated
const int32_t kDel2Tap = 6261 / kDiv, kDel2Mod = 50 / kDiv;
const int32_t kDap1a = 2182 / kDiv, kDap1b = 2690 / kDiv, kDel1 = kDel1Tap + kDel1Mod + 1;
const int32_t kDap2a = 2525 / kDiv, kDap2b = 2197 / kDiv, kDel2 = kDel2Tap + kDel2Mod + 1;
const int32_t kAp1Base = 0;
const int32_t kAp2Base = kAp1Base + kAp1 + 1;
const int32_t kAp3Base = kAp2Base + kAp2 + 1;
const int32_t kAp4Base = kAp3Base + kAp3 + 1;
const int32_t kDap1aBase = kAp4Base + kAp4 + 1;
const int32_t kDap1bBase = kDap1aBase + kDap1a + 1;
const int32_t kDel1Base = kDap1bBase + kDap1b + 1;
const int32_t kDap2aBase = kDel1Base + kDel1 + 1;
const int32_t kDap2bBase = kDap2aBase + kDap2a + 1;
const int32_t kDel2Base = kDap2bBase + kDap2b + 1;
static_assert(kDel2Base + kDel2 <= ReverbFixed::kSize, "reverb memory full");

// The furthest back any read goes: del2's modulated tap, 6261 + 50 + 1 (= kDel2).
const int32_t kMaxReach = kDel2;
static_assert(ReverbFixed::kSize - 1 + kDel2Base + kMaxReach < ReverbFixed::kBufferSize,
              "reverb buffer: reads");
static_assert(kMaxReach - 1 + kDel2Base + ReverbFixed::kSize < ReverbFixed::kBufferSize,
              "reverb buffer: wrapped writes");

// The LFOs: upstream's approximate cosine oscillators, stepped every 32
// samples, run at sqrt(32) / (2 pi) of their nominal 0.5 and 0.3 Hz.
// LfoIncrement(hz) = 0.9003163161571061 hz 32 / OP1RINGS_SAMPLE_RATE 2^32.
const uint32_t kLfoIncrement[2] = { 2805862u /* LfoIncrement(0.5) */, 1683517u /* LfoIncrement(0.3) */ };

// 0.5 + 0.5 cos(2 pi phase), Q31.
inline int32_t Lfo(uint32_t phase) {
  const uint32_t p = phase + 0x40000000u;
  const uint32_t i = p >> 20;
  const int32_t frac = static_cast<int32_t>((p & 0xfffff) << 11);
  const int32_t s = kSineQ31[i] + MulQ31Fast(kSineQ31[i + 1] - kSineQ31[i], frac);
  return (1 << 30) + (s >> 1);
}

// Upstream's 16-bit format: value * 32768, clipped (Q27 -> Q15, floored).
// On Blackfin the saturating shift is one instruction (ShlSat's generic
// code branches, and GCC 14's builtin can't be trusted with constant shifts:
// fixed.h).
inline int16_t Compress(int32_t acc) {
#ifdef __bfin__
  int32_t r;
  __asm__("%0 = %1 << 4 (S);" : "=d"(r) : "d"(acc));
  return static_cast<int16_t>(r >> 16);
#else
  return static_cast<int16_t>(ShlSat(acc, 4) >> 16);
#endif
}

}  // namespace

void ReverbFixed::Init() {
  amount_ = 0;
  input_gain_ = 429496730 /* Q(0.2, 31) */;
  time_ = 1073741824 /* Q(0.5, 31) */;
  diffusion_ = 1342177280 /* Q(0.625, 31) */;
  lp_ = 1503238554 /* Q(0.7, 31) */;
  Clear();
}

void ReverbFixed::Clear() {
  lp_decay_1_ = lp_decay_2_ = 0;
  write_ptr_ = 0;
  age_ = 0;
  // Upstream's oscillators start at their peak (value() = 1 before the
  // first step).
  lfo_phase_[0] = lfo_phase_[1] = 0;
  lfo_value_[0] = lfo_value_[1] = INT32_MAX;
}

void ReverbFixed::Process(int32_t* in_out, size_t size) {
  // Runs over which the write pointer stays either within kMaxReach of its
  // wrap (writes go to two places) or not.
  while (size) {
    const int32_t next = (write_ptr_ - 1) & kMask;
    const bool alias = next < kMaxReach;
    size_t n = static_cast<size_t>(alias ? next + 1 : next - kMaxReach + 1);
    if (n > size) n = size;
    const bool fresh = age_ <= kMaxReach;
#if defined(__bfin__) && !defined(OP1RINGS_NO_ASM)
    if (alias) {
      if (fresh) ProcessInternal<true, true>(in_out, n); else ProcessAsm<true>(in_out, n);
    } else {
      if (fresh) ProcessInternal<true, false>(in_out, n); else ProcessAsm<false>(in_out, n);
    }
#else
    if (alias) {
      if (fresh) ProcessInternal<true, true>(in_out, n); else ProcessInternal<false, true>(in_out, n);
    } else {
      if (fresh) ProcessInternal<true, false>(in_out, n); else ProcessInternal<false, false>(in_out, n);
    }
#endif
    in_out += n;
    size -= n;
  }
}

template<bool kAlias>
void ReverbFixed::ProcessAsm(int32_t* in_out, size_t size) {
  ReverbKernel k;
  k.gain = input_gain_;
  k.krt = time_;
  k.klp = lp_;
  k.amount = amount_;
  k.kap = diffusion_;
  k.lp_1 = lp_decay_1_;
  k.lp_2 = lp_decay_2_;
  while (size) {
    // Runs between the LFOs' steps (every 32 samples, FxEngine::Start).
    const int32_t next = (write_ptr_ - 1) & kMask;
    if ((next & 31) == 0) {
      lfo_value_[0] = Lfo(lfo_phase_[0]);
      lfo_value_[1] = Lfo(lfo_phase_[1]);
      lfo_phase_[0] += kLfoIncrement[0];
      lfo_phase_[1] += kLfoIncrement[1];
    }
    size_t n = (next & 31) == 0 ? 32 : static_cast<size_t>(next & 31);
    if (n > size) n = size;
    const int32_t offset_2 = (kDel2Tap << 16) + MulQ31(lfo_value_[1], kDel2Mod << 16);
    const int32_t offset_1 = (kDel1Tap << 16) + MulQ31(lfo_value_[0], kDel1Mod << 16);
    k.d2_offset = 2 * (kDel2Base + (offset_2 >> 16));
    k.d2_frac = (offset_2 & 0xffff) << 15;
    k.d1_offset = 2 * (kDel1Base + (offset_1 >> 16));
    k.d1_frac = (offset_1 & 0xffff) << 15;
    // The kernel moves its pointer down before each sample and doesn't
    // wrap it: wrap here (0 is kSize), as the C loop's mask does. A run
    // never crosses the wrap (Process ends alias runs at 0).
    const int32_t start = write_ptr_ == 0 ? kSize : write_ptr_;
    ReverbKernelAsm(&k, in_out, buffer_ + start, static_cast<int32_t>(n),
                    kAlias ? 2 * kSize : 0);
    write_ptr_ = start - static_cast<int32_t>(n);
    in_out += n;
    size -= n;
  }
  lp_decay_1_ = k.lp_1;
  lp_decay_2_ = k.lp_2;
}

template<bool kFresh, bool kAlias>
void ReverbFixed::ProcessInternal(int32_t* in_out, size_t size) {
  const int32_t kap = diffusion_;
  const int32_t klp = lp_;
  const int32_t krt = time_;
  const int32_t amount = amount_;
  const int32_t gain = input_gain_;
  int16_t* const buffer = buffer_;
  int32_t write_ptr = write_ptr_;
  const int16_t* p = buffer;
  int32_t age = age_;
  int32_t lp_1 = lp_decay_1_;
  int32_t lp_2 = lp_decay_2_;
  int32_t lfo_1 = lfo_value_[0];
  int32_t lfo_2 = lfo_value_[1];

  // A read k samples back (Q27); after a Clear, what wasn't written since is
  // 0. A write at the line's start.
#define READ(base, k) \
  ((kFresh && (k) > age) ? 0 : static_cast<int32_t>(p[(base) + (k)]) << 12)
#define WRITE(base, value)                                   \
  {                                                          \
    const int16_t w = Compress(value);                       \
    int16_t* q = buffer + write_ptr + (base);                \
    q[0] = w;                                                \
    if (kAlias) q[ReverbFixed::kSize] = w;                   \
  }
  // Read(TAIL, read_scale); WriteAllPass(write_scale).
#define ALLPASS(base, length, read_scale, write_scale) \
  {                                                    \
    const int32_t previous = READ(base, (length) - 1); \
    acc += MulQ31Fast(previous, read_scale);           \
    WRITE(base, acc);                                  \
    acc = MulQ31Fast(acc, write_scale) + previous;     \
  }

  const int32_t minus_kap = -kap;
  for (size_t i = 0; i < size; ++i) {
    // FxEngine::Start.
    write_ptr = (write_ptr - 1) & kMask;
    p = buffer + write_ptr;
    if ((write_ptr & 31) == 0) {
      lfo_1 = Lfo(lfo_phase_[0]);
      lfo_2 = Lfo(lfo_phase_[1]);
      lfo_phase_[0] += kLfoIncrement[0];
      lfo_phase_[1] += kLfoIncrement[1];
    }
    const int32_t dry = in_out[i];
    int32_t acc = MulQ31Fast(dry, gain);

    // Diffuse through 4 allpasses.
    ALLPASS(kAp1Base, kAp1, kap, minus_kap)
    ALLPASS(kAp2Base, kAp2, kap, minus_kap)
    ALLPASS(kAp3Base, kAp3, kap, minus_kap)
    ALLPASS(kAp4Base, kAp4, kap, minus_kap)
    const int32_t apout = acc;

    // Main loop, first branch: del2 at 6261 + 50 lfo_2 (interpolated).
    {
      const int32_t offset = (kDel2Tap << 16) + MulQ31(lfo_2, kDel2Mod << 16);
      const int32_t k = offset >> 16;
      const int32_t a = READ(kDel2Base, k);
      const int32_t b = READ(kDel2Base, k + 1);
      acc = apout + MulQ31Fast(a + MulQ31Fast(b - a, (offset & 0xffff) << 15), krt);
    }
    lp_1 += MulQ31Fast(klp, acc - lp_1);
    acc = lp_1;
    ALLPASS(kDap1aBase, kDap1a, minus_kap, kap)
    ALLPASS(kDap1bBase, kDap1b, kap, minus_kap)
    WRITE(kDel1Base, acc);
    const int32_t wet_1 = acc << 1;

    // Second branch: del1 at 4460 + 40 lfo_1.
    {
      const int32_t offset = (kDel1Tap << 16) + MulQ31(lfo_1, kDel1Mod << 16);
      const int32_t k = offset >> 16;
      const int32_t a = READ(kDel1Base, k);
      const int32_t b = READ(kDel1Base, k + 1);
      acc = apout + MulQ31Fast(a + MulQ31Fast(b - a, (offset & 0xffff) << 15), krt);
    }
    lp_2 += MulQ31Fast(klp, acc - lp_2);
    acc = lp_2;
    ALLPASS(kDap2aBase, kDap2a, kap, minus_kap)
    ALLPASS(kDap2bBase, kDap2b, minus_kap, kap)
    WRITE(kDel2Base, acc);
    const int32_t wet_2 = acc << 1;

    // Upstream: left += (wet_1 - left) amount, right likewise; summed.
    in_out[i] = dry + MulQ31Fast(amount, wet_1 + wet_2 - dry);
    if (kFresh && age < ReverbFixed::kSize) ++age;
  }
#undef ALLPASS
#undef WRITE
#undef READ

  write_ptr_ = write_ptr;
  age_ = age;
  lp_decay_1_ = lp_1;
  lp_decay_2_ = lp_2;
  lfo_value_[0] = lfo_1;
  lfo_value_[1] = lfo_2;
}

}  // namespace op1rings
