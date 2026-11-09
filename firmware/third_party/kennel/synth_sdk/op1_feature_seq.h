/*
 * kennel feature API, sequencer types (docs/feature-hooks.md "Sequencer types"): a feature that sets
 * op1_feature_t.seq adds a sequencer the OP-1 offers next to its stock ones (euclid, dots, oracle, peal).
 * The core (kennel/kn_seq.c) owns every R246 patch site and dispatches to the registered types:
 *   - name: the sequencer's name in the seq_types table of op1_factory.db and in saved sounds. The build writes the
 *     seq_types row (make_firmware.patch_db) whether the type is compiled in or shipped as a file. A name never
 *     changes meaning once released.
 *   - SeqIds::from_name (0x01A42A2E / 0x01A42BDC / 0x01A42CE0) maps the name to R246's type 3 (finger) and marks the
 *     type active for the current mode (synth / drum); SeqIds::name (0x01A422EC) gives the name back for type 3 while
 *     it is active, so the sound saves with it. A saved name with no registered type (its file missing or broken)
 *     runs as finger and keeps its name, so the sound does not halt the OP-1 and comes back when the file does.
 *   - PlayerManager::create_player case 3 (0x01A0D150) gets the active type's player(), finger's otherwise.
 *   - the screen replaces finger's in slot 0x2D while the type is the mode's sequencer; while it is shown, left /
 *     right (keyboard octave) are handled by the core and the encoders and other keys go to the type.
 *   - persistence is the type's own: its own SQLite table through R246's background saver (seq/seq_text.h
 *     sq_store_*), loaded on the first player() call. The table name, like the type name, never changes.
 * mode below: 0 synth, 1 drum. All callbacks except player() and midi_start may be 0.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_SEQ_H
#define OP1_FEATURE_SEQ_H
#include "op1_feature.h"

#define OP1_SEQ_NAME_MAX 15                      /* seq_types names, as R246's short strings */

/* A player object is a pointer to a vtable of {adj, fn} pairs (fn(self + adj, ...)), offsets as R246's ok_player
 * (vtable 0x01910E9C): +0x14 key_down(key 0..23) -> 1 = consumed, +0x1c key_up(key) (consumed keys only),
 * +0x24 reset, +0x2c tick (96 per beat, the current mode's player while the sequencer runs), +0x3c stop. */
typedef struct op1_seq_type {
    const char *name;                             /* seq_types / saved sound name, <= 15 chars, lowercase */
    const char *alias;                            /* an older name that also selects it, or 0 */
    void *(*player)(void);                        /* player object for create_player case 3 */
    const char *svg;                              /* screen file in content/display (shipped by the build) */
    const op1_screen_t *screen;                   /* drawn in slot 0x2D; 0 = finger's screen */
    /* on its screen: a key event other than left / right and the encoder pushes; 1 = consumed (press and release) */
    int (*key)(int mode, int code, int pressed, int shift);
    void (*press)(int mode, int knob, int shift);           /* encoder push, knob 0..3 blue..red */
    void (*turn)(int mode, int knob, int delta, int shift); /* encoder turn on its screen */
    int (*shift_wants)(int key);                  /* Shift + key 0..23 is a shortcut of this sequencer (Scale mode) */
    void (*midi_start)(void);                     /* MIDI START (FA): restart running parts at step 0 */
} op1_seq_type_t;

/* core services for sequencer types (exported to plugin files) */
int op1_seq_note_on(int mode, int note, int key, int vel);   /* VoiceCtl key_down as the stock sequencers; -> id */
/* Already scale-quantized synth pitch: bypass only the Scale keyboard mapper.
 * Chord and subsequent stages still run. Pair the returned opaque ID with note_off. */
int op1_seq_note_on_quantized(int mode, int note, int key, int vel);
void op1_seq_note_off(int id);                                /* id from op1_seq_note_on */
void op1_seq_press_play(void);                   /* press PLAY once the current key event is done (start with REC) */

#endif
