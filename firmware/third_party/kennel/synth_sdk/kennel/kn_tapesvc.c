/*
 * Tape services for features (op1_feature_tape.h): loop points, selected track, playhead, transport state, and R246's
 * audio clipboard (what lift fills and drop reads), so a feature can build tape edits the way R246's own lift and drop
 * work (Maiko #250's tape edits, rebuilt in undo.c).
 *
 * R246 (OP1-Emulator docs/firmware/tape.md):
 * - Loop: tape_get_loop_in 0x01A53DA0 / _out 0x01A53D88, tape_set_loop_in 0x01A550A8 / _out 0x01A55100 (0..0xF23FA0;
 *   out is stored clamped to 0xF23F90; both drop the loop seams), loop on 0xFF8029C5 and loop effective 0xFF8029C6.
 *   The loop keys (0x2D / 0x2E in 0x01A589A8) set a point, then on = effective = (in != out), mark the tape controller
 *   dirty (bg_mark_dirty 0x0193EF3C on tape_controller_instance 0x01A56D94 + 8: the tape row is saved by the
 *   background saver) and ask the tape screen to redraw (0x01B6039C = 1). loop_set does the same.
 * - Selected track 0xFF802964, playhead 0xFF802974 (1/64 samples), tape_locate 0x01A55024 (samples 0..0xF23FA0; it
 *   also clears loop effective, so a loop is suspended until the playhead enters it again, as with R246's own locates).
 * - Transport: tape state 0x018C17D4 (9 / 10 playing, 4 / 5 recording), run state 0xFF802960 (1 recording).
 * - Clipboard: four slot pointers at 0x02040000, samples from 0x02040010 (shared with the sampler editors); channel
 *   0x01AA7100 (-1 = all four), length 0x01AA7104 (samples), content 0x01AA7108 (0 empty, 1 tape lift, 4 / 5 synth /
 *   drum sample). A lift of one track (clipboard command 2) zeroes the four pointers and points that track's at the
 *   buffer (length below 0x3C8FE8, 90 s); a lift of all four (command 3) points them B, B + 0x1E47F4, B + 0x3C8FE8,
 *   B + 0x5AD7DC bytes (length up to 0xF23F9 each, 22.5 s). Drop (commands 0 / 1) reads them from there.
 *   The enqueue side keeps its own copy, set when a lift is queued (audio_clipboard_enqueue_lift 0x0191BBD0 / _all
 *   0x0191BD64): channel 0x01AA710C (-1 all; the drop key picks drop / drop-all by it), length 0x01AA7110 (0 = the
 *   drop key does nothing) and 0x01AA7114 = 1.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_feature_tape.h"

#ifdef OP1_HOST                                         /* harness/tapesvc_test.c */
typedef struct {
    int32_t loop_in, loop_out, track, pos_q6, state, run, clip_cons, clip_prod, clip_ch, clip_len, clip_kind;
    uint8_t loop_on, loop_eff, redraw, dirty;
    int32_t located, q_ch, q_len, q_has;
    int16_t *slot[4];
    int16_t buf[0x3C8FE8 + 64];
} host_tsvc_t;
extern host_tsvc_t host_tsvc;
static int32_t h_get_in(void) { return host_tsvc.loop_in; }
static int32_t h_get_out(void) { return host_tsvc.loop_out; }
static void h_set_in(int32_t v) { host_tsvc.loop_in = v; }
static void h_set_out(int32_t v) { host_tsvc.loop_out = v > 0xF23F90 ? 0xF23F90 : v; }
static void h_locate(int32_t v) { host_tsvc.located = v; host_tsvc.loop_eff = 0; }
static void h_dirty(void) { host_tsvc.dirty = 1; }
#define GET_LOOP_IN   h_get_in
#define GET_LOOP_OUT  h_get_out
#define SET_LOOP_IN   h_set_in
#define SET_LOOP_OUT  h_set_out
#define LOCATE        h_locate
#define MARK_DIRTY()  h_dirty()
#define LOOP_ON       host_tsvc.loop_on
#define LOOP_EFF      host_tsvc.loop_eff
#define REDRAW        host_tsvc.redraw
#define CUR_TRACK     host_tsvc.track
#define TAPE_POS_Q6   host_tsvc.pos_q6
#define TAPE_STATE    host_tsvc.state
#define TAPE_RUN      host_tsvc.run
#define CLIP_CONS     host_tsvc.clip_cons
#define CLIP_PROD     host_tsvc.clip_prod
#define CB_SLOT       host_tsvc.slot
#define CB_BUF        host_tsvc.buf
#define CB_CHANNEL    host_tsvc.clip_ch
#define CB_LEN        host_tsvc.clip_len
#define CB_KIND       host_tsvc.clip_kind
#define Q_CH          host_tsvc.q_ch
#define Q_LEN         host_tsvc.q_len
#define Q_HAS         host_tsvc.q_has
#else
#define GET_LOOP_IN   ((int32_t (*)(void))0x01A53DA0)
#define GET_LOOP_OUT  ((int32_t (*)(void))0x01A53D88)
#define SET_LOOP_IN   ((void (*)(int32_t))0x01A550A8)
#define SET_LOOP_OUT  ((void (*)(int32_t))0x01A55100)
#define LOCATE        ((void (*)(int32_t))0x01A55024)
#define MARK_DIRTY()  (((void (*)(void *))0x0193EF3C)((uint8_t *)((void *(*)(void))0x01A56D94)() + 8))
#define LOOP_ON       (*(volatile uint8_t *)0xFF8029C5)
#define LOOP_EFF      (*(volatile uint8_t *)0xFF8029C6)
#define REDRAW        (*(volatile uint8_t *)0x01B6039C)
#define CUR_TRACK     (*(volatile int32_t *)0xFF802964)
#define TAPE_POS_Q6   (*(volatile int32_t *)0xFF802974)
#define TAPE_STATE    (*(volatile int32_t *)0x018C17D4)
#define TAPE_RUN      (*(volatile int32_t *)0xFF802960)
#define CLIP_CONS     (*(volatile int32_t *)0x01AA7330)
#define CLIP_PROD     (*(volatile int32_t *)0x01AA7334)
#define CB_SLOT       ((int16_t *volatile *)0x02040000)
#define CB_BUF        ((int16_t *)0x02040010)
#define CB_CHANNEL    (*(volatile int32_t *)0x01AA7100)
#define CB_LEN        (*(volatile int32_t *)0x01AA7104)
#define CB_KIND       (*(volatile int32_t *)0x01AA7108)
#define Q_CH          (*(volatile int32_t *)0x01AA710C)       /* the enqueue side's copy */
#define Q_LEN         (*(volatile int32_t *)0x01AA7110)
#define Q_HAS         (*(volatile int32_t *)0x01AA7114)
#endif
#define TAPE_END      0xF23FA0
#define CB_ONE_MAX    0x3C8FE7                  /* one track: R246 asserts length <= this */
#define CB_ALL_MAX    0xF23F9                   /* all four: per track */
#define CB_ALL_STEP   (0x1E47F4 / 2)            /* samples between the four slots */

/* ---- loop, track, playhead, transport ---- */
int op1_tape_loop_get(int *on, int32_t *in, int32_t *out)
{
    if (in) *in = GET_LOOP_IN();
    if (out) *out = GET_LOOP_OUT();
    if (on) *on = LOOP_ON != 0;
    return LOOP_ON != 0;
}

int op1_tape_loop_set(int on, int32_t in, int32_t out)
{
    if (in < 0 || out < in || out > TAPE_END) return -1;
    if (in > GET_LOOP_OUT()) { SET_LOOP_OUT(out); SET_LOOP_IN(in); }   /* never in > out on the way */
    else { SET_LOOP_IN(in); SET_LOOP_OUT(out); }
    LOOP_ON = LOOP_EFF = (uint8_t)(on && GET_LOOP_IN() != GET_LOOP_OUT());
    MARK_DIRTY();
    REDRAW = 1;
    return 0;
}

int op1_tape_track(void) { int t = CUR_TRACK; return t >= 0 && t < 4 ? t : 0; }
int32_t op1_tape_pos_get(void) { return TAPE_POS_Q6 >> 6; }
int op1_tape_pos_set(int32_t samples)
{
    if (samples < 0 || samples > TAPE_END) return -1;
    LOCATE(samples);
    return 0;
}

int op1_tape_transport(void)
{
    int s = TAPE_STATE;
    if (TAPE_RUN == 1 || s == 4 || s == 5) return OP1_TAPE_RECORDING;
    if (s == 9 || s == 10) return OP1_TAPE_PLAYING;
    return OP1_TAPE_STOPPED;
}

/* ---- clipboard (main loop) ---- */
int op1_clipboard_info(int *chmask, int32_t *len)
{
    int kind = CB_KIND, ch = CB_CHANNEL, mask = 0;
    if (kind) mask = ch < 0 ? 0xF : ch < 4 ? 1 << ch : 0;
    if (chmask) *chmask = mask;
    if (len) *len = kind ? CB_LEN : 0;
    return kind;
}

int16_t *op1_clipboard_slot(int ch)
{
    if (ch < 0 || ch > 3 || !CB_KIND) return 0;
    int c = CB_CHANNEL;
    if (c >= 0 && c != ch) return 0;
    return CB_SLOT[ch];
}

int op1_clipboard_read(int ch, int32_t at, int32_t len, int16_t *dst)
{
    const int16_t *s = op1_clipboard_slot(ch);
    if (!s || !dst || at < 0 || len < 0 || at > CB_LEN - len) return -1;
    s += at;
    for (int32_t i = 0; i < len; i++) dst[i] = s[i];
    return len;
}

int op1_clipboard_write(int chmask, int32_t len)
{
    if (CLIP_CONS != CLIP_PROD || len <= 0) return -1;
    int one = chmask == 1 || chmask == 2 || chmask == 4 || chmask == 8;
    if (!one && chmask != 0xF) return -1;
    if (len > (one ? CB_ONE_MAX : CB_ALL_MAX)) return -1;
    CB_KIND = 0;
    for (int t = 0; t < 4; t++) CB_SLOT[t] = one ? 0 : CB_BUF + t * CB_ALL_STEP;
    int ch = -1;
    if (one) { ch = chmask == 1 ? 0 : chmask == 2 ? 1 : chmask == 4 ? 2 : 3; CB_SLOT[ch] = CB_BUF; }
    CB_CHANNEL = ch;
    CB_LEN = len;
    CB_KIND = 1;                                    /* a tape lift's content: drop takes it as its own */
    Q_CH = ch; Q_LEN = len; Q_HAS = 1;              /* what the drop key looks at */
    return 0;
}
