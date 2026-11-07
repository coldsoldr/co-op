/*
 * coop-fw u5 / u7: pitch bend amount and time per synth sound (docs/ui-features.md §2).
 *
 * What the device showed (u5, 2026-11-07): the word stock's Shift + arrows write (W[0xFF8016DC] = -32 / +64) is a
 * RATE, not a target: the Core glides toward a fixed octave at that speed, so a small value bent an octave slowly and
 * the time had no effect. The live bend the Core applies is W[0xFF80104C], an int16 the MIDI pitch-bend handler
 * (0x01A84EFA) writes directly (down: (v - 0x2000) << 1 = -16384 .. 0, up: (v - 0x2000) << 2 = 0 .. 32764, with
 * the same 1:2 asymmetry as the keyboard word: a linear frequency delta, full = an octave each way [I]). u7 drives
 * that word itself: Shift + left / right (taken before the stock, so the keyboard word stays 0) ramps the live bend
 * every audio block to 32764 * (2^(st/12) - 1) up or -32768 * (1 - 2^(-st/12)) down (a linear frequency delta, as the
 * emulator's pitch track showed) over the bend time, and back to 0 on
 * the release; the state bytes the MIDI handler writes (0xFF80104E = 0, 0xFF801052 = 0, 0xFF801053 = 1) are written
 * the same way. Amount 0..12 semitones (the owner: the same both ways; default 12 = the stock bend), time OFF /
 * 1..127 / MAX (5 ms .. 2 s on the stock portamento's scale; OFF = at once).
 *
 * The shifted T2 page (screen 40, playmode.svg) hides two rows the stock drew; the shifted white and orange
 * encoders there write adsr words 6 and 7 that nothing reads. coop takes them (coop_ui_knob, before the stock step):
 * white = bend amount, orange = bend time, drawn in the stock font at the hidden rows' places. Both values live in
 * the sound's "coop" member (coop_sound.cc).
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
#define BEND_LIVE    (*(volatile int16_t *)0xFF80104Cu)                   /* the bend the Core applies (MIDI writes it) */
#define BEND_ECHO    (*(volatile uint8_t *)0xFF80104Eu)                   /* "send the bend to MIDI out": MIDI in clears it */
#define BEND_ST52    (*(volatile uint8_t *)0xFF801052u)
#define BEND_ST53    (*(volatile uint8_t *)0xFF801053u)
#define SCREEN_PLAYMODE 40
#define KEY_LEFT  0x24
#define KEY_RIGHT 0x25

struct coop_sound { int16_t bend, bend_t; int8_t pan[24]; uint8_t has; };
extern "C" const struct coop_sound *coop_sound_get(int kind, int slot);     /* coop_sound.cc */
extern "C" void coop_sound_set_bend(int slot, int bend, int bend_t);

static const uint16_t ms_of[129] = { 0, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31, 33, 35, 36, 38, 40, 42, 44, 46, 48, 50, 53, 55, 58, 61, 64, 67, 70, 74, 77, 81, 85, 89, 93, 98, 102, 107, 113, 118, 124, 130, 136, 142, 149, 157, 164, 172, 180, 189, 198, 208, 218, 228, 239, 251, 263, 276, 289, 303, 318, 333, 349, 366, 384, 402, 422, 442, 463, 486, 509, 534, 560, 587, 615, 645, 676, 708, 743, 778, 816, 856, 897, 940, 986, 1033, 1083, 1135, 1190, 1248, 1308, 1371, 1438, 1507, 1580, 1656, 1736, 1820, 1908, 2000, 2000 };   /* 5 * 400^(i/127) */

volatile uint32_t g_bend_presses, g_bend_knobs, g_bend_ramps;              /* stats / tests */
static volatile int32_t s_goal, s_cur, s_step;                              /* the live value and its goal, Q8; the step per block */
static volatile int s_active;                                               /* coop owns the live word (a key is or was held) */
static int s_left, s_right;
static uint64_t s_swallow;

static const uint16_t up_of[13] = { 0, 1948, 4012, 6199, 8516, 10971, 13571, 16327, 19246, 22338, 25615, 29086, 32764 };     /* 32764 * (2^(st/12) - 1) */
static const uint16_t down_of[13] = { 0, 1839, 3575, 5214, 6760, 8220, 9598, 10898, 12125, 13284, 14378, 15410, 16384 };   /* 32768 * (1 - 2^(-st/12)) */
static int32_t goal_for(int dir)                                            /* the live word is a linear frequency delta:
                                                                               ratio = 1 + v / 32764 up, 1 - v / 32768 down
                                                                               (op1emu, 2026-11-07: amount 2 as 5460 gave x1.169) */
{
    if (!dir) return 0;
    int a = coop_sound_get(0, SYNTH_INST)->bend;
    if (a < 0) a = 0;
    if (a > 12) a = 12;
    return dir > 0 ? (int32_t)up_of[a] : -(int32_t)down_of[a];
}
static void set_goal(int dir)                                               /* KeyHandler thread */
{
    const struct coop_sound *c = coop_sound_get(0, SYNTH_INST);
    int32_t to = goal_for(dir) << 8;
    int t = c->bend_t > 128 ? 128 : (c->bend_t < 0 ? 0 : c->bend_t);
    int ms = ms_of[t];
    s_active = 1;
    if (ms == 0) { s_step = 0; s_cur = to; s_goal = to; BEND_LIVE = (int16_t)(to >> 8); BEND_ECHO = 0; BEND_ST52 = 0; BEND_ST53 = 1; }
    else {
        int32_t d = to - s_cur;
        if (d < 0) d = -d;
        int32_t blocks = (ms * 100) / 145;                                   /* 1.45 ms a block */
        if (blocks < 1) blocks = 1;
        s_step = d / blocks + 1;
        s_goal = to;
    }
}

/* the audio block: step the live bend toward the goal (stereo_bus.cc block entry) */
extern "C" void coop_bend_block(void)
{
    if (!s_active) return;
    int32_t cur = s_cur, goal = s_goal, step = s_step;
    if (!step || cur == goal) { if (goal == 0 && cur == 0) s_active = 0; return; }   /* back at rest: hand the word back */
    if (cur < goal) { cur += step; if (cur > goal) cur = goal; } else { cur -= step; if (cur < goal) cur = goal; }
    s_cur = cur;
    BEND_LIVE = (int16_t)(cur >> 8);
    BEND_ECHO = 0; BEND_ST52 = 0; BEND_ST53 = 1;
    g_bend_ramps++;
}

/* from coop_ui_key: Shift + arrows on the synth, both edges taken */
extern "C" int coop_bend_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) {
        if (!(s_swallow & bit)) return 0;
        s_swallow &= ~bit;
        if (code == KEY_LEFT) s_left = 0; else s_right = 0;
        set_goal(s_left ? -1 : (s_right ? 1 : 0));
        return 1;
    }
    if (code != KEY_LEFT && code != KEY_RIGHT) return 0;
    if (CUR_MODE != 0 || !SHIFT_HELD) return 0;
    if (code == KEY_LEFT) s_left = 1; else s_right = 1;
    set_goal(s_left && !s_right ? -1 : 1);
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
    if (bend < 0) bend = 0;
    if (bend > 12) bend = 12;
    coop_sound_set_bend(slot, bend, t);
    g_bend_knobs++;
    return 1;
}

/* drawing (Gfx thread): the hidden rows of playmode.svg, white "bend" and orange "time" */
static const uint8_t c_white[4] = { 0xDF, 0xD9, 0xFF, 255 }, c_orange[4] = { 0xFF, 0x3A, 0x5D, 255 };
static struct coop_text s_tx[4];
static void num_s(char *out, int v)
{
    int i = 0;
    if (v < 0) { out[i++] = '-'; v = -v; }
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
    num_s(b, c->bend < 0 ? 0 : c->bend);
    coop_text_draw(&s_tx[1], r, 214, 82, 20, c_white, b, 0);
    coop_text_draw(&s_tx[2], r, 55, 115, 20, c_orange, "time", 0);
    if (c->bend_t <= 0) { b[0] = 'o'; b[1] = 'f'; b[2] = 'f'; b[3] = 0; }
    else if (c->bend_t >= 128) { b[0] = 'm'; b[1] = 'a'; b[2] = 'x'; b[3] = 0; }
    else num_s(b, c->bend_t);
    coop_text_draw(&s_tx[3], r, 214, 115, 20, c_orange, b, 0);
}
