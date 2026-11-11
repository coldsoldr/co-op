/*
 * kennel feature API: features (chord mode, later scale, melody, ...) register callbacks at hook points the core owns
 * (docs/feature-hooks.md). A feature is compiled into the core or shipped as a plugin file of kind 3; both export one
 * op1_feature_t. Include this header (and op1_plugin.h, which it includes) from feature code.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_H
#define OP1_FEATURE_H
#include "op1_plugin.h"

#define OP1_FEATURE_API 1                /* a file built against another core layout is refused, not run. Not a version: it stays 1 until the first release */

/* the 16-byte note record VoiceCtl key_down / key_up pass to voicectl_synth_note_on / _note_off. Keyboard, MIDI in
 * and every sequencer reach the synth through these two calls. */
typedef struct { int32_t note, velocity; uint8_t send_midi, pad[3]; int32_t extra; } op1_note;
/* Reserved transient extra sentinel: owned by final-pitch sequencer routing.
 * Preserved through VoiceCtl copies and cleared before the stock synth sink. */
#define OP1_NOTE_SCALE_FINAL 0x4b4e5154

/* A clock page: in clock mode its T key toggles the page; while it is shown the four encoder pushes and turns go to
 * it. The screen is built at boot on `svg` (a file in content/display; "usbout.svg" is an empty one for pages
 * drawn in code). tkey 1..4. When the core shows the page (or the view) it calls push(-1) once: ignore it if
 * there is nothing to do (kennel/kn_pages.c). */
typedef struct {
    uint8_t tkey;
    const char *svg;
    const op1_screen_t *screen;
    void (*push)(int knob);                       /* 0..3 blue..red, on press */
    void (*turn)(int knob, int delta);
} op1_page_t;

/* Hook points beyond notes and pages: each has its own header (op1_feature_<point>.h) with the struct a feature
 * fills; this header only names them, so features that don't use a point never see its details. */
struct op1_seq_type;                             /* sequencer type: op1_feature_seq.h */
struct op1_lfo_type;                             /* LFO type: op1_feature_lfo.h */
struct op1_drum_synth;
struct op1_drum_type;                            /* drum type: op1_feature_drum.h */
struct op1_tape_hooks;                           /* tape: op1_feature_tape.h */

/* Events (op1_feature_t.event): delivered on the main loop thread unless noted. New ids may be added; ignore unknown. */
enum {
    OP1_EV_BOOT_DONE = 1,     /* first main loop pass after boot (screens up, sounds loaded) */
    OP1_EV_MODE,              /* arg: the new mode id (ModeSwitcher, 0xFF907D58: clock 0x0C, ...) */
    OP1_EV_SLOT,              /* arg: (instrument << 8) | slot: the current synth slot changed (instrument 0) */
    OP1_EV_TRANSPORT,         /* arg: the new tape state (0x018C17D4: 3 stopped, 6 armed, 10 playing, ...) */
    OP1_EV_TEMPO,             /* arg: the new beat length, 1/16384 samples (op1_beat_t.len) */
    OP1_EV_KEY_HOLD,          /* arg: key code held for 600 ms (KeyHandler codes, as op1_input.code) */
    OP1_EV_TAPE_EDIT,         /* an edit of the tape, delivered before and after it on the thread that makes it (main
                               * loop or KeyHandler): arg = op | track << 8 | before << 16, details from
                               * op1_tape_edit_now(arg) (op1_feature_tape.h) */
    OP1_EV_SAVE_HOLD,         /* arg: kind << 8 | slot (kind 0 synth, 1 drum; slot 0..7 = sound key 1..8): a held
                               * sound key is about to be saved by the stock hold-to-save (main loop). Call
                               * op1_snapshot_defer() in the event to hold that save back. */
    OP1_EV_TAPE_WRITE,        /* arg: track | chunk << 8: the tape cache is about to write 64 KiB of a track to the tape
                               * file (main loop); op1_tape_write_now() / op1_tape_read_old() (op1_feature_tape.h) */
    OP1_EV_TAPE_DRAINED,      /* after a tape edit or a recording: everything has reached the tape file (main loop) */
    OP1_EV_DISK,              /* arg 1: disk mode is starting (the tape cache is flushed, the host gets the disk next);
                               * 0: disk mode has ended (the sync of what the host changed follows). Main loop */
    OP1_EV_TAPE_SWITCH,       /* arg: old tape | new tape << 8 | before << 16 (tapes 0..3): the tape is being switched
                               * (before: the cache flushed, the old tape still open) or has been (after: the new tape
                               * mounted, its clips, loop and tempo loaded; new == old if the switch failed). Main loop */
    OP1_EV_NOTES_RESET,      /* stock voices already reset; clear note-stage history (caller thread) */
    OP1_EV_REGISTER,         /* enabled features, after claims resolve, before screen construction: register hook types */
    OP1_EV_SERVICE,          /* each main-loop pass; short, nonblocking work, never the audio thread */
};
#define OP1_HAS_TAPE_SWITCH 1        /* OP1_EV_TAPE_SWITCH and op1_tape_current (op1_feature_tape.h) exist */

/* Input (op1_feature_t.input): every key and encoder event, before the stock code, the core's pages and the other
 * features (priority order). Return 1 to consume it. KeyHandler thread. */
enum { OP1_IN_KEY = 1, OP1_IN_KNOB = 2 };
typedef struct {
    uint8_t type;             /* OP1_IN_KEY / OP1_IN_KNOB */
    uint8_t code;             /* key code, or knob 0..3 (blue, green, white, red) */
    uint8_t pressed;          /* key: 1 down, 0 up */
    uint8_t shift;            /* Shift held */
    int16_t delta;            /* knob: encoder delta (1024 per detent) */
    int16_t mode;             /* current mode id (OP1_EV_MODE) */
    int32_t screen;           /* current screen id (0x018B65C8) */
} op1_input;

/* Note input observers (op1_feature_t.note_in): VoiceCtl key_down / key_up entries (0x01A85750 / 0x01A85840), every
 * note from the keyboard, MIDI in and the sequencers, synth and drum, before the note pipeline. Look, don't change.
 * Notes a feature plays itself with op1_key_down / op1_key_up come back here too: guard against your own. */
typedef struct {
    void (*down)(const op1_note *n, int param);
    void (*up)(const op1_note *n);
} op1_note_input;

/* Claims (op1_feature_t.claims): the exclusive resources a feature takes, a 0-terminated list of names. Two features
 * with the same claim cannot both run: at boot the core keeps one (a built-in beats a file, then the lower priority,
 * then the name) and switches the other off completely, with a log line and a toast. The workshop reads the same
 * list from plugin files to warn before installing. Names: "page:t1".."page:t4" (clock pages), "view" (clock
 * view), "tape.erase", "tape.metro", "tape.lookup" (single-owner tape hooks), "lfo:<name>", "seq:<name>",
 * "drum:<name>", "key:<mode>:<combo>" (a key or encoder combo the feature consumes, e.g. "key:synth:shift+t2"),
 * "view:com" (the COM view, op1_feature_t.com). */

typedef struct {
    uint16_t api;                                 /* OP1_FEATURE_API */
    uint16_t priority;                            /* note pipeline order, lower first (scale 100, chord 200, melody 300) */
    const char *name;
    /* note pipeline stage (0 = not in the pipeline): pass notes on with op1_note_on / op1_note_off (next, ...),
     * zero, one or several per input note. Route a note_off by your own record of what you sent for that note. */
    void (*note_on)(const op1_note *n, int next);
    void (*note_off)(const op1_note *n, int next);
    void (*key_sync)(void);                       /* every key event (KeyHandler thread), may be 0 */
    const op1_page_t *page;                       /* clock page, may be 0 */
    void (*tick)(const op1_beat_t *beat);         /* audio thread, every 64-sample block, before the metronome: short! */
    /* one slot per hook point, 0 = not used */
    const struct op1_seq_type *seq;
    const struct op1_lfo_type *lfo;
    const struct op1_drum_type *drum;
    const struct op1_tape_hooks *tape;
    const op1_page_t *view;                       /* clock view (Shift + Help), like Oblique Strategies */
    /* common hooks */
    void (*event)(int id, int32_t arg);           /* OP1_EV_* */
    int (*input)(const op1_input *in);            /* 1 = consumed */
    void (*midi_in)(const uint8_t *msg, int len); /* incoming MIDI: CC (B0 cc v) and START (FA) so far; look, don't change */
    const op1_note_input *note_in;                /* note input observers, may be 0 */
    const char *const *claims;                    /* exclusive resources, 0-terminated, may be 0 */
    /* every frame, after all screens are drawn (Gfx thread): draw over the current screen (screen = its id, as
     * op1_input.screen). Keep it short; use op1_raster_line / op1_text_* on raster. */
    void (*overlay)(void *raster, int32_t screen);
    /* COM view (claim "view:com"): in COM (Shift+COM, mode 11) Shift+COM again toggles it against the stock COM
     * screen; while shown it gets the four encoder pushes and turns (kennel/kn_pages.c). tkey is not used. */
    const op1_page_t *com;
    /* every USB MIDI message as it arrives, any channel, before the stock parser (timer context, keep it short): notes
     * (8n / 9n, note-on with velocity 0 as sent), CC, program change, pressure, pitch bend, clock F8, start FA,
     * continue FB, stop FC. The channel is the status byte's low nibble. Look, don't change. op1_feature_midi.h */
    void (*midi_raw)(const uint8_t *msg, int len);
    /* SEQ + TAPE view (claim "view:seqtape"): holding SEQ and pressing TAPE opens it full screen over the tape screen;
     * TAPE alone or another mode key closes it. While it is shown it gets the encoder pushes and turns that the
     * features' input filter leaves (op1_tape_view_open() says if it is shown). tkey is not used. */
    const op1_page_t *tape_view;
    const struct op1_drum_synth *drum_synth;        /* native synthesized drum instrument, checked by loader */
} op1_feature_t;

/* the next stage of the note pipeline (past the last one: R246's VoiceCtl) */
void op1_note_on(int next, const op1_note *n);
void op1_note_off(int next, const op1_note *n);

/* play a note through VoiceCtl key_down / key_up, as the keyboard does: note input observers, then the note pipeline */
void op1_key_down(const op1_note *n, int param);
void op1_key_up(const op1_note *n);

/* ---- core services for features and plugins ---- */
void *op1_alloc(uint32_t size);                   /* zeroed SDRAM, never freed; 0 when full. Best at init. */
void op1_toast(const char *text);                 /* a short message over every screen for ~2 s (any thread) */
int op1_midi_send(const uint8_t *msg, int len);   /* 1..3 byte MIDI message out over USB; 0 ok */
/* settings, saved in op1.db (op1rsettings, key "<feature>.<key>"); not from the audio thread */
int32_t op1_setting_get(const char *feature, const char *key, int32_t def);
void op1_setting_set(const char *feature, const char *key, int32_t value);
int op1_setting_get_str(const char *feature, const char *key, char *out, int size);   /* length, 0 if none */
void op1_setting_set_str(const char *feature, const char *key, const char *value);   /* [A-Za-z0-9 ._,:-] only */

/* Text entry: a modal screen over everything (drawn after the overlays) that takes every key press and encoder turn
 * while it is open. Blue turns one character, green one row; blue or white push adds the character, left deletes the
 * last one, red push finishes (ok 1). A mode key (help, tempo, synth, drum, tape, mixer, sequencer, COM, mic)
 * finishes with ok 0 and then acts as usual. Characters: `charset`, or "a".."z", "0".."9" when 0; only
 * [A-Za-z0-9 ._,-] are accepted (op1_setting_set_str and FAT file names take them); shown uppercase. `key` (may be
 * 0) sees each key press and release first (KeyHandler thread): return 1 to take it (e.g. a sound key that saves:
 * call op1_text_entry_close(1) there). `done` runs once on the main loop with the text (trailing spaces and dots
 * trimmed) and ok. Releases of keys that were down when the entry opened go to the stock code. */
#define OP1_TEXT_MAX 24
typedef struct {
    const char *title;                            /* top line, may be 0 */
    const char *initial;                          /* start text, may be 0 */
    const char *charset;                          /* 0: a-z 0-9 */
    uint8_t max;                                  /* 1..OP1_TEXT_MAX */
    uint8_t cols;                                 /* characters per row in the grid, 1..16, 0: 9 */
    void (*done)(const char *text, int ok);       /* main loop */
    int (*key)(int code, int pressed, int shift); /* KeyHandler, may be 0: 1 = taken */
} op1_text_entry_t;
int op1_text_entry(const op1_text_entry_t *te);   /* 0 opened; -1 another one is open or te is invalid. Any thread */
void op1_text_entry_close(int ok);                /* finish the open entry (done follows on the main loop) */
int op1_text_entry_open(void);                    /* 1 while an entry is open (until its done has run) */

/* SDRAM kept for tape undo: kn_mem.h KN_UNDO_LO..KN_UNDO_HI (0x02800000..0x02980000, 1.5 MiB), between R246's last
 * BSS (0x027FFB03) and the plugin arena. Nothing else in kennel or R246 uses it; one owner (the first caller), no cache flush needed
 * (data, not code). Returns the start and sets *size, or 0 when another feature has it. Main loop or boot. */
void *op1_undo_area(const char *owner, uint32_t *size);

/* Snapshot save (kennel/kn_save.c), for sound naming. When R246's hold-to-save is about to save a held sound key,
 * every feature gets OP1_EV_SAVE_HOLD; one that calls op1_snapshot_defer() in that event holds the save back (the
 * progress goes away as if the key had been released). It then finishes it with op1_snapshot_save(kind, slot,
 * name): R246 saves that slot as it would have, under `name` (stock adds "_1", "_2" if the name is taken), or under
 * its own date stamp when name is 0 or "". Kind may differ from the bank shown at that moment (a mode key ends the
 * naming): the core sets the bank for the one save call. Only a held-back save can be finished (-1 otherwise, or
 * while another save waits); a newer hold-back replaces an unfinished one. The save runs on a later main loop pass
 * with help and Shift up. Names: [A-Za-z0-9 ._,-], up to OP1_SNAPSHOT_NAME_MAX (R246's synth stamp buffer is 16). */
#define OP1_SNAPSHOT_NAME_MAX 15
void op1_snapshot_defer(void);                            /* only inside OP1_EV_SAVE_HOLD */
int op1_snapshot_save(int kind, int slot, const char *name);   /* 0 queued, -1 refused. Any thread */
int op1_snapshot_name_exists(int kind, const char *name);      /* 1 if preset_<kind>/snapshot/<name>.raw exists. Main loop */

/* Storage (kennel/kn_storage.c), for the FILE view: bytes on the flash per category, from a walk of /yaffs2 that runs
 * on the main loop a few entries per pass and is cached until the next disk mode ends (or op1_storage_refresh).
 * Files count by their data chunks (2 KiB each; the sparse tape file by what it really takes). tape = user/tape_c.raw
 * + user/tape.db; sounds = the rest of user/; mods = plugins/ and .knl / .op1p files in content/; factory = the
 * rest of content/; system = everything else; bad = dead blocks; free = YAFFS free space; overhead = the rest of
 * YAFFS's total (headers, chunks waiting for garbage collection, reserved blocks). */
enum { OP1_STORE_TAPE, OP1_STORE_SOUNDS, OP1_STORE_FACTORY, OP1_STORE_SYSTEM, OP1_STORE_MODS, OP1_STORE_BAD,
       OP1_STORE_OVERHEAD, OP1_STORE_FREE, OP1_STORE_N };
typedef struct {
    uint32_t bytes[OP1_STORE_N];                  /* per category, OP1_STORE_* */
    uint32_t total;                               /* YAFFS total space (the categories add up to it) */
    uint32_t files, file_size;                    /* files walked, the sum of their sizes */
    uint16_t gen;                                 /* +1 per finished walk */
    uint8_t ready;                                /* 1: the numbers are from a finished walk */
} op1_storage_t;
int op1_storage(op1_storage_t *out);              /* copies the cache; ready (1) or not yet (0, a walk is started). Any thread */
void op1_storage_refresh(void);                   /* walk again (the cache stays until it finishes) */

/* Files under /yaffs2/user/ (kennel/kn_files.c), e.g. for tape undo's history. Paths: "/yaffs2/user/" then names of
 * [A-Za-z0-9 ._-] joined by single slashes, no "." or ".." part, under 120 characters; anything else is refused (-1).
 * read / write / seek / close accept only handles op1_file_open returned (8 open at most). Main loop. */
enum { OP1_FILE_READ = 0, OP1_FILE_WRITE = 1, OP1_FILE_RDWR = 2, OP1_FILE_ACCESS = 3, OP1_FILE_CREATE = 0x40,
       OP1_FILE_TRUNC = 0x200 };
int op1_file_open(const char *path, int flags);   /* a handle, or -1 */
int op1_file_read(int fd, void *dst, int len);    /* bytes read, or -1 */
int op1_file_write(int fd, const void *src, int len);   /* bytes written, or -1 */
int32_t op1_file_seek(int fd, int32_t off, int whence); /* whence 0 start, 1 current, 2 end; the new position or -1 */
int op1_file_close(int fd);
int op1_file_unlink(const char *path);            /* 0 or -1 */
int op1_file_mkdir(const char *path);             /* 0 or -1 (also when it exists) */

/* Flash check (kennel/kn_flash.c), opened from FILE (blue push): a read-only scan of every flash block through the
 * NAND driver's own read (nothing is marked or retired), counting healthy, fixed (ECC corrected), errors and dead
 * blocks; it reads on the main loop while the check is shown, the tape is stopped and disk mode is off (PAUSED
 * otherwise). The core draws the whole screen (op1_flash_draw); a COM view shows it while op1_flash_shown(). */
void op1_flash_open(void);                        /* show it and start a scan (again, after one finished) */
void op1_flash_close(void);                       /* hide it (the scan state is kept) */
int op1_flash_shown(void);
void op1_flash_draw(void *raster);                /* Gfx thread: counts, SCAN %, the block map */
int op1_flash_push(int knob);                     /* while shown: blue (0) checks again; another knob closes it: 1 */
int op1_flash_counts(int32_t out[4]);             /* healthy, fixed, errors, dead; 0 idle, 1 scanning, 2 done */

/* shared music context: a feature that sets the key publishes it, the others read it */
typedef struct {
    volatile uint8_t valid;                       /* 0: nobody has set a key */
    volatile uint8_t root;                        /* 0..11, 0 = C */
    volatile uint16_t scale_mask;                 /* bit n = n semitones above the root is in the scale */
    const char *volatile owner;                   /* name of the feature that set it */
} op1_music_t;
extern op1_music_t op1_music;
/* set the key on purpose (the user changed it): one owner, the last feature that published; valid 0 withdraws
 * the key only if `owner` still holds it */
void op1_music_publish(const char *owner, int valid, int root, uint16_t scale_mask);
/* Active harmony snapshot: root, scale mask, Scale active, Chord active.
 * Scale has priority when both are active; returns 0 when neither is active. */
int op1_harmony_context(int32_t out[4]);

#endif
