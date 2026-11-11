/*
 * kennel feature API, MIDI and synth services (kennel/kn_midi.c): where a note comes from, synth parameters set like
 * a knob turn, notes into the stock sequencers, the SEQ + TAPE view and on / off switches on the settings page.
 * op1_feature_t.midi_raw and .tape_view are in op1_feature.h.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_MIDI_H
#define OP1_FEATURE_MIDI_H
#include "op1_feature.h"

/* Where the note being played comes from: valid inside note input observers (op1_feature_t.note_in) and note
 * pipeline stages, for the note they were called with. Taken from VoiceCtl key_down / key_up's caller. */
enum {
    OP1_SRC_KEYS = 1,         /* the keyboard (no sequencer took the key) */
    OP1_SRC_PLAYER = 2,       /* a stock sequencer or arpeggio, or a kennel sequencer type (op1_seq_note_on) */
    OP1_SRC_MIDI = 3,         /* received MIDI; the channel (0..15) is OP1_SRC_CH(source) */
    OP1_SRC_FEATURE = 4,      /* a feature's own op1_key_down / op1_key_up */
};
#define OP1_SRC_KIND(s) ((s) & 0xFF)
#define OP1_SRC_CH(s)   (((s) >> 8) & 15)
int op1_note_source(void);                        /* OP1_SRC_* | channel << 8; 0 outside a note */

/* Synth parameters of the current synth slot, set like turning the knob on that page and saved in the sound (the
 * patch setter marks it changed). value 0..127 over the knob's range. Any thread: the value is stored and applied on
 * the KeyHandler thread, as the stock knobs are. Skipped (returns -1): engine knobs that are sample start / end
 * pairs, LFO types with packed words (MIDI LFO / quad), FX or LFO switched off in the slot. Returns 0 if queued. */
enum { OP1_PARAM_ENGINE = 0, OP1_PARAM_ENV = 1, OP1_PARAM_FX = 2, OP1_PARAM_LFO = 3 };
int op1_param_set(int group, int knob, int value);    /* knob 0..3 (envelope: attack, decay, sustain, release) */

/* CCs on the OP-1's channel still reach midi_in, but a taken one is no longer turned into a key press or encoder
 * turn by the core (CC = key code + 5, CC 102..105 encoders). For CC maps of a feature's own. Any thread. */
void op1_midi_take_cc(int cc, int take);

/* A note into the stock sequencer of the current mode, as a key of the keyboard: it reaches the sequencer in order
 * with the real keys (KeyHandler queue). note 0..127 folds onto the 24 keys (53..76 at octave 0). A key that is
 * already down is not pressed again (the stock code asserts). Any thread. */
void op1_stock_seq_key(int note, int down);

int op1_tape_view_open(void);                     /* 1 while the SEQ + TAPE view is shown */
void op1_tape_view_close(void);                   /* back to the tape screen */
void op1_tape_view_enable(int on);                /* 0: SEQ and TAPE stay stock (the view closes); default 1 */

/* On / off switches on the settings page (clock, T3): green, white and red, in registration order (up to three).
 * The label (up to 8 characters fit before the value) is drawn like the page's count-in row. The value is saved as setting "<feature>.on" (default off) and
 * read with op1_switch_on. Register at OP1_EV_BOOT_DONE or earlier. */
int op1_switch(const char *feature, const char *label);   /* 0 ok, -1 no room */
int op1_switch_on(const char *feature);                   /* 1 on, 0 off (or not registered). Any thread */

#endif
