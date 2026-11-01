/*
 * Tape hook point (op1_feature_tape.h, docs/feature-hooks.md "Tape"): the core side of the R246 tape sites. The
 * patch targets keep their op1r names (hook.S, link.ld and make_firmware.py point at them):
 *   ci_metro_render  L1 audio callback, metronome render (l1v_metro): feature blocks, then the metro owner or stock
 *   ci_service       main loop, CALL tape_c::service(0) at 0x019F0A24: stock service, feature services, dev update
 *   tm_screen_draw   tape screen vtable draw slot 0x01911B78: stock draw, then feature overlays
 *   fast_tape_erase  clipboard command 4, CALL 0x01A54B08 at 0x0191B9F0: the erase owner or the stock erase.
 *                    Maiko #249's undo wrapper JUMP.Ls here (tools/firmware/maiko249.py ERASE_CHAIN):
 *                    keep it a void(void) function in the core image.
 * and new sites (hook.S): VoiceCtl key_down / key_up entries 0x01A85750 / 0x01A85840 (note input observers), the
 * tape draw's CALL 0x01A5C728 at 0x01A5CFA8 (kn_tape_pre) and tape_c's chunk lookup entry 0x01A54D5C (lookup owner).
 * The entry stubs test kn_note_obs / kn_lookup_owner first: 0 runs the original first instruction and jumps on.
 * dispatcher.c calls kn_tape_key / kn_tape_knob from its key and encoder hooks. With no tape feature registered every
 * site does exactly what stock R246 does.
 * Tape edit events (OP1_EV_TAPE_EDIT, op1_feature_tape.h): the clipboard queue's CALL audio_clipboard_service at
 * 0x019F0A2A (kn_clip_service) and, inside the service, the first change of each command: CALL tape_clips_add at
 * 0x0191B65A (drop) / 0x0191B730 (drop all), CALL lift at 0x0191B89C / lift all at 0x0191B974 (kn_clip_*); erase in
 * fast_tape_erase. The after event of a queued command follows at the next command's before or when the service
 * returns. Split / join: CALL tape_clips_split at 0x01A56F24, 0x01A57690, 0x01A5769C, CALL tape_clips_join at
 * 0x01A577C6 (KeyHandler). Record end: the five CALL tape_clips_add in tape_controller_set_mode (0x01A5828A,
 * 0x01A5830C, 0x01A5838E, 0x01A58432, 0x01A5857E), the clip of a finished recording (kn_rec_clip_add); record start:
 * set_mode's CALL tape_record_extent_reset at 0x01A584D0, only on the way into recording (kn_rec_start).
 * Writeback (OP1_EV_TAPE_WRITE): the tape cache's CALL yaffs_write at 0x01A54842 in tape_cache_io_step (kn_tape_write);
 * drained (OP1_EV_TAPE_DRAINED) and the flush in ci_service; disk mode: COM's four CALL disk_mode_toggle (0x01964336,
 * 0x0196443E, 0x01964616, 0x01964CFE, KeyHandler) -> kn_disk_toggle, which has the main loop flush before entering.
 * Held saves while recording (as op1r 49): ci_service's step (kn_tape_service) keeps a chunk saved less than HOLD_CYCLES
 * ago out of R246's step; recording's end flushes them, and the shutdown's write-back loop (CALL tape_c::service(1) at
 * 0x019F0BE2) becomes kn_tape_final, one bounded flush of everything before the unmount.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "../op1_feature_tape.h"
#include "../kn_mem.h"
#ifdef OP1_HOST
void kn_event(int id, int32_t arg);                  /* the test's */
#define kn_features_tick()
#define kn_core_service()
#else
void kn_event(int id, int32_t arg);
void kn_features_tick(void);
void kn_core_service(void);
#endif

#ifdef OP1_HOST
void host_metro_render(void);
int host_tape_service(int arg);
void host_tape_draw(void *scr, int dt);
void host_stock_erase(void);
#define STOCK_METRO    host_metro_render
#define STOCK_SERVICE  host_tape_service
#define STOCK_DRAW     host_tape_draw
#define STOCK_ERASE    host_stock_erase
void host_tape_pre(void *scr);
void *host_lookup_orig(int32_t pos, int32_t *dir);
void host_key_down(const op1_note *n, int param), host_key_up(const op1_note *n);
#define STOCK_PRE      host_tape_pre
#define LOOKUP_ORIG    host_lookup_orig
#define KEY_DOWN       host_key_down
#define KEY_UP         host_key_up
extern int32_t host_clipq[16 * 8], host_clip_cons, host_clip_prod, host_clip_len;
int host_clip_service(void);
int host_clips_add(void *inst, void *clip), host_clip_lift(int ch, int32_t start, int32_t stop);
int host_clip_lift_all(int32_t start, int32_t stop, void *hdr), host_clips_split(int track, int32_t pos, int flag);
int host_clips_join(int track, int32_t pos);
void host_extent_reset(void);
extern int32_t host_track, host_pos_q6;
#define EXTENT_RESET   host_extent_reset
#define TAPE_TRACK     host_track
#define TAPE_POS_Q6    host_pos_q6
int32_t host_lseek(int fd, int32_t off, int whence);
int host_read(int fd, void *dst, int len), host_write(int fd, const void *src, int len), host_disk_toggle(int arg);
void host_delay_ms(int ms);
extern int32_t host_track_base[4], host_run;
extern uint8_t *host_cache_head;
extern volatile uint8_t host_disk_active;
#define LSEEK          host_lseek
#define READ_FILE      host_read
#define WRITE_FILE     host_write
#define DISK_TOGGLE    host_disk_toggle
#define DELAY_MS       host_delay_ms
#define TRACK_BASE     host_track_base
#define TAPE_RUN       host_run
extern int32_t host_tape_fd;
void host_invalidate(uint32_t from, uint32_t to);
#define TAPE_FD        host_tape_fd
#define INVALIDATE     host_invalidate
#define CACHE_HEAD     host_cache_head
#define DISK_ACTIVE    host_disk_active
uint32_t kn_tape_host_now;                                                /* the test's core cycle counter */
#define NOW()          kn_tape_host_now
#define NODE_DATA      8
#define NODE_NEXT      16                                                 /* the test's node layout (8-byte pointers) */
#define CLIP_Q         host_clipq
#define CLIP_CONS      host_clip_cons
#define CLIP_PROD      host_clip_prod
#define CLIP_LEN       host_clip_len
#define CLIP_SERVICE   host_clip_service
#define CLIPS_ADD      host_clips_add
#define CLIP_LIFT      host_clip_lift
#define CLIP_LIFT_ALL  host_clip_lift_all
#define CLIPS_SPLIT    host_clips_split
#define CLIPS_JOIN     host_clips_join
#else
#define STOCK_METRO    ((void (*)(void))0x019DE548)          /* metronome::render */
#define STOCK_SERVICE  ((int (*)(int))0x01A54710)            /* tape_c::service */
#define STOCK_DRAW     ((void (*)(void *, int))0x01A5CF94)   /* tape screen draw */
#define STOCK_ERASE    ((void (*)(void))0x01A54B08)          /* zero the whole tape file (no arguments) */
#define STOCK_PRE      ((void (*)(void *))0x01A5C728)         /* tape screen: recording state (R0 unused after) */
void *kn_lookup_orig(int32_t pos, int32_t *dir);             /* hook.S: LINK 0, then 0x01A54D60 */
#define LOOKUP_ORIG    kn_lookup_orig
#define KEY_DOWN       ((void (*)(const op1_note *, int))0x01A85750)   /* VoiceCtl key_down / key_up (hooked entries) */
#define KEY_UP         ((void (*)(const op1_note *))0x01A85840)
/* R246's audio clipboard (docs/firmware/tape.md): queue of 16 entries x 8 words (command, channel, start, stop, -,
 * argument, clip id removed by a lift, end-slide flag), consume / produce indices, clipboard length */
#define CLIP_Q         ((volatile int32_t *)0x01AA7130)
#define CLIP_CONS      (*(volatile int32_t *)0x01AA7330)
#define CLIP_PROD      (*(volatile int32_t *)0x01AA7334)
#define CLIP_LEN       (*(volatile int32_t *)0x01AA7104)
#define CLIP_SERVICE   ((int (*)(void))0x0191B560)
#define CLIPS_ADD      ((int (*)(void *, void *))0x0195CCEC)              /* tape_clips_add(pool, clip) */
#define CLIP_LIFT      ((int (*)(int, int32_t, int32_t))0x01A5485C)       /* (channel, start, stop) */
#define CLIP_LIFT_ALL  ((int (*)(int32_t, int32_t, void *))0x01A54984)    /* (start, stop, clipboard header) */
#define CLIPS_SPLIT    ((int (*)(int, int32_t, int))0x0195CF9C)           /* (track, position, flag) */
#define CLIPS_JOIN     ((int (*)(int, int32_t))0x0195D7E8)                /* (track, position) */
#define EXTENT_RESET   ((void (*)(void))0x01A54C1C)                       /* tape_record_extent_reset */
#define TAPE_TRACK     (*(volatile int32_t *)0xFF802964)                  /* selected track: the one recorded */
#define TAPE_POS_Q6    (*(volatile uint32_t *)0xFF802974)                 /* position, 1/64 samples */
#define LSEEK          ((int32_t (*)(int, int32_t, int))0x01A9A0E4)       /* yaffs_lseek(fd, off, whence 0 / 1) */
#define READ_FILE      ((int (*)(int, void *, int))0x01A99C90)            /* yaffs_read */
#define WRITE_FILE     ((int (*)(int, const void *, int))0x01A99F84)      /* yaffs_write */
#define DISK_TOGGLE    ((int (*)(int))0x0199742C)                         /* disk_mode_toggle */
#define DELAY_MS       ((void (*)(int))0x0193E5C0)                        /* sleeps (VDK_Sleep) */
#define TRACK_BASE     ((const int32_t *)0x018C1810)                      /* the four tracks' offsets in the file */
#define TAPE_RUN       (*(volatile int32_t *)0xFF802960)                  /* 1 recording */
#define TAPE_FD        (*(volatile int32_t *)0x01B4EF44)                  /* tape_c.raw, the cache's file */
#define INVALIDATE     ((void (*)(uint32_t, uint32_t))0x01A540C0)          /* tape_cache_invalidate_clean_range(from, to) */
#define CACHE_HEAD     (*(uint8_t *volatile *)0x01B4EF3C)                 /* cache nodes, +2 flush request, +0x14 next */
#define DISK_ACTIVE    (*(volatile uint8_t *)0xFF906EF9)                  /* disk_mode_active */
#define NODE_DATA      0x10
#define NODE_NEXT      0x14
static inline uint32_t tape_now(void) { uint32_t c; __asm__ volatile("%0 = CYCLES;" : "=d"(c)); return c; }
#define NOW()          tape_now()                                         /* CYCLES: 400 per us at 400 MHz */
#endif
#define TRACK_BYTES    0x1E50000                                          /* 0xF28000 samples of 2 bytes */
#define TRACK_SAMPLES  0xF28000
#define KN_TAPE_MAX 16

/* shared tape data (op1_feature_tape.h) */
int32_t tm_pos[OP1_TAPE_MARKERS];
int tm_n;
volatile uint8_t pu_mode;
uint8_t ci_beats = 4;
static const op1_countin_t *countin_provider;
void op1_countin_register(const op1_countin_t *p) { if (!countin_provider) countin_provider = p; }
int op1_countin_start(void) { return countin_provider ? countin_provider->start() : 0; }
int op1_countin_pending(void) { return countin_provider ? countin_provider->pending() : 0; }
void op1_countin_cancel(void) { if (countin_provider) countin_provider->cancel(); }

void *tm_scr;                                         /* the stock tape screen object, once drawn (dispatcher.c) */

static const struct op1_tape_hooks *hooks[KN_TAPE_MAX];
static int n_hooks;
static void (*metro_owner)(void (*render)(void));
static void (*erase_owner)(void);
static void (*pre_draw[KN_TAPE_MAX])(void *scr);
static int n_pre;
static void (*obs_down[KN_TAPE_MAX])(const op1_note *n, int param);
static void (*obs_up[KN_TAPE_MAX])(const op1_note *n);
static int n_obs_down, n_obs_up;
volatile int kn_note_obs;                             /* hook.S: 0 = no observer, key_down / key_up run stock */
void *(*volatile kn_lookup_owner)(int32_t pos, int32_t *dir, void *(*stock)(int32_t, int32_t *));   /* hook.S */

/* end of the loader (kn_features_finish): the registry is in priority order */
void kn_tape_finish(void)
{
    extern int kn_feature_count(void);
    extern const op1_feature_t *kn_feature_at(int i);
    int n = 0;
    for (int i = 0; i < kn_feature_count() && n < KN_TAPE_MAX; i++) {
        const op1_note_input *ni = kn_feature_at(i)->note_in;    /* note input observers (any feature) */
        if (ni && ni->down && n_obs_down < KN_TAPE_MAX) obs_down[n_obs_down++] = ni->down;
        if (ni && ni->up && n_obs_up < KN_TAPE_MAX) obs_up[n_obs_up++] = ni->up;
        const struct op1_tape_hooks *t = kn_feature_at(i)->tape;
        if (!t) continue;
        hooks[n++] = t;
        if (t->metro && !metro_owner) metro_owner = t->metro;
        if (t->erase && !erase_owner) erase_owner = t->erase;
        if (t->pre_draw) pre_draw[n_pre++] = t->pre_draw;
        if (t->lookup && !kn_lookup_owner) kn_lookup_owner = t->lookup;
    }
    n_hooks = n;                                      /* last: the audio thread may already be running */
    int stages = 0;                                   /* pipeline stages read op1_note_source too */
    for (int i = 0; i < kn_feature_count(); i++) stages += kn_feature_at(i)->note_on != 0;
    kn_note_obs = n_obs_down + n_obs_up + stages;
}

#ifdef OP1_HOST
#define fr_capture()                                  /* host tests of this file: no FX2 routing linked */
#else
void fr_capture(void);                                /* FX2 routing (screens/fxroute.c): the instrument part */
#endif
void ci_metro_render(void)
{
    fr_capture();                                     /* first: tape L / R hold tape + instrument, no click yet */
    kn_features_tick();                               /* features' clock tick (kn_core.c) */
    for (int i = 0; i < n_hooks; i++)
        if (hooks[i]->block) hooks[i]->block();
    if (metro_owner) metro_owner(STOCK_METRO); else STOCK_METRO();
}

/* ---- tape edit events (OP1_EV_TAPE_EDIT) ---- */
volatile int kn_drain_pending;
/* one record per source, so an edit on one thread never overwrites another's: the clipboard queue (main loop),
 * split / join (KeyHandler), record end (whichever thread stops the recording) */
static op1_tape_edit_t edit_main, edit_key, edit_rec;
static op1_tape_edit_t *record_of(int op)
{
    return op == OP1_TAPE_SPLIT || op == OP1_TAPE_JOIN ? &edit_key : op == OP1_TAPE_REC ? &edit_rec : &edit_main;
}
const op1_tape_edit_t *op1_tape_edit_now(int32_t arg) { return record_of(OP1_TAPE_EDIT_OP(arg)); }
static void edit_event(int op, int track, int before, int32_t start, int32_t stop, int32_t clip, int slide)
{
    op1_tape_edit_t *e = record_of(op);
    if (!before) kn_drain_pending = 1;                /* OP1_EV_TAPE_DRAINED once it is all in the file */
    e->op = (uint8_t)op; e->track = (uint8_t)track; e->before = (uint8_t)before;
    e->slide = (uint8_t)slide; e->start = start; e->stop = stop; e->clip = clip;
    kn_event(OP1_EV_TAPE_EDIT, op | (track & 0xF) << 8 | (before ? 1 << 16 : 0));
}

/* clipboard commands: the before event at the command's first change, the after event once it is done (at the next
 * command's before, or when the service returns). Main loop. */
static volatile int in_clip_event;                    /* a clipboard command's event runs (its queue entry is still in) */
int op1_tape_flush(void);
static op1_tape_edit_t clip_pending;
static int clip_has_pending;
static void clip_after(void)
{
    if (!clip_has_pending) return;
    clip_has_pending = 0;
    op1_tape_edit_t e = clip_pending;
    in_clip_event = 1;
    edit_event(e.op, e.track, 0, e.start, e.stop, e.clip, e.slide);
    in_clip_event = 0;
}
static void clip_before(int op, int track, int32_t start, int32_t stop, int32_t clip, int slide)
{
    op1_tape_flush();                                 /* the file is current: a plain read in the event sees it all */
    clip_after();
    in_clip_event = 1;
    edit_event(op, track, 1, start, stop, clip, slide);
    in_clip_event = 0;
    clip_pending = edit_main; clip_pending.before = 0;
    clip_has_pending = 1;
}
static volatile int32_t *clip_entry(int back) { return &CLIP_Q[((CLIP_CONS - back) & 0xF) * 8]; }

int kn_clip_service(void)                             /* CALL audio_clipboard_service at 0x019F0A2A */
{
    if (CLIP_CONS != CLIP_PROD && op1_tape_flush() < 0) return 0; /* queued edits wait for unsaved audio */
    int r = CLIP_SERVICE();
    clip_after();
    return r;
}
int kn_clip_drop_add(void *pool, void *clip)          /* command 0, CALL tape_clips_add at 0x0191B65A */
{
    volatile int32_t *e = clip_entry(0);              /* still the head: the service pops it after the drop */
    clip_before(OP1_TAPE_DROP, e[1] & 3, e[2], e[2] + CLIP_LEN, -1, (uint8_t)e[7]);
    return CLIPS_ADD(pool, clip);
}
int kn_clip_dropall_add(void *pool, void *clip)       /* command 1, first CALL tape_clips_add at 0x0191B730 */
{
    volatile int32_t *e = clip_entry(0);
    clip_before(OP1_TAPE_DROP_ALL, OP1_TAPE_ALL, e[2], e[2] + CLIP_LEN, -1, 0);
    return CLIPS_ADD(pool, clip);
}
int kn_clip_lift(int ch, int32_t start, int32_t stop)   /* command 2, CALL lift at 0x0191B89C (popped already) */
{
    clip_before(OP1_TAPE_LIFT, ch & 3, start, stop, clip_entry(1)[6], 0);
    return CLIP_LIFT(ch, start, stop);
}
int kn_clip_lift_all(int32_t start, int32_t stop, void *hdr)   /* command 3, CALL lift all at 0x0191B974 */
{
    clip_before(OP1_TAPE_LIFT_ALL, OP1_TAPE_ALL, start, stop, -1, 0);
    return CLIP_LIFT_ALL(start, stop, hdr);
}

/* split / join: KeyHandler thread, before and after around the stock call */
int kn_tape_split(int track, int32_t pos, int flag)
{
    edit_event(OP1_TAPE_SPLIT, track, 1, pos, pos, -1, 0);
    int r = CLIPS_SPLIT(track, pos, flag);
    edit_event(OP1_TAPE_SPLIT, track, 0, pos, pos, -1, 0);
    return r;
}
int kn_tape_join(int track, int32_t pos)
{
    edit_event(OP1_TAPE_JOIN, track, 1, pos, pos, -1, 0);
    int r = CLIPS_JOIN(track, pos);
    edit_event(OP1_TAPE_JOIN, track, 0, pos, pos, -1, 0);
    return r;
}

/* record start: set_mode's CALL tape_record_extent_reset at 0x01A584D0 runs only when the tape goes into recording,
 * before it sets the run state the audio thread records on: REC before, track and position (start = stop) */
void kn_rec_start(void)
{
    int32_t pos = (int32_t)(TAPE_POS_Q6 >> 6);
    edit_event(OP1_TAPE_REC, TAPE_TRACK & 3, 1, pos, pos, -1, 0);
    EXTENT_RESET();
}

/* record end: tape_controller_set_mode stops the recording, builds the clip from the recorded extent (minimum length
 * and the tape end applied: {channel, start, stop}), adds it and resets the extent at once, so the range comes from
 * the clip. REC after, once the clip row is in. A recording that wrote nothing gets no clip and no after event. */
int kn_rec_clip_add(void *pool, const int32_t *clip)
{
    int r = CLIPS_ADD(pool, (void *)clip);
    edit_event(OP1_TAPE_REC, clip[0] & 3, 0, clip[1], clip[2], -1, 0);
    return r;
}

/* ---- held saves while recording (op1r 49's rule, kennel's own code) ----
 * R246's main loop runs tape_c::service(0) on every pass (~80 a second while recording). Its first node with the flush
 * request set is the one being recorded into, so the same 64 KiB chunk went to flash on every pass and keys, sequencers
 * and chunk loads waited behind the writes. While recording, a node saved less than HOLD_CYCLES ago now waits:
 * kn_tape_service clears its flush request around the stock step and sets it again after, so R246's own step (and our
 * writeback hook, the undo history's old bytes, the bounce buffer) still does every write. A node is held only while
 * recording (run 1), while it is loaded (+0 valid) and among the HOLD_DEPTH most recently used nodes (eviction takes
 * the tail of 24); a node never saved before is saved at once. When recording stops, everything is flushed in that
 * pass; flushes (disk mode, tape switch, edits, undo, dev restart) and power-off never hold. A sudden power loss while
 * recording loses at most the last HOLD_CYCLES of the take (the chunk's previous save is in the file). */
#define HOLD_CYCLES  400000000u                       /* 1 s at 400 MHz, as op1r 49 */
#define HOLD_DEPTH   12                               /* the 12 most recently used nodes, as op1r 49 */
#define HOLD_SLOTS   32                               /* R246 has 24 nodes */
static struct { const uint8_t *node; uint32_t saved; } hold_slot[HOLD_SLOTS];
volatile uint32_t kn_tape_held;                       /* nodes held back (counted per pass), for tests */
static void hold_note_save(const uint8_t *node)       /* a write of this node starts now (kn_tape_write) */
{
    int free = -1;
    for (int i = 0; i < HOLD_SLOTS; i++) {
        if (hold_slot[i].node == node) { hold_slot[i].saved = NOW(); return; }
        if (!hold_slot[i].node && free < 0) free = i;
    }
    if (free >= 0) { hold_slot[free].node = node; hold_slot[free].saved = NOW(); }
}
static int hold_recent(const uint8_t *node, uint32_t now)   /* saved less than HOLD_CYCLES ago (the difference survives
                                                               CYCLES' wrap; a wrapped one holds at most HOLD_CYCLES) */
{
    for (int i = 0; i < HOLD_SLOTS; i++)
        if (hold_slot[i].node == node) return now - hold_slot[i].saved < HOLD_CYCLES;
    return 0;                                         /* never saved: now */
}

/* ---- writeback: the tape cache's CALL yaffs_write at 0x01A54842 (main loop) ---- */
static op1_tape_write_t wb;
static int wb_fd, wb_open, wb_moved, write_busy;
const op1_tape_write_t *op1_tape_write_now(void) { return &wb; }

int op1_tape_read_old(void *dst, int32_t at, int32_t len)
{
    if (!wb_open || !dst || at < 0 || len < 0 || len > wb.len || at > wb.len - len) return -1;
    if (!len) return 0;
    wb_moved = 1; /* even a failed seek must not leave the write at an unknown position */
    if (LSEEK(wb_fd, wb.offset + at, 0) != wb.offset + at) return -1;
    int done = 0;
    for (unsigned calls = 0; done < len && calls < 32; calls++) {
        int n = READ_FILE(wb_fd, (uint8_t *)dst + done, len - done);
        if (n <= 0 || n > len - done) return -1;
        done += n;
    }
    return done == len ? len : -1;
}

/* TE's bad block bug (R246): R246 writes a cache node straight from the node's buffer while the
 * record node (audio thread) keeps filling it. YAFFS writes each 2 KiB chunk from the caller's buffer and then reads
 * it back and compares (yaffs_write_new_chunk, FUN_01A91556: memcmp at 0x01A9173E); a buffer that changed in between
 * fails the compare, and as the chunk had just passed the erased check the block is retired for good (needs-retiring
 * set at 0x01A91858). The fix: write from a stable copy. What the audio thread changes after the copy marks the node
 * modified again, so it is written on a later pass; a real write failure still fails the compare and is retired. */
#ifndef KN_NO_STABLE_WRITE
/* Core .farbss, reserved at link time: recording safety must not depend on plugin arena space.
 * Keep the full64KiB snapshot so undo still sees one complete write event. */
static uint32_t stable[0x10000 / 4] kn_farbss;
static const void *stable_copy(const void *buf, int len)
{
    if (!buf || len < 0 || len > (int)sizeof stable) return 0;
    const uint8_t *s = buf;
    unsigned n = (unsigned)len;
    unsigned i = 0;
    if (!((uintptr_t)buf & 3)) {
        for (; i + 4 <= n; i += 4) stable[i >> 2] = *(const uint32_t *)(s + i);
    }
    for (; i < n; i++) ((uint8_t *)stable)[i] = s[i];
    return stable;
}
#else
#define stable_copy(buf, len) (buf)                   /* the stock path, for the comparison runs */
#endif

/* R246 clears dirty flags before calling us and ignores the write result. Match
 * the cache source to its node so failures stay dirty and can be retried. */
static void redirty(uint8_t *node, int track, int chunk)
{
    if (!node) return;
#ifndef OP1_HOST
    uint32_t mask; __asm__ volatile("cli %0;" : "=d"(mask) : : "memory");
#endif
    if (*(int16_t *)(node + 6) == chunk) {
        *(volatile uint16_t *)(node + 4) |= (uint16_t)(1u << track);
        node[2] = 1;
    }
#ifndef OP1_HOST
    __asm__ volatile("sti %0;" : : "d"(mask) : "memory");
#endif
}
int kn_tape_write(int fd, const void *buf, int len)
{
    if (!len) return 0;
    if (!buf || len < 0 || len > 0x10000) return -1;
    if (write_busy) return -1; /* a callback must not overwrite the active stable snapshot */
    uint8_t *owner = 0; int owner_track = -1, chunk = -1;
    unsigned nodes = 0;
    for (uint8_t *node = CACHE_HEAD, *next; node && nodes++ < 64; node = next) {
        /* Bounded separately below: a stock cache has24 nodes. */
        next = *(uint8_t **)(node + NODE_NEXT);
        uintptr_t data = (uintptr_t)*(uint8_t **)(node + NODE_DATA);
        uintptr_t src = (uintptr_t)buf;
        if (data && src >= data && src - data < 4u * 0x10000 && !((src - data) & 0xffff)) {
            owner = node; owner_track = (int)((src - data) >> 16); chunk = *(int16_t *)(node + 6); break;
        }
        if (next == CACHE_HEAD) break;
    }
    if (owner) hold_note_save(owner);                 /* a failed write too: retried after the hold, not every pass */
    const void *copy = stable_copy(buf, len);
    if (!copy) { redirty(owner, owner_track, chunk); return -1; }
    write_busy = 1;
    int32_t off = owner && fd == TAPE_FD && chunk >= 0 && chunk < 485 ?
        TRACK_BASE[owner_track] + chunk * 0x10000 : LSEEK(fd, 0, 1);
    if (off < 0 || LSEEK(fd, off, 0) != off) goto fail;
    for (int t = 0; t < 4; t++) {
        int32_t rel = off - TRACK_BASE[t];
        if (rel < 0 || rel >= TRACK_BYTES) continue;
        if (len < 0 || len > TRACK_BYTES - rel) goto fail;
        wb.track = (uint8_t)t; wb.chunk = rel >> 16; wb.offset = off; wb.len = len; wb.data = copy;
        wb_fd = fd; wb_moved = 0; wb_open = 1;
        kn_event(OP1_EV_TAPE_WRITE, t | wb.chunk << 8);
        wb_open = 0;
        if (wb_moved && LSEEK(fd, off, 0) != off) goto fail;
        break;
    }
    int done = 0;
    for (unsigned calls = 0; done < len && calls < 32; calls++) {
        int n = WRITE_FILE(fd, (const uint8_t *)copy + done, len - done);
        if (n <= 0 || n > len - done) goto fail;
        done += n;
    }
    if (done != len) goto fail;
    write_busy = 0;
    return done;
fail:
    wb_open = 0; write_busy = 0;
    redirty(owner, owner_track, chunk);
    return -1;
}

/* ---- drained, flush (main loop) ---- */
static int cache_dirty(void)
{
    int n = 0;
    for (uint8_t *node = CACHE_HEAD; node && n < 64; node = *(uint8_t **)(node + NODE_NEXT), n++)
        if (node[2]) return 1;
    return 0;
}

int op1_tape_flush(void)                              /* every modified chunk to the file now (and invalid ones loaded) */
{
    if (write_busy) return -1;
    int n = 0;
    while (n < 96 && STOCK_SERVICE(0)) n++;           /* one node per call; 24 nodes, so this ends unless recording */
    return cache_dirty() ? -1 : n;
}

/* ci_service's tape_c::service(0): R246's step with the held nodes out of it (see "held saves" above) */
static uint8_t was_recording;
static int kn_tape_service(int arg)
{
    if (TAPE_RUN != 1 || arg) {
        if (was_recording) {                          /* the take ended: every held chunk to the file in this pass */
            was_recording = 0;
            return op1_tape_flush() != 0;
        }
        return STOCK_SERVICE(arg);
    }
    was_recording = 1;
    uint8_t *held[HOLD_DEPTH];
    int n = 0, depth = 0;
    uint32_t now = NOW();
    for (uint8_t *node = CACHE_HEAD; node && depth < HOLD_DEPTH; node = *(uint8_t **)(node + NODE_NEXT), depth++) {
        if (!node[2] || !node[0] || !hold_recent(node, now)) continue;
#ifndef OP1_HOST
        uint32_t mask; __asm__ volatile("cli %0;" : "=d"(mask) : : "memory");
#endif
        node[2] = 0;                                  /* only the main loop clears it; the record node only sets it */
#ifndef OP1_HOST
        __asm__ volatile("sti %0;" : : "d"(mask) : "memory");
#endif
        held[n++] = node;
    }
    int r = STOCK_SERVICE(0);
    for (int i = 0; i < n; i++) {                     /* dirty_tracks were left alone: the next save writes them */
#ifndef OP1_HOST
        uint32_t mask; __asm__ volatile("cli %0;" : "=d"(mask) : : "memory");
#endif
        held[i][2] = 1;
#ifndef OP1_HOST
        __asm__ volatile("sti %0;" : : "d"(mask) : "memory");
#endif
    }
    kn_tape_held += (uint32_t)n;
    return r;
}

/* power-off and restart: R246's shutdown runs CALL tape_c::service(1) at 0x019F0BE2 until it returns 0 (after the
 * BgFileWorker stop, before the unmount, hook.S hook_tapefinal). Stock's forced step stops at the first unmodified
 * node in use order, which held chunks can sit behind, and with our writeback hook re-marking a failed write it would
 * never return 0 on a flash that refuses writes. One flush instead: every modified node (at most 96 steps), then 0. */
int kn_tape_final(int arg)
{
    (void)arg;
    was_recording = 0;
    op1_tape_flush();
    return 0;
}

/* ---- range read and restore (main loop): through the file, as R246's own lift and drop ---- */
static int file_busy(void) { return TAPE_RUN == 1 || (CLIP_CONS != CLIP_PROD && !in_clip_event); }
static int range_ok(int track, int32_t at, int32_t len)
{
    return track >= 0 && track < 4 && at >= 0 && len >= 0 && at <= TRACK_SAMPLES - len;
}

int op1_tape_read(int track, int32_t at, int32_t len, int16_t *dst)
{
    if (!range_ok(track, at, len) || !dst || file_busy()) return -1;
    if (op1_tape_flush() < 0) return -1;               /* modified chunks in RAM reach the file first */
    int32_t offset = TRACK_BASE[track] + at * 2;
    if (LSEEK(TAPE_FD, offset, 0) != offset) return -1;
    return READ_FILE(TAPE_FD, dst, len * 2) == len * 2 ? len : -1;
}

int op1_tape_restore(int track, int32_t at, int32_t len, const int16_t *data)
{
    if (!range_ok(track, at, len) || !data || file_busy()) return -1;
    if (op1_tape_flush() < 0) return -1;               /* every node clean, so the invalidation below reaches them all */
    int32_t offset = TRACK_BASE[track] + at * 2;
    if (LSEEK(TAPE_FD, offset, 0) != offset) return -1;
    int n = WRITE_FILE(TAPE_FD, data, len * 2);
    INVALIDATE((uint32_t)at, (uint32_t)(at + len));   /* cached chunks reload, loop seams rebuilt (R246's drop does it) */
    return n == len * 2 ? len : -1;
}

/* COM's disk mode switch (KeyHandler): before the host can read the tape file, the main loop flushes the cache (the
 * main loop owns the file position; flushing here could interleave with its writes). Waits up to 3 s. */
static volatile int flush_req, flush_failed;
int kn_disk_toggle(int arg)
{
    if (!DISK_ACTIVE) {
#ifndef OP1_HOST
        extern int bgs_drain(int ms);
        bgs_drain(3000);                              /* delayed patch saves (bgsave.c): queued and written first */
#endif
        flush_failed = 0; flush_req = 1;
        for (int i = 0; i < 300 && flush_req; i++) DELAY_MS(10);
        if (flush_req || flush_failed) { flush_req = 0; op1_toast("tape still writing"); return -1; }
    }
    return DISK_TOGGLE(arg);
}

int ci_service(int arg)
{
    int r = kn_tape_service(arg);
    if (flush_req) {                                  /* disk mode is starting (kn_disk_toggle waits for this) */
        flush_failed = op1_tape_flush() < 0;
        if (flush_req && !flush_failed) kn_event(OP1_EV_DISK, 1);
        flush_req = 0;
    }
    static int disk_was;                              /* ... and has ended */
    if (disk_was && !DISK_ACTIVE) kn_event(OP1_EV_DISK, 0);
    disk_was = DISK_ACTIVE;
    if (kn_drain_pending && TAPE_RUN != 1 && CLIP_CONS == CLIP_PROD && !cache_dirty()) {
        kn_drain_pending = 0;
        kn_event(OP1_EV_TAPE_DRAINED, 0);
    }
#ifndef OP1_HOST
    extern void kn_tapes_service(void) __attribute__((weak));
    if (kn_tapes_service) kn_tapes_service();         /* four tapes: a requested switch (kn_tapes.c) */
#endif
    for (int i = 0; i < n_hooks; i++)
        if (hooks[i]->service) hooks[i]->service();
    kn_core_service();                                /* events, crash marker, toast timer (kn_core.c) */
#ifndef OP1_HOST
    extern void bgs_service(void);
    bgs_service();                                    /* delayed patch saves after a quiet 1.5 s (bgsave.c) */
    extern void kn_boot_service(void) __attribute__((weak));
    if (kn_boot_service) kn_boot_service();           /* kennel loader: crash marker (kennel/kn_boot.c) */
#endif
    extern void du_service(void) __attribute__((weak));
    if (du_service) du_service();                     /* dev update over SysEx (--dev, docs/35) */
    return r;
}

void tm_screen_draw(void *scr, int dt)
{
    STOCK_DRAW(scr, dt);
    tm_scr = scr;
    for (int i = 0; i < n_hooks; i++)
        if (hooks[i]->draw) hooks[i]->draw();
}

void fast_tape_erase(void)
{
    clip_after();                                     /* a command before it in the same queue pass */
    if (op1_tape_flush() < 0) return;                 /* keep unsaved cache data */
    in_clip_event = 1;
    edit_event(OP1_TAPE_ERASE, OP1_TAPE_ALL, 1, 0, 0x7FFFFFFF, -1, 0);
    in_clip_event = 0;
    if (erase_owner) erase_owner(); else STOCK_ERASE();
    edit_event(OP1_TAPE_ERASE, OP1_TAPE_ALL, 0, 0, 0x7FFFFFFF, -1, 0);
}

/* tape screen draw, CALL 0x01A5C728 at 0x01A5CFA8 */
void kn_tape_pre(void *scr)
{
    STOCK_PRE(scr);
    for (int i = 0; i < n_pre; i++) pre_draw[i](scr);
}

/* VoiceCtl key_down / key_up entries (hook.S stubs, only while kn_note_obs != 0): any thread that plays a note */
void kn_note_source_set(uint32_t ret);                  /* kn_midi.c */
void kn_note_input_down(const op1_note *n, int param, uint32_t ret)
{
#ifndef OP1_HOST
    kn_note_source_set(ret);
#else
    (void)ret;
#endif
    for (int i = 0; i < n_obs_down; i++) obs_down[i](n, param);
}
void kn_note_input_up(const op1_note *n, uint32_t ret)
{
#ifndef OP1_HOST
    kn_note_source_set(ret);
#else
    (void)ret;
#endif
    for (int i = 0; i < n_obs_up; i++) obs_up[i](n);
}
void op1_key_down(const op1_note *n, int param) { KEY_DOWN(n, param); }
void op1_key_up(const op1_note *n) { KEY_UP(n); }

/* tape_c chunk lookup (hook.S stub, only while kn_lookup_owner != 0; L1 tape tick, audio thread) */
void *kn_tape_lookup(int32_t pos, int32_t *dir)
{
    return kn_lookup_owner ? kn_lookup_owner(pos, dir, LOOKUP_ORIG) : LOOKUP_ORIG(pos, dir);
}

int kn_tape_key(int code, int pressed, int tape_screen)
{
    for (int i = 0; i < n_hooks; i++)
        if (hooks[i]->key && hooks[i]->key(code, pressed, tape_screen)) return 1;
    return 0;
}

int kn_tape_knob(int knob, int delta, int tape_screen)
{
    for (int i = 0; i < n_hooks; i++)
        if (hooks[i]->knob && hooks[i]->knob(knob, delta, tape_screen)) return 1;
    return 0;
}
