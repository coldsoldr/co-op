/* petri: the state in the slot's position buffer (sdk/coop_plugin.h e8), shared by the render (petri.cc) and the screen
 * (petri_screen.cc, e9: it reads only `disp`, through coop_fx_view_t.state while the view is live).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_PETRI_STATE_H_
#define COOP_PETRI_STATE_H_
#include <stdint.h>
#include "src/half_rate.h"
#include "src/petri_engine.h"

namespace coop_petri {

const int32_t kBlock = petri_hb::kFxBlockSize;          /* 64 */
const int32_t kHalfN = petri_hb::kHalfN;                /* 32 */

/* What the render publishes for the screen (sdk/coop_plugin.h "Display data"): whole words, written once a block (the
 * clearing blocks too), after the DSP; the DSP never reads them. */
struct DisplayVoice {
    int32_t pos;                                       /* the read position in the buffer, 0..32767 (frames); -1 idle */
    int32_t rate;                                      /* the playback rate, Q8 signed: 256 = 1x forward, -512 = 2x back */
    int32_t level;                                     /* the envelope x the voice's gain, 0..32767 */
};

struct Display {
    int32_t effect;                                    /* the effect in force, 0..10; -1 before the first render */
    int32_t head;                                      /* the write head, 0..32767 (frames; it stops while held) */
    int32_t level;                                     /* the input level, 0..32767 (a fast peak follower) */
    int32_t onsets;                                    /* onsets detected (counts; the screen reacts to a change) */
    int32_t flags;                                     /* bit 0 hold, bit 1 reverse, bit 2 clearing (starting) */
    int32_t active;                                    /* bit i: voice i sounding */
    DisplayVoice v[petri::kVoices];
};

const int32_t kDispHold = 1, kDispReverse = 2, kDispClearing = 4;

struct Instance {
    petri::Engine eng;
    petri_hb::HalfRate half;
    int32_t clear;                                     /* the next word to zero; petri::kClearEnd = clean */
    int16_t dry[2][kBlock];                            /* the input as int16: the mix writes the output here */
    Display disp;                                      /* e9 */
};

}  // namespace coop_petri

#endif
