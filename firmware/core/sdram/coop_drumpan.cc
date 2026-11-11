/*
 * coop-fw u6 / u7: per-voice pan for the drum (docs/ui-features.md §3; the owner, 2026-11-07: on the drum's T2 page
 * with Shift held, its own page like the synth's play-mode page: "PAN" in blue with the value on the right).
 *
 * Each of the 24 drum keys keeps a pan (-50..50, L50 .. C .. R50) in the kit's sound ("coop":"pan", coop_sound.cc).
 * While Shift is held on the drum's T2 page coop draws the PAN page over the stock's (black, "pan" blue, the value
 * blue at the right: the shifted blue encoder pans the last played key ([0xFF801044], the key index 0..23 the drum
 * note-on writes); the stock's shifted blue there wrote the dynamic envelope's fifth word, which this takes over).
 * When a drum voice starts, coop's stereo stage takes the pan of the key that started it instead of a random spread
 * draw (stereo_bus.cc: spread is the synth's only). Saved with the kit through the stock setter (the patch marked
 * dirty).
 *
 * KeyHandler thread for the encoder, Gfx thread for the page, the audio block for the lookup (plain memory, no
 * allocation). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define CUR_MODE   (*(volatile int *)0xFF907D58u)
#define DRUM_PAGE  (*(volatile int *)0x01AAB9F4u)
#define DRUM_KIT   (*(volatile int *)0xFF8016D8u)
#define LAST_KEY   (*(volatile int *)0xFF801044u)                          /* 0..23: the last drum key pressed */
#define SHIFT_HELD (*(volatile uint8_t *)0x01DD7F78u)

typedef struct { int32_t x, y; } op1_pt;
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

struct coop_sound { int16_t bend, bend_t; int8_t pan[24]; uint8_t has; };
extern "C" const struct coop_sound *coop_sound_get(int kind, int slot);     /* coop_sound.cc */
extern "C" void coop_sound_set_pan(int kit, int voice, int pan);

volatile uint32_t g_drumpan_knobs;                                          /* stats / tests */

static int key_now(void) { int k = LAST_KEY; return k < 0 ? 0 : (k > 23 ? 23 : k); }
static int kit_now(void) { int k = DRUM_KIT; return k < 0 ? 0 : (k > 7 ? 7 : k); }
static int page_up(void) { return CUR_MODE == 1 && DRUM_PAGE == 1 && SHIFT_HELD; }

/* the audio block: the pan of the key that started a drum voice, Q15 (-32750 .. 32750) */
extern "C" int16_t coop_drumpan_q15(void)
{
    return (int16_t)(coop_sound_get(1, kit_now())->pan[key_now()] * 655);
}

/* from coop_ui_knob: Shift + blue on the drum's T2 page. 1 = taken */
extern "C" int coop_drumpan_knob(int knob, int steps)
{
    if (!page_up() || knob != 0) return 0;
    int kit = kit_now(), key = key_now();
    coop_sound_set_pan(kit, key, coop_sound_get(1, kit)->pan[key] + steps);
    g_drumpan_knobs++;
    return 1;
}

/* drawing (Gfx thread): the PAN page while Shift is held on the drum's T2 */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_blue[4] = { 0x69, 0x8E, 0xFF, 255 }, c_dim[4] = { 0x62, 0x5D, 0x77, 255 };
static struct coop_text s_tx[3];
static void fill(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    int32_t ym = (y0 + y1) << 9, w = (y1 - y0) << 10;
    sf_line(r, x0 << 10, ym, x1 << 10, ym, w, c);
}
extern "C" void coop_drumpan_draw(void *r)
{
    if (!page_up()) return;
    for (int32_t y = -4; y < 164; y += 4) fill(r, -4, y, 324, y + 8, c_bg);
    int key = key_now(), pan = coop_sound_get(1, kit_now())->pan[key];
    char v[8], k[8];
    int i = 0;
    if (pan == 0) v[i++] = 'c';
    else { int a = pan < 0 ? -pan : pan; v[i++] = pan < 0 ? 'l' : 'r'; if (a >= 10) v[i++] = (char)('0' + a / 10); v[i++] = (char)('0' + a % 10); }
    v[i] = 0;
    i = 0;
    const char *q = "key ";
    for (; *q; q++) k[i++] = *q;
    if (key + 1 >= 10) k[i++] = (char)('0' + (key + 1) / 10);
    k[i++] = (char)('0' + (key + 1) % 10);
    k[i] = 0;
    coop_text_draw(&s_tx[0], r, 55, 17, 20, c_blue, "pan", 0);
    coop_text_draw(&s_tx[1], r, 214, 17, 20, c_blue, v, 0);
    coop_text_draw(&s_tx[2], r, 55, 50, 12, c_dim, k, 0);
}
