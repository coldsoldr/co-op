/*
 * kennel feature API, hook point "LFO types" (docs/feature-hooks.md): an extra LFO type that runs inside one of
 * R246's stock LFO classes. The feature's op1_feature_t.lfo points at one op1_lfo_type.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 *
 * How it runs: the type has an `lfo_types` row (name + 8 default words, added by the build) and runs in a stock
 * class (v1: only the MIDI LFO, class 4). Its 8 parameter words are saved with the sound under the stock class, so
 * the words themselves must say whose they are: word 7's high byte is the type's `marker` (0x80..0xFF, never a
 * stock MIDI LFO word). The core owns every R246 site (name compares, read_lfo_param, the patch range check, the
 * LfoMidi vtables, ScreenId::for_lfo) and calls the type only for words its `match` claims; all other words keep
 * the stock MIDI LFO code.
 *
 * Without the feature (file missing or broken), a sound saved with its words must still load: the core accepts
 * quad's name and words on its own (kennel/kn_lfo.c, reserved). A new type adds its name and marker there too when
 * it is released, so its sounds survive a missing file in the same way.
 *
 * Threads: apply_* and trig_* run on the audio thread (integers only, no division per sample); match on the audio,
 * GUI and patch-loading threads; press / turn on the KeyHandler thread; midi_* in midi_poll.
 */
#ifndef OP1_FEATURE_LFO_H
#define OP1_FEATURE_LFO_H
#include "op1_feature.h"

#define OP1_LFO_CLASS_MIDI 4                     /* stock LFO class a type runs in (lfo_types / LfoMidi) */

typedef struct op1_lfo_type {
    const char *name;                            /* lfo_types row name, saved in sounds: never changes meaning */
    uint8_t stock_class;                         /* OP1_LFO_CLASS_MIDI (the only one in v1) */
    uint8_t marker;                              /* high byte of word 7 in this type's words, 0x80..0xFF */
    uint8_t pad[2];
    int (*match)(const int16_t *w);              /* 1: these 8 words are this type's */
    /* the stock class's apply entries: return 1 if something was modulated. pass: 0 synth knobs, 1 synth
     * envelope, 2 synth FX, 3 drum knobs, 4 drum envelope, 5 drum FX. self = R246's LFO object (per voice). */
    int (*apply_synth)(void *self, const int16_t *w, int pass);
    int (*apply_drum)(void *self, const int16_t *w, int pass);
    int (*apply_fx)(void *self, const int16_t *w, int pass);
    /* note-on (synth voice / drum / FX LFO object), may be 0 */
    void (*trig_synth)(void *self, const int16_t *w);
    void (*trig_drum)(void *self, const int16_t *w);
    void (*trig_fx)(void *self, const int16_t *w);
    /* LFO page: shown instead of the stock class's screen while the page's words match. svg: a file in
     * content/display; may be 0 (no own screen, no controls) */
    const char *svg;
    const op1_screen_t *screen;
    void (*press)(int pos, int knob);            /* encoder press 0..3 blue..red; pos 0 synth, 1 drum */
    /* encoder turn: edit w in place; the core saves the words into the sound when they changed */
    void (*turn)(int16_t *w, int pos, int knob, int delta, int shift);
    /* MIDI in (may be 0): every control change after the stock CC handling, and MIDI START */
    void (*midi_cc)(int cc, int value);
    void (*midi_start)(void);
} op1_lfo_type;

#endif
