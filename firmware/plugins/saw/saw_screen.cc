/*
 * saw's screen (E2, e4; docs/engines.md "Screens", "Controls"): every encoder moves its own part, in its own colour.
 * Blue brightness: the oscillators' height. Green detune: the second oscillator slides right of the first. White sub:
 * the square an octave or two down grows from its line. Orange drift: the dot wobbles, wider and faster as drift rises.
 * The declared controls (e4): the shape icons top right (fn + blue: the shown shape bright blue; while fn is held the
 * others turn a dim blue, so the layer shows) and the shape of both oscillators; the sub's octave as one or two
 * orange dots bottom right (bright while shift + fn is held: shift + fn + orange clicks it). The core draws the
 * control's name and value when it changes; this screen draws the state.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_screen.h"

static void *n_saw1, *n_saw2, *n_sub, *n_drift, *n_shape1[3], *n_shape2[3], *n_oct2;
static uint8_t *c_icon[3], *c_oct[2];
static int32_t t_ms;                                   /* the screen's clock, for the drift dot */

static const uint8_t BLUE[4] = { 0x69, 0x8E, 0xFF, 255 }, BLUE_DIM[4] = { 0x34, 0x47, 0x7F, 255 }, DIM[4] = { 0x38, 0x35, 0x4D, 255 };
static const uint8_t ORANGE[4] = { 0xFF, 0x3A, 0x5D, 255 }, ORANGE_DIM[4] = { 0x6A, 0x20, 0x30, 255 };

static void set_matrix(void *node, int32_t a, int32_t d, int32_t e, int32_t f)   /* matrix(a,0,0,d,e,f), Q10 */
{
    int32_t m[9] = { a, 0, 0, 0, d, 0, e, f, 0x400 };
    coop_scr_transform(node, m);
}
static void paint(uint8_t *style, const uint8_t *c) { if (style) for (int i = 0; i < 4; i++) style[i] = c[i]; }

static void init(void *scr)
{
    static const char *const s1[3] = { "saw1s0", "saw1s1", "saw1s2" }, *const s2[3] = { "saw2s0", "saw2s1", "saw2s2" };
    static const char *const ic[3] = { "ic0", "ic1", "ic2" };
    n_saw1 = coop_scr_find(scr, "saw1");
    n_saw2 = coop_scr_find(scr, "saw2");
    n_sub = coop_scr_find(scr, "sub");
    n_drift = coop_scr_find(scr, "drift");
    n_oct2 = coop_scr_find(scr, "oct2g");
    for (int i = 0; i < 3; i++) {
        n_shape1[i] = coop_scr_find(scr, s1[i]);
        n_shape2[i] = coop_scr_find(scr, s2[i]);
        c_icon[i] = coop_scr_line_rgba(scr, ic[i]);
    }
    c_oct[0] = coop_scr_fill_rgba(scr, "oct1");
    c_oct[1] = coop_scr_fill_rgba(scr, "oct2");
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void update(void *scr, int dt)
{
    int16_t w[8];
    coop_scr_words(w);
    int mods = coop_scr_mods();
    int32_t bright = w[0] < 0 ? 0 : w[0], detune = w[1] < 0 ? 0 : w[1];
    int32_t sub = w[2] < 0 ? 0 : w[2], drift = w[3] < 0 ? 0 : w[3];
    int shape = clampi(w[5], 0, 2), oct = clampi(w[7], 0, 1);
    /* blue: the oscillators scale about their baseline y 94, from 20 % to 100 % of the drawn height (Q10) */
    int32_t sy = 205 + ((bright * 819) >> 15);
    if (n_saw1) set_matrix(n_saw1, 0x400, sy, 0, 94 * (0x400 - sy));
    /* green: the second oscillator sits up to 24 units right (60 is a whole cycle) */
    if (n_saw2) set_matrix(n_saw2, 0x400, sy, (detune * 24) >> 5, 94 * (0x400 - sy));   /* 24 x 1024 / 32768 */
    for (int i = 0; i < 3; i++) {
        if (n_shape1[i]) coop_scr_visible(n_shape1[i], i == shape);
        if (n_shape2[i]) coop_scr_visible(n_shape2[i], i == shape);
        paint(c_icon[i], i == shape ? BLUE : (mods & 2) && !(mods & 1) ? BLUE_DIM : DIM);   /* fn alone: the shape layer */
    }
    /* white: the square scales about its line y 120, from flat to the drawn 16 units */
    int32_t qy = (sub * 1024) >> 15;
    if (n_sub) set_matrix(n_sub, 0x400, qy, 0, 120 * (0x400 - qy));
    /* orange: the sub's octave as dots, bright while shift + fn (its layer) is held */
    if (n_oct2) coop_scr_visible(n_oct2, oct == 1);
    paint(c_oct[0], (mods & 3) == 3 ? ORANGE : ORANGE_DIM);
    paint(c_oct[1], (mods & 3) == 3 ? ORANGE : ORANGE_DIM);
    /* orange: a triangle wobble, +-56 units at full drift, a period of 4 s at rest down to 1 s at full */
    t_ms += dt > 0 && dt < 200 ? dt : 16;
    int32_t period = 4000 - ((drift * 3000) >> 15);
    int32_t ph = (int32_t)((t_ms % period) * 65536 / period);               /* 0 .. 65535 */
    int32_t tri = ph < 32768 ? ph * 2 - 32768 : (65535 - ph) * 2 - 32768;    /* -32768 .. 32767 */
    if (n_drift) set_matrix(n_drift, 0x400, 0x400, ((tri >> 5) * ((drift * 56) >> 15)), 0);   /* x in Q10 */
    coop_scr_draw(scr, dt);
}

extern "C" const coop_screen_t SCREEN_saw = { init, update };
