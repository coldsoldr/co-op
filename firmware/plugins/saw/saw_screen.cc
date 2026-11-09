/*
 * saw's screen (E2, docs/engines.md "Screens"): every encoder moves its own part, in its own colour.
 * Blue brightness: the saw's height. Green detune: the second saw slides right of the first. White sub: the square an
 * octave down grows from its line. Orange drift: the dot wobbles, wider and faster as drift rises.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_screen.h"

static void *n_saw1, *n_saw2, *n_sub, *n_drift;
static int32_t t_ms;                                   /* the screen's clock, for the drift dot */

static void set_matrix(void *node, int32_t a, int32_t d, int32_t e, int32_t f)   /* matrix(a,0,0,d,e,f), Q10 */
{
    int32_t m[9] = { a, 0, 0, 0, d, 0, e, f, 0x400 };
    coop_scr_transform(node, m);
}

static void init(void *scr)
{
    n_saw1 = coop_scr_find(scr, "saw1");
    n_saw2 = coop_scr_find(scr, "saw2");
    n_sub = coop_scr_find(scr, "sub");
    n_drift = coop_scr_find(scr, "drift");
}

static void update(void *scr, int dt)
{
    int16_t k[4];
    coop_scr_knobs(k);
    int32_t bright = k[0] < 0 ? 0 : k[0], detune = k[1] < 0 ? 0 : k[1];
    int32_t sub = k[2] < 0 ? 0 : k[2], drift = k[3] < 0 ? 0 : k[3];
    /* blue: the saws scale about their baseline y 94, from 20 % to 100 % of the drawn height (Q10) */
    int32_t sy = 205 + ((bright * 819) >> 15);
    if (n_saw1) set_matrix(n_saw1, 0x400, sy, 0, 94 * (0x400 - sy));
    /* green: the second saw sits up to 24 units right (60 is a whole cycle) */
    if (n_saw2) set_matrix(n_saw2, 0x400, sy, (detune * 24) >> 5, 94 * (0x400 - sy));   /* 24 x 1024 / 32768 */
    /* white: the square scales about its line y 120, from flat to the drawn 16 units */
    int32_t qy = (sub * 1024) >> 15;
    if (n_sub) set_matrix(n_sub, 0x400, qy, 0, 120 * (0x400 - qy));
    /* orange: a triangle wobble, +-56 units at full drift, a period of 4 s at rest down to 1 s at full */
    t_ms += dt > 0 && dt < 200 ? dt : 16;
    int32_t period = 4000 - ((drift * 3000) >> 15);
    int32_t ph = (int32_t)((t_ms % period) * 65536 / period);               /* 0 .. 65535 */
    int32_t tri = ph < 32768 ? ph * 2 - 32768 : (65535 - ph) * 2 - 32768;    /* -32768 .. 32767 */
    if (n_drift) set_matrix(n_drift, 0x400, 0x400, ((tri >> 5) * ((drift * 56) >> 15)), 0);   /* x in Q10 */
    coop_scr_draw(scr, dt);
}

extern "C" const coop_screen_t SCREEN_saw = { init, update };
