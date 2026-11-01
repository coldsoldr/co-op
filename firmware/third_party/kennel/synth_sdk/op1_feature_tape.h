/*
 * kennel feature API, tape hook point (docs/feature-hooks.md "Tape"): a feature that works on the tape (count-in,
 * punch in / out, markers, fast erase) fills an op1_tape_hooks and points op1_feature_t.tape at it. The core owns
 * every R246 patch site and calls the registered hooks in priority order; with none registered each site behaves
 * as stock R246. R246's encrypted L1 code (tape resampler, wow, mix-loop meters) stays in the core.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_TAPE_H
#define OP1_FEATURE_TAPE_H
#include "op1_feature.h"

struct op1_tape_hooks {
    /* audio thread, once per block, before the metronome render (L1 audio callback, l1v_metro) */
    void (*block)(void);
    /* audio thread: wraps the stock metronome render; call `render` once. One owner: the first registered
     * feature that sets it; without one the core calls the stock render. */
    void (*metro)(void (*render)(void));
    /* main loop, after the stock tape_c::service(0) (0x019F0A24) */
    void (*service)(void);
    /* key thread, every key event before the stock key: return 1 to consume it (the stock code and later features
     * don't see it). tape_screen: the stock tape screen is the current one. */
    int (*key)(int code, int pressed, int tape_screen);
    /* encoder turn (knob 0..3 blue..red, delta 1024 per detent): return 1 to consume it */
    int (*knob)(int knob, int delta, int tape_screen);
    /* tape screen, after the stock draw (op1_raster() is the current raster) */
    void (*draw)(void);
    /* replaces the stock erase of the whole tape file (clipboard command 4: tape_c 0x01A54B08). One owner, the
     * first registered; it must leave the file as the stock erase would (all zero). */
    void (*erase)(void);
    /* tape screen, after the stock recording-state update (CALL 0x01A5C728 at 0x01A5CFA8, scr = the tape screen) and
     * before the stock draw reads it (e.g. the red recording theme: scr + 0x93 = 1) */
    void (*pre_draw)(void *scr);
    /* tape_c chunk lookup 0x01A54D5C, called from the L1 tape tick (audio thread): position (samples), &direction ->
     * cache entry. One owner, the first registered; `stock` is the original lookup. Short, integers only. */
    void *(*lookup)(int32_t pos, int32_t *direction, void *(*stock)(int32_t pos, int32_t *direction));
};

/* Tape edits (OP1_EV_TAPE_EDIT, op1_feature.h): every feature's event() gets each edit twice, before it changes
 * anything (audio or clip rows) and after it is complete, synchronously on the thread that makes it, so a feature
 * can save what is about to change (tape undo). arg = op | track << 8 | before << 16; op1_tape_edit_now(arg) has
 * the details while the event runs (one record per thread, so a split during a queued edit doesn't overwrite it). R246 sites (kennel/kn_tape.c):
 *   LIFT, LIFT_ALL, DROP, DROP_ALL, ERASE  main loop: R246's clipboard queue (lift, Shift+lift, drop, Shift+drop,
 *                                          erase; a clip slide ends with a DROP flagged slide), CALL 0x0191B560 at
 *                                          0x019F0A2A, one before / after pair per queued command
 *   SPLIT                                  KeyHandler: scissors (0x01A56F24) and the splits Shift+lift makes at the
 *                                          loop points before it queues LIFT_ALL (0x01A57690 / 0x01A5769C)
 *   JOIN                                   KeyHandler: Shift+scissors (0x01A577C6)
 *   REC                                    before: the tape goes into recording (set_mode, before the audio thread
 *                                          writes), track and position (start = stop). After: R246 has added the
 *                                          finished recording's clip row (set_mode, 5 sites), start / stop = that
 *                                          clip. Both on the thread that calls set_mode (KeyHandler for the keys).
 *                                          A recording that wrote nothing gets no clip and no after event.
 * Range: samples, [start, stop). Reverse is a playback direction in R246, not an edit. */
enum { OP1_TAPE_ERASE = 1, OP1_TAPE_LIFT, OP1_TAPE_LIFT_ALL, OP1_TAPE_DROP, OP1_TAPE_DROP_ALL, OP1_TAPE_SPLIT,
       OP1_TAPE_JOIN, OP1_TAPE_REC };
#define OP1_TAPE_ALL               0xF            /* track: all four */
#define OP1_TAPE_EDIT_OP(arg)      ((arg) & 0xFF)
#define OP1_TAPE_EDIT_TRACK(arg)   (((arg) >> 8) & 0xF)
#define OP1_TAPE_EDIT_BEFORE(arg)  (((arg) >> 16) & 1)
typedef struct {
    uint8_t op, track, before, slide;             /* slide: a DROP that ends a clip slide */
    int32_t start, stop;                          /* samples; ERASE: 0 .. the end of the tape */
    int32_t clip;                                 /* LIFT: the clip row removed (-1 none); otherwise -1 */
} op1_tape_edit_t;
const op1_tape_edit_t *op1_tape_edit_now(int32_t arg);

/* Tape writeback, for undo (kennel/kn_tape.c). The tape cache keeps 32768-sample chunks of all four tracks in RAM and
 * writes a modified chunk of a track back to the tape file (64 KiB) on the main loop; a recording reaches the file
 * this way, chunk by chunk. Just before each such write every feature gets OP1_EV_TAPE_WRITE (main loop); during the
 * event the old bytes are still in the file: op1_tape_read_old copies them (a part or all 64 KiB). Keep it short:
 * the main loop does the tape's file I/O, so a long event delays the cache (and, when it falls behind, playback).
 * Lift, drop and erase write the file themselves and have OP1_EV_TAPE_EDIT before events instead.
 * OP1_EV_TAPE_DRAINED follows a tape edit or a recording once everything has reached the file: not recording, the
 * clipboard queue empty and no modified chunk left in the cache.
 * op1_tape_flush() (main loop) writes every modified chunk now; the core also does it before disk mode starts (the
 * host reads the tape file directly). Returns a work count, or -1 when dirty data
 * remains or a write callback attempts a nested flush; failed flushes block disk/tape switching. */
typedef struct {
    uint8_t track;                                /* 0..3 */
    int32_t chunk;                                /* chunk index: samples chunk * 32768 .. + 32768 */
    int32_t offset, len;                          /* bytes in the tape file (len 65536) */
    const int16_t *data;                          /* the new samples about to be written */
} op1_tape_write_t;
const op1_tape_write_t *op1_tape_write_now(void); /* during OP1_EV_TAPE_WRITE */
int op1_tape_read_old(void *dst, int32_t at, int32_t len);   /* exact old bytes [at,at+len); -1 on bounds/seek/read failure or outside the event */
/* Undo's read and restore, both through the tape file, as R246's lift and drop work (main loop only; -1 while
 * recording, while R246's clipboard queue has commands (except inside their OP1_EV_TAPE_EDIT events), or out of
 * range: track 0..3, samples 0..0xF28000). op1_tape_read flushes the cache first, so it returns the newest audio.
 * op1_tape_restore flushes, writes the file and drops the cached chunks in the range (tape_cache_invalidate_clean_range
 * 0x01A540C0: they reload, the loop seams are rebuilt); it never goes through the cache, so it fires no
 * OP1_EV_TAPE_WRITE. The LIFT, LIFT_ALL, DROP, DROP_ALL and ERASE before events fire with the cache flushed. */
int op1_tape_read(int track, int32_t at, int32_t len, int16_t *dst);          /* samples; len or -1 */
int op1_tape_restore(int track, int32_t at, int32_t len, const int16_t *data); /* samples; len or -1 */
int op1_tape_flush(void);

/* Clip rows per track, for undo (kennel/kn_clips.c; main loop): a snapshot and a replace-all, through R246's own clip
 * pool calls (tape_clips_remove_channel, tape_clips_add), so the pool and tape.db stay in step. Ids are not kept: R246
 * gives new ones; the ranges come back exactly when they are valid clips (each at least the minimum clip length,
 * 0xDC8 samples, inside 0..0xF23FA0: what get returns always is). set returns -1 (and changes nothing) while
 * recording, while R246's clipboard queue has commands, for an invalid range, or when there are not enough free rows. */
typedef struct { int32_t start, stop; } op1_tape_clip;  /* samples */
int op1_tape_clips_get(int track, op1_tape_clip *out, int max);   /* the track's clip count; the first max by start */
int op1_tape_clips_set(int track, const op1_tape_clip *clips, int n);   /* 0 or -1 */                         /* main loop: writes left (0 = nothing was waiting) */   /* the edit being reported (the event's arg), during the event */

/* tape zoom (R246's float px/s at 0x018C183C) as a Q10 integer, clamped to 1..4096 px/s; no soft-float */
static inline int32_t op1_tape_zoom_q10_bits(uint32_t b)
{
    int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 0) return 1 << 10;                        /* negative, < 1 px/s, 0, denormal */
    if (e >= 12) return 4096 << 10;                                /* >= 4096 px/s, inf, nan */
    uint32_t m = (b & 0x7FFFFF) | 0x800000;                        /* 1.m in Q23 */
    return (int32_t)(m >> (13 - e));                               /* Q23 * 2^e -> Q10 */
}
#ifndef OP1_HOST
static inline int32_t op1_tape_zoom_q10(void) { return op1_tape_zoom_q10_bits(*(volatile uint32_t *)0x018C183C); }
#endif

/* Tape services (kennel/kn_tapesvc.c): loop, track, playhead and transport through R246's own setters (any thread; none
 * refuses during playback), and R246's audio clipboard, what lift fills and drop reads (main loop). Samples throughout. */
int op1_tape_loop_get(int *on, int32_t *in, int32_t *out);   /* returns on; pointers may be 0 */
int op1_tape_loop_set(int on, int32_t in, int32_t out);      /* 0..0xF23FA0, in <= out; on sticks only if in != out (as
                                                              * R246's loop keys); the tape row is saved later. 0 / -1 */
int op1_tape_track(void);                                    /* selected track 0..3 */
int32_t op1_tape_pos_get(void);                              /* playhead */
int op1_tape_pos_set(int32_t samples);                       /* R246's locate (it suspends the loop, as its own do). 0 / -1 */
enum { OP1_TAPE_STOPPED = 0, OP1_TAPE_PLAYING = 1, OP1_TAPE_RECORDING = 2 };
int op1_tape_transport(void);                                /* OP1_EV_TRANSPORT reports changes of the raw state */
int op1_clipboard_info(int *chmask, int32_t *len);           /* content: 0 empty, 1 tape lift, 4 / 5 synth / drum sample;
                                                              * chmask 1 << track, 0xF for all four */
int op1_clipboard_read(int ch, int32_t at, int32_t len, int16_t *dst);   /* samples copied, or -1 */
int op1_clipboard_write(int chmask, int32_t len);            /* set up as a lift would be, without touching the tape: one
                                                              * track (len < 0x3C8FE8) or 0xF (len <= 0xF23F9 each); -1
                                                              * while R246's clipboard queue has commands. Fill the slots
                                                              * in the same main loop pass */
int16_t *op1_clipboard_slot(int ch);                         /* where track ch's samples live, 0 if it isn't in it */

/* Four tapes (kennel/kn_tapes.c, docs/tapes.md): tape 0 is TE's (/yaffs2/user/tape_c.raw, tape.db), tapes 1..3 live
 * in /yaffs2/user/tape2..4/ and are made (sparse, empty, at the current tempo) on first use. op1_tape_switch asks
 * for a switch (any thread); the main loop stops playback, saves the old tape's row and tempo, then swaps the image
 * and tape.db and loads the new tape's clips, loop, position and tempo, with OP1_EV_TAPE_SWITCH before and after.
 * Boot is always tape 0. */
enum { OP1_TAPE_SW_SAME = 1,                      /* already on that tape */
       OP1_TAPE_SW_RECORDING = -1, OP1_TAPE_SW_DISK = -2, OP1_TAPE_SW_BUSY = -3, OP1_TAPE_SW_FULL = -4,
       OP1_TAPE_SW_BAD = -5 };
int op1_tape_switch(int tape);                    /* 0 = asked (done on the main loop), else OP1_TAPE_SW_* */
int op1_tape_current(void);                       /* 0..3, 0 from boot */
int32_t op1_tape_bpm10(void);                     /* the current tempo x 10 (R246's tempobpmdecimal) */
uint32_t op1_flash_free(void);                    /* bytes free on the flash (yaffs_free_space) */

/* Core data the tape features share (so each works without the others, compiled in or as a file):
 * markers publish their positions, punch reads them; punch publishes its mode, markers draw it; the count-in length
 * is a setting (screens/settings.c writes it). */
#define OP1_TAPE_MARKERS 32
extern int32_t tm_pos[OP1_TAPE_MARKERS];          /* sorted marker positions (samples) */
extern int tm_n;                                  /* number of markers, 0 when no markers feature */
extern volatile uint8_t pu_mode;                  /* punch mode: 0 off, 1 armed, 2 punched in */
extern uint8_t ci_beats;                          /* count-in length: 4 / 8 / 16 beats (settings page) */

/* Optional count-in provider (registered after claims). MIDI recording starts PLAY from stop without arming
 * audio tape recording. start: 1 accepted / 0 unavailable or busy; pending spans count and deferred PLAY. */
typedef struct { int (*start)(void); int (*pending)(void); void (*cancel)(void); } op1_countin_t;
void op1_countin_register(const op1_countin_t *provider);
int op1_countin_start(void);
int op1_countin_pending(void);
void op1_countin_cancel(void);

#endif
