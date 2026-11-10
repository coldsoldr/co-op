/* v1 test engine: tests/plugins/tone_common/tone.h, rendered at 88.2 kHz (flags 0, the reference for tone44).
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "tone.h"

extern "C" const coop_engine_t ENGINE_tone88 = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 4, 0, tone_note_on, tone_render88, 0, 0,
};
