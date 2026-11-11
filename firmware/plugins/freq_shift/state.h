/* freq_shift: the state in the slot's position buffer (sdk/coop_plugin.h e8), shared by the render (freq_shift.cc) and
 * the screen (freq_shift_screen.cc, e9: it reads only `disp`, through coop_fx_view_t.state while the view is live).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_FREQ_SHIFT_STATE_H_
#define COOP_FREQ_SHIFT_STATE_H_
#include <stdint.h>
#include "src/shift.h"

namespace coop_freq_shift {

/* What the render publishes for the screen (sdk/coop_plugin.h "Display data"): whole words, written once a block. */
struct Display {
  int32_t level;   /* the input's peak (top 16 bits of L and R, 0..32767), decaying by an eighth a block */
};

/* The words the mapped parameters were last computed from (turns 0..3, word 5), and the mapped values. */
struct State {
  op1fx::FreqShift shift;
  int16_t l[SH_MAX_BLOCK], r[SH_MAX_BLOCK];
  int32_t words[5];
  int32_t mapped[5];   /* plain words: a ShiftParams member would need a constructor (its gain initialiser) */
  Display disp;
};

}  // namespace coop_freq_shift

#endif
