// The host / GNU simulator stand-in for the core's screen calls (sdk/coop_screen.h, core/sdram/engine_screen.cc), linked
// into the host test of every effect port with a screen (e9): the port's screen file compiles and links with the
// render it shares state with, and the calls do nothing. The tests compare audio only; the screen is gated in op1emu.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include "coop_screen.h"

extern "C" {
void *coop_scr_find(void *, const char *) { return 0; }
void coop_scr_transform(void *, const int32_t *) {}
void coop_scr_visible(void *, int) {}
uint8_t *coop_scr_line_rgba(void *, const char *) { return 0; }
uint8_t *coop_scr_fill_rgba(void *, const char *) { return 0; }
void coop_scr_draw(void *, int) {}
void *coop_scr_raster(void) { return 0; }
void coop_scr_line(void *, const int32_t *, const int32_t *, int32_t, const uint8_t *) {}
void coop_scr_knobs(int16_t *out4) { for (int i = 0; i < 4; i++) out4[i] = 0; }
void coop_scr_words(int16_t *out8) { for (int i = 0; i < 8; i++) out8[i] = 0; }
int coop_scr_mods(void) { return 0; }
}
