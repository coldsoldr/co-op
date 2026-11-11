// The core clock's cycle counter, for the half-rate path's pause check (src/half_rate.h). On the Blackfin it is the
// CYCLES register (a register, not memory: no firmware address), as plugins/rings/op1/op1.h reads it; host builds
// (tests/host/ensemble) step their own counter.

#ifndef COOP_ENSEMBLE_CYCLES_H_
#define COOP_ENSEMBLE_CYCLES_H_

#include <cstdint>

namespace coop_ensemble {

#ifdef __bfin__
inline uint32_t Cycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#else
extern uint32_t host_cycles;
inline uint32_t Cycles() { return host_cycles; }
#endif

}  // namespace coop_ensemble

#endif  // COOP_ENSEMBLE_CYCLES_H_
