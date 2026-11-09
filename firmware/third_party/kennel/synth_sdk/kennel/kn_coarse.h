/*
 * Coarse engine knobs (plugin.json "coarse", op1r 44 / op1r-src addbd83): a synth knob listed there turns in 8 steps
 * over 0..32767 (a selector, a mode) instead of the stock ~50, on both engine pages. dispatcher.c knob_event_hook
 * calls this for the word the knob edits (params[k], or OP1_ENG_PAGE_WORD(k) while page 2 is on). UI thread only.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef KN_COARSE_H
#define KN_COARSE_H
#include <stdint.h>

#define KN_COARSE_DETENT 2048            /* a detent is 2 encoder events of 1024 (emulator, as op1r) */
#define KN_COARSE_STEP   4681            /* 32767 / 7: 8 positions 0, 4681, .. 32767 */

/* acc: this knob's event accumulator (starts over when the direction changes); word: the knob word.
 * -> 1 when a step was taken (the word is written, maybe unchanged at an end), 0 while between detents. */
static inline int kn_coarse_turn(int32_t *acc, int16_t *word, int delta)
{
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int32_t st = *acc / KN_COARSE_DETENT;
    if (!st) return 0;
    *acc -= st * KN_COARSE_DETENT;
    int32_t v = (*word < 0 ? 0 : *word) + st * KN_COARSE_STEP;
    *word = (int16_t)(v < 0 ? 0 : (v > 32767 ? 32767 : v));
    return 1;
}

/*
 * Stepped page 2 (OP1_PRESS_PAGE_STEPS(n) in an engine's press[], op1_plugin.h): the knob's page-2 word (a selector,
 * e.g. Amp's voice) turns in n steps, one per detent. Page 1 is not touched. Step i holds the word at the centre of its
 * band, ((2i+1) * 32768) / (2n), and the step of a word is (word * n) >> 15, so a word saved by a finer control
 * keeps the step it always meant. An unset word (<= 0) starts at 8192 like the plain page-2 turn.
 * Same detent and direction-change rule as kn_coarse_turn. -> 1 when a step was taken, 0 while between detents.
 */
static inline int kn_step_turn(int32_t *acc, int16_t *word, int delta, int n)
{
    if ((*acc > 0 && delta < 0) || (*acc < 0 && delta > 0)) *acc = 0;
    *acc += delta;
    int32_t st = *acc / KN_COARSE_DETENT;
    if (!st) return 0;
    *acc -= st * KN_COARSE_DETENT;
    int32_t i = (((*word > 0 ? *word : 8192) * n) >> 15) + st;
    i = i < 0 ? 0 : (i > n - 1 ? n - 1 : i);
    int32_t v = ((2 * i + 1) * 32768) / (2 * n);
    *word = (int16_t)(v > 32767 ? 32767 : v);
    return 1;
}
#endif
