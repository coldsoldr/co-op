/*
 * saw: the reference synth engine (E1, docs/engines.md). Two saws a little apart, a square an octave down, a one-pole
 * low-pass; the smallest engine that uses every part of the engine ABI: the pitch words, velocity, the knobs, the
 * host's per-voice state and note off. Its job is the host's gate (pitch, six voices, select / save / reload), not
 * to be a great synth; the saws are naive (they alias at the top of the keyboard, the low-pass takes the edge off).
 *
 * Knobs (0..32767): blue brightness (the low-pass), green detune (the second saw, up to ~25 cents), white sub (the
 * square's level), orange drift (a slow wobble of the detune, a different speed on each voice).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"

struct voice {
    uint32_t ph1, ph2, ph3;                            /* the saws and the sub's phases */
    int32_t lp;                                        /* the low-pass state (Q8 sample) */
    uint32_t drift;                                    /* the drift LFO's phase */
    int32_t gain;                                      /* velocity, Q15 */
    int32_t released;                                  /* note off seen (the stock envelope does the release) */
};

static inline int16_t sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : (int16_t)x); }

static void note_on(int voice, void *state, int32_t velocity, const int16_t *params)
{
    (void)params;
    struct voice *v = (struct voice *)state;
    v->ph1 = 0;
    v->ph2 = 0x40000000u;                              /* a quarter cycle apart: the two saws don't cancel at the start */
    v->ph3 = 0;
    v->lp = 0;
    v->drift = (uint32_t)voice * 0x2AAAAAAAu;          /* each voice wobbles from its own place */
    v->gain = velocity;
    v->released = 0;
}

static void note_off(int voice, void *state)
{
    (void)voice;
    ((struct voice *)state)->released = 1;
}

static void render(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    struct voice *v = (struct voice *)state;
    int32_t bright = params[0] < 0 ? 0 : params[0];
    int32_t detune = params[1] < 0 ? 0 : params[1];
    int32_t sub = params[2] < 0 ? 0 : params[2];
    int32_t drift = params[3] < 0 ? 0 : params[3];

    /* the low-pass coefficient: squared so the knob's low half is the dark half (Q12, 1/128 .. 1) */
    int32_t a = (((bright * bright) >> 15) + 256) >> 3;
    if (a > 4095) a = 4095;
    /* drift: a triangle from the voice's own phase, each voice a little faster (Q15, -1 .. 1) */
    v->drift += 0x00080000u + (uint32_t)voice * 0x00011000u;
    int32_t tri = (int32_t)(v->drift >> 16);           /* 0 .. 65535 */
    tri = tri < 32768 ? tri * 2 - 32768 : (65535 - tri) * 2 - 32768;
    int32_t det = detune + ((drift * tri) >> 16);      /* the drift moves the detune by up to half its range */
    if (det < 0) det = 0;
    if (det > 32767) det = 32767;
    int32_t g = (v->gain * 3) >> 3;                    /* headroom: two saws and a square, about -6 dB at full velocity */
    int32_t lp = v->lp;                                /* Q8 */
    uint32_t ph1 = v->ph1, ph2 = v->ph2, ph3 = v->ph3;
    /* 32-bit arithmetic only: a 64-bit multiply is a libgcc call per sample on the Blackfin */
    for (int i = 0; i < COOP_VOICE_BLOCK; i++) {
        uint32_t inc = (uint32_t)pitch[i];
        ph1 += inc;
        ph2 += inc + (((inc >> 16) * (uint32_t)det) >> 5);   /* up to inc / 64: about 27 cents */
        ph3 += inc >> 1;
        int32_t x = ((int32_t)ph1 >> 17) + ((int32_t)ph2 >> 17);                /* two saws, +-16384 each */
        x += ((int32_t)(ph3 & 0x80000000u ? -16384 : 16384) * sub) >> 15;      /* the sub square */
        lp += ((x - (lp >> 8)) * a) >> 4;                                       /* one pole, Q12 coefficient, Q8 state */
        out[i] = sat16((sat16(lp >> 8) * g) >> 15);
    }
    v->ph1 = ph1; v->ph2 = ph2; v->ph3 = ph3; v->lp = lp;
}

extern "C" const coop_screen_t SCREEN_saw;            /* saw_screen.cc */
extern "C" const coop_engine_t ENGINE_saw = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, sizeof(struct voice), 0, note_on, render, note_off, &SCREEN_saw,
};
