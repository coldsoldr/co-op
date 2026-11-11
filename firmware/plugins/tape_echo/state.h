/* tape_echo: the state in the slot's position buffer (sdk/coop_plugin.h e8), shared by the render (tape_echo.cc) and the
 * screen (tape_echo_screen.cc, e9: it reads only `disp`, through coop_fx_view_t.state while the view is live).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_TAPE_ECHO_STATE_H_
#define COOP_TAPE_ECHO_STATE_H_
#include <stdint.h>
#include "src/half_rate.h"
#include "src/tape_echo.h"

namespace coop_tape_echo {

const int32_t kBlock = op1clouds::kFxBlockSize;        /* 64 */
const int32_t kHalfN = op1clouds::kHalfN;              /* 32 */

/* What the render publishes for the screen (sdk/coop_plugin.h "Display data"): whole words, written once a block (the
 * clearing blocks too), after the DSP; the DSP never reads them. */
struct Display {
    int32_t time;                                      /* the time word the echo gets (0..32767): the turn's, or the sync
                                                        * division's; -1 before the first render */
    int32_t div;                                       /* the division in force, 0..10 (1/16T .. 1/2, sync step - 1);
                                                        * -1 when free (sync off, or the division fell back to the turn) */
    int32_t beat;                                      /* a 96th note at the tempo in use, Q4 samples at 22050 Hz */
};

struct Instance {
    op1fx::TapeEcho echo;
    op1clouds::HalfRate half;
    int32_t clear;                                     /* the next tape pair to zero; TE_TAPE_PAIRS = clean */
    int16_t dry[2][kBlock];                            /* the input as int16: the mix writes the output here */
    int16_t gained[2][kHalfN];                         /* the decimated input after the gain */
    Display disp;                                      /* e9 */
};

}  // namespace coop_tape_echo

#endif
