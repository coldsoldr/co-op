/*
 * coop-fw u5: pitch bend amount and time per synth sound (docs/ui-features.md §2).
 *
 * Stock bends with Shift + left / right in synth mode: the shifted key-down writes a global int16 target
 * W[0xFF8016DC] = -32 (left) or +64 (right), the Core slews toward it with a fixed short glide (L1), and the release
 * puts the target back (research 2026-11-07, OP1_vdk.dis). The target is a linear frequency delta in 1/64:
 * +64 = x2 (an octave up), -32 = x0.5 (an octave down) [I: the asymmetry says so; a device test confirms the
 * semitones]. coop takes those keys first (coop_ui_key, before the stock dispatch) and writes its own target:
 * the sound's bend amount in semitones (-12..+12, default 12 = the stock range), as 64 * (2^(st/12) - 1); a
 * negative amount bends the other way. The bend time (OFF, 1..127, MAX -> 0..2 s, the stock portamento's scale)
 * is coop's own ramp toward the target, stepped every audio block (coop_bend_block from the stereo block entry);
 * OFF writes the target at once and the stock's own short slew stays. MIDI bend (0x01A84EFA) is untouched.
 *
 * On the shifted T2 page (screen 40, playmode.svg) stock shows play mode (blue) and portamento (green) and hides
 * two rows it drew for a "G-BEND" (white) and "DETUNE" (red); the shifted white and orange encoders there write
 * adsr words 6 and 7 that nothing reads. coop takes those two encoders (coop_ui_knob, before the stock step):
 * white = bend amount, orange = bend time, and draws the two rows in the stock font at the hidden rows' places.
 * Both values live in the sound's "coop" member (coop_sound.cc).
 *
 * Threads: keys and encoders on the KeyHandler thread, the ramp in the audio block, drawing on the Gfx thread.
 * Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_text.h"

#define CUR_MODE     (*(volatile int *)0xFF907D58u)
#define CUR_SCREEN   (*(volatile int *)0x018B65C8u)
#define SYNTH_PAGE   (*(volatile int *)0x01B4EE88u)
#define SYNTH_INST   (*(volatile int *)0xFF8016D4u)
#define SHIFT_HELD   (*(volatile uint8_t *)0x01DD7F78u)
#define BEND_TARGET  (*(volatile int16_t *)0xFF8016DCu)
#define HELD_LEFT    (*(volatile uint8_t *)0x018C2188u)
#define HELD_RIGHT   (*(volatile uint8_t *)0x018C2189u)
#define BEND_ECHO    (*(volatile uint8_t *)0xFF80104Eu)                   /* "send the bend to MIDI out" */
#define BEND_ST52    (*(volatile uint8_t *)0xFF801052u)                   /* Core state bytes, written as the stock release does */
#define BEND_ST53    (*(volatile uint8_t *)0xFF801053u)
#define SCREEN_PLAYMODE 40
#define KEY_LEFT  0x24
#define KEY_RIGHT 0x25

struct coop_sound { int16_t bend, bend_t; int8_t pan[24]; uint8_t has; };
extern "C" const struct coop_sound *coop_sound_get(int kind, int slot);     /* coop_sound.cc */
extern "C" void coop_sound_set_bend(int slot, int bend, int bend_t);

static const int8_t unit_of[25] = { -32, -30, -28, -26, -24, -21, -19, -16, -13, -10, -7, -4, 0, 4, 8, 12, 17, 21, 27, 32, 38, 44, 50, 57, 64 };   /* 64 * (2^(st/12) - 1), st -12..12 */
static const uint16_t ms_of[129] = { 0, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 33, 35, 36, 38, 40, 42, 44, 46, 48, 50, 53, 55, 58, 61, 64, 67, 70, 74, 77, 81, 85, 89, 93, 98, 102, 107, 113, 118, 124, 130, 136, 142, 149, 157, 164, 172, 180, 189, 198, 208, 218, 228, 239, 251, 263, 276, 289, 303, 318, 333, 349, 366, 384, 402, 422, 442, 463, 486, 509, 534, 560, 587, 615, 645, 676, 708, 743, 778, 816, 856, 897, 940, 986, 1033, 1083, 1135, 1190, 1248, 1308, 1371, 1438, 1507, 1580, 1656, 1736, 1820, 1908, 2000, 2000 };   /* 5 * 400^(i/127) */

volatile uint32_t g_bend_presses, g_bend_knobs, g_bend_ramps;              /* stats / tests */
static volatile int32_t s_goal;                                              /* the target in Q8 (units << 8) */
static volatile int32_t s_cur;                                               /* the ramp position, Q8 */
static volatile int32_t s_step;                                              /* per block, Q8; 0 = at once */
static int s_dir;                                                            /* -1 left, +1 right, 0 none */
static uint64_t s_swallow;

static int16_t target_for(int dir)
{
    if (!dir) return 0;
    const struct coop_sound *c = coop_sound_get(0, SYNTH_INST);
    int st = dir > 0 ? c->bend : -c->bend;
    return unit_of[st + 12];
}
static void set_goal(int dir)                                                /* KeyHandler thread */
{
    const struct coop_sound *c = coop_sound_get(0, SYNTH_INST);
    int16_t t = target_for(dir);
    int ms = ms_of[c->bend_t > 128 ? 128 : (c->bend_t < 0 ? 0 : c->bend_t)];
    if (ms == 0) { s_step = 0; s_cur = (int32_t)t << 8; s_goal = s_cur; BEND_TARGET = t; }
    else {
        int32_t from = s_cur, to = (int32_t)t << 8, d = to - from;
        if (d < 0) d = -d;
        int32_t blocks = (ms * 100) / 145;                                   /* 1.45 ms a block */
        if (blocks < 1) blocks = 1;
        s_step = d / blocks + 1;
        s_goal = to;
    }
    BEND_ECHO = 1;
}

/* the audio block: step the ramp toward the goal (stereo_bus.cc block entry) */
extern "C" void coop_bend_block(void)
{
    int32_t cur = s_cur, goal = s_goal, step = s_step;
    if (!step || cur == goal) return;
    if (cur < goal) { cur += step; if (cur > goal) cur = goal; } else { cur -= step; if (cur < goal) cur = goal; }
    s_cur = cur;
    BEND_TARGET = (int16_t)(cur >> 8);
    g_bend_ramps++;
}

/* from coop_ui_key: Shift + arrows on the synth, both edges taken */
extern "C" int coop_bend_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) {
        if (!(s_swallow & bit)) return 0;
        s_swallow &= ~bit;
        if (code == KEY_LEFT) HELD_LEFT = 0; else HELD_RIGHT = 0;
        s_dir = HELD_LEFT ? -1 : (HELD_RIGHT ? 1 : 0);
        set_goal(s_dir);
        BEND_ST52 = 0; BEND_ST53 = 1;                                        /* as the stock release */
        return 1;
    }
    if (code != KEY_LEFT && code != KEY_RIGHT) return 0;
    if (CUR_MODE != 0 || !SHIFT_HELD) return 0;
    if (code == KEY_LEFT) { HELD_LEFT = 1; s_dir = -1; } else { HELD_RIGHT = 1; s_dir = 1; }
    set_goal(s_dir);
    s_swallow |= bit;
    g_bend_presses++;
    return 1;
}

/* from coop_ui_knob: the shifted white / orange on T2 (knob 2 / 3 with shift). 1 = taken */
extern "C" int coop_bend_knob(int knob, int steps)
{
    if (CUR_MODE != 0 || SYNTH_PAGE != 1 || !SHIFT_HELD || (knob != 2 && knob != 3)) return 0;
    int slot = SYNTH_INST;
    const struct coop_sound *c = coop_sound_get(0, slot);
    int bend = c->bend, t = c->bend_t;
    if (knob == 2) bend += steps; else t += steps;
    coop_sound_set_bend(slot, bend, t);
    g_bend_knobs++;
    return 1;
}

/* drawing (Gfx thread): the hidden rows of playmode.svg, white "bend" and orange "time" */
static const uint8_t c_white[4] = { 0xDF, 0xD9, 0xFF, 255 }, c_orange[4] = { 0xFF, 0x3A, 0x5D, 255 };
static struct coop_text s_tx[4];
static void num_s(char *out, int v, int sign)
{
    int i = 0;
    if (v < 0) { out[i++] = '-'; v = -v; } else if (sign && v > 0) out[i++] = '+';
    if (v >= 100) out[i++] = (char)('0' + v / 100);
    if (v >= 10) out[i++] = (char)('0' + (v / 10) % 10);
    out[i++] = (char)('0' + v % 10);
    out[i] = 0;
}
extern "C" void coop_bend_draw(void *r)
{
    if (CUR_MODE != 0 || CUR_SCREEN != SCREEN_PLAYMODE) return;
    const struct coop_sound *c = coop_sound_get(0, SYNTH_INST);
    char b[8];
    coop_text_draw(&s_tx[0], r, 55, 82, 20, c_white, "bend", 0);
    num_s(b, c->bend, 1);
    coop_text_draw(&s_tx[1], r, 214, 82, 20, c_white, b, 0);
    coop_text_draw(&s_tx[2], r, 55, 115, 20, c_orange, "time", 0);
    if (c->bend_t <= 0) { b[0] = 'o'; b[1] = 'f'; b[2] = 'f'; b[3] = 0; }
    else if (c->bend_t >= 128) { b[0] = 'm'; b[1] = 'a'; b[2] = 'x'; b[3] = 0; }
    else num_s(b, c->bend_t, 0);
    coop_text_draw(&s_tx[3], r, 214, 115, 20, c_orange, b, 0);
}
