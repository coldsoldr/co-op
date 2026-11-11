/* bitcrusher: the state in the slot's position buffer (sdk/coop_plugin.h e8), shared by the render (bitcrusher.cc) and
 * the screen (bitcrusher_screen.cc, e9: it reads only `disp`, through coop_fx_view_t.state while the view is live).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_BITCRUSHER_STATE_H_
#define COOP_BITCRUSHER_STATE_H_
#include <stdint.h>
#include "src/crush.h"

namespace coop_bitcrusher {

/* What the render publishes for the screen (sdk/coop_plugin.h "Display data"): whole words, written once a block. */
struct Display {
  int32_t level;   /* the input's peak (top 16 bits, 0..32767), decaying by an eighth a block */
};

struct State {
  crush::Crush crush;
  Display disp;
};

}  // namespace coop_bitcrusher

#endif
