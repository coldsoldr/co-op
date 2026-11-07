/*
 * coop-fw u6: per-voice pan for the drum (docs/ui-features.md §3; the owner, 2026-11-07: on the drum's shifted T2
 * page, since the stock takes all four shifted encoders on the drum's T1 page).
 *
 * Each of the 24 drum keys keeps a pan (-50..50, L50 .. C .. R50) in the kit's sound ("coop":"pan", coop_sound.cc).
 * On the drum's T2 page with Shift held, the **green** encoder pans the last played key ([0xFF801044], the key index
 * 0..23 the drum note-on writes): stock's shifted blue there is the dynamic envelope's fifth word, green / white /
 * orange write nothing. A readout "pan <key> <L12>" is drawn in green while Shift is held. When a drum voice starts,
 * coop's stereo stage takes the pan of the key that started it instead of a random spread draw (stereo_bus.cc:
 * spread is the synth's only). Saved with the kit through the stock setter (the patch marked dirty).
 *
 * KeyHandler thread for the encoder, Gfx thread for the readout, the audio block for the lookup (plain memory, no
 * allocation). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_text.h"

#define CUR_MODE   (*(volatile int *)0xFF907D58u)
#define DRUM_PAGE  (*(volatile int *)0x01AAB9F4u)
#define DRUM_KIT   (*(volatile int *)0xFF8016D8u)
#define LAST_KEY   (*(volatile int *)0xFF801044u)                          /* 0..23: the last drum key pressed */
#define SHIFT_HELD (*(volatile uint8_t *)0x01DD7F78u)

struct coop_sound { int16_t bend, bend_t; int8_t pan[24]; uint8_t has; };
extern "C" const struct coop_sound *coop_sound_get(int kind, int slot);     /* coop_sound.cc */
extern "C" void coop_sound_set_pan(int kit, int voice, int pan);

volatile uint32_t g_drumpan_knobs;                                          /* stats / tests */

static int key_now(void) { int k = LAST_KEY; return k < 0 ? 0 : (k > 23 ? 23 : k); }
static int kit_now(void) { int k = DRUM_KIT; return k < 0 ? 0 : (k > 7 ? 7 : k); }

/* the audio block: the pan of the key that started a drum voice, Q15 (-32750 .. 32750) */
extern "C" int16_t coop_drumpan_q15(void)
{
    return (int16_t)(coop_sound_get(1, kit_now())->pan[key_now()] * 655);
}

/* from coop_ui_knob: Shift + green on the drum's T2 page. 1 = taken */
extern "C" int coop_drumpan_knob(int knob, int steps)
{
    if (CUR_MODE != 1 || DRUM_PAGE != 1 || !SHIFT_HELD || knob != 1) return 0;
    int kit = kit_now(), key = key_now();
    coop_sound_set_pan(kit, key, coop_sound_get(1, kit)->pan[key] + steps);
    g_drumpan_knobs++;
    return 1;
}

/* drawing (Gfx thread): the readout while Shift is held on the drum's T2 page */
static const uint8_t c_green[4] = { 0x00, 0xED, 0x95, 255 };
static struct coop_text s_tx[2];
extern "C" void coop_drumpan_draw(void *r)
{
    if (CUR_MODE != 1 || DRUM_PAGE != 1 || !SHIFT_HELD) return;
    int key = key_now(), pan = coop_sound_get(1, kit_now())->pan[key];
    char a[12], b[8];
    int i = 0;
    const char *q = "pan ";
    for (; *q; q++) a[i++] = *q;
    if (key + 1 >= 10) a[i++] = (char)('0' + (key + 1) / 10);
    a[i++] = (char)('0' + (key + 1) % 10);
    a[i] = 0;
    i = 0;
    if (pan == 0) b[i++] = 'c';
    else { int v = pan < 0 ? -pan : pan; b[i++] = pan < 0 ? 'l' : 'r'; if (v >= 10) b[i++] = (char)('0' + v / 10); b[i++] = (char)('0' + v % 10); }
    b[i] = 0;
    coop_text_draw(&s_tx[0], r, 200, 118, 12, c_green, a, 0);
    coop_text_draw(&s_tx[1], r, 260, 118, 12, c_green, b, 0);
}
