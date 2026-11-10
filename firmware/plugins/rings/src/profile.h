// Stage timers for the simulator profile (tests/host/rings, `make profile`, -DRINGS_PROFILE). Elsewhere every macro
// is empty: the plugin and the host test carry no trace of them.
#ifndef OP1RINGS_PROFILE_H_
#define OP1RINGS_PROFILE_H_

#include <cstdint>

namespace op1rings {

enum ProfileStage {
  PROF_SMOOTH, PROF_REVERB_BUS, PROF_VOICE, PROF_UPSAMPLE, PROF_TAIL,        // rings.cc Render
  PROF_EXCITATION, PROF_COEFFS, PROF_WEIGHTS, PROF_MODE_BANK,                // modal (op1_voice.cc, modal_resonator.cc)
  PROF_STRINGS, PROF_FM, PROF_LIMITER, PROF_PEAK, PROF_STRING_CONFIG, PROF_BATCH, PROF_COUNT
};

#ifdef RINGS_PROFILE
extern uint64_t g_profile[PROF_COUNT];
inline uint32_t ProfileNow() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#define PROF_START(t) const uint32_t t = ::op1rings::ProfileNow()
#define PROF_ADD(stage, t) (::op1rings::g_profile[::op1rings::stage] += ::op1rings::ProfileNow() - (t))
#else
#define PROF_START(t) do { } while (0)
#define PROF_ADD(stage, t) do { } while (0)
#endif

}  // namespace op1rings

#endif  // OP1RINGS_PROFILE_H_
