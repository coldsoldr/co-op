// What the Rings engine needs from its host, for the coop build
// (plugins/rings/rings.cc). The te-op1-cfw original named firmware
// addresses (the pitch buffer, the knob words, the voice objects, the stock
// phase engine): a coop plugin takes all of those from the engine ABI's
// arguments instead (sdk/coop_plugin.h), so none are left here.

#ifndef OP1RINGS_OP1_OP1_H_
#define OP1RINGS_OP1_OP1_H_

#include <cstdint>

#include "coop_plugin.h"

namespace op1 {

const int32_t kNumVoices = COOP_VOICES;        // 6
const int32_t kBlockSize = COOP_VOICE_BLOCK;   // 128 samples a render, at 88.2 kHz
const int32_t kVoiceSampleRate = 88200;

#ifdef __bfin__
// The core clock's cycle counter (a register, not memory): the shared
// reverb's idle gap (op1/reverb_bus.h) is measured with it.
inline uint32_t Cycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#else
// The host test (tests/rings_host.cc) steps its own counter.
extern uint32_t host_cycles;
inline uint32_t Cycles() { return host_cycles; }
#endif

inline int16_t Saturate16(int32_t x) {
#ifdef __bfin__
  // `<< 16 (S)` then `>>> 16`: the clamp in two instructions (GCC compiles
  // the comparisons below to branches).
  int32_t r;
  __asm__("%0 = %1 << 16 (S);" : "=d"(r) : "d"(x));
  return static_cast<int16_t>(r >> 16);
#else
  return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
#endif
}

}  // namespace op1

#endif  // OP1RINGS_OP1_OP1_H_
