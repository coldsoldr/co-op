/* Sound-change services for native features. Main-loop patch/fade calls, read-only registry access.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission. */
#ifndef OP1_FEATURE_SOUND_H
#define OP1_FEATURE_SOUND_H
#include "op1_feature.h"
/* Main loop: out=1 requests fade, returns 1 until audio confirms silence; out=0 releases it.
 * Pair every request with release, including cancelled work. No blocking and no audio-thread patch writes. */
int op1_sound_fade(int out);
void op1_sound_apply(int *patch);
int op1_sound_editor_open(void);
const op1_engine_t *op1_sound_engine(int order);
const op1_fx_t *op1_sound_fx(int order);
int op1_sound_fx_ready(const char *name);
int op1_sound_lfo_ready(const char *name);
#endif
