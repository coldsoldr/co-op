// The reference for the drive port: te-op1-cfw's DRIVE (ports/fx/src/drive, compiled with -Dop1fx=up so it sits
// beside the plugin's copy) behind the glue kennel's drive_knl.cc RenderStereo was: the four knobs through
// op1_fx_param_q15 (the filter type's ranges, the same row coop's core uses), word 5 through knl::GainQ14, and
// Drive::Process(l, r, 64). On the Blackfin the C passes run (set_use_asm(false)): the plugin's assembly is checked
// against upstream's C. Not a device artifact.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cstdint>

#include "src/drive/drive.h"   // upstream, with -I$(TE)

namespace {

up::Drive ref;

// kennel synth_sdk/op1_plugin.h op1_fx_param_q15, with the filter row (0x01DE5C50 + 5 x 0x30) as a table.
const int16_t kRange[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };
int32_t ParamQ15(const int16_t* p, int i) {
  int32_t lo = kRange[i][0], hi = kRange[i][1], v = p[i];
  if (hi <= lo) return v < 0 ? 0 : v;
  v = (v - lo) * 32767 / (hi - lo);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

// te-op1-cfw ports/knl/common/knl_fx.h, verbatim.
inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
inline int32_t Word(const int16_t* p, int32_t k) { return Clamp15(p[k]); }
inline int32_t GainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

}  // namespace

extern "C" void drive_ref_init(void) {
  ref.Init();
  ref.set_use_asm(false);
}

// drive_knl.cc RenderStereo (its fine.Apply, which rewrote word 5 from the stock shift byte, is the core's now).
extern "C" void drive_ref_render(int16_t* l, int16_t* r, const int16_t* p, int n) {
  up::DriveParams q;
  q.drive = ParamQ15(p, 0);
  q.tone = ParamQ15(p, 1);
  q.bias = ParamQ15(p, 2);
  q.mix = ParamQ15(p, 3);
  q.gain = GainQ14(Word(p, 5));
  ref.Process(l, r, n, q);
}
