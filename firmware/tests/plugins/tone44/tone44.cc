/* v1 test engine: tests/plugins/tone_common/tone.h, rendered at 44.1 kHz (COOP_ENGINE_F_44K).
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "tone.h"

extern "C" const coop_engine_t ENGINE_tone44 = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, COOP_ENGINE_F_44K, 4, 0, tone_note_on, tone_render44, 0, 0,
};
