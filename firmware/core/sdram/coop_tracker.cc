/*
 * coop-fw t1: the tracker sequencer (docs/tracker.md; docs/tracker-handover.md for the owner's decisions).
 *
 * The data: 16 patterns of up to 64 rows x 6 channels, an order list of up to 32 pattern numbers, the song's tempo,
 * speed (ticks a row), octave and orientation. A cell holds a note (0 none, 1..127 MIDI, 254 = note off), an
 * instrument (0 the synth, 1 the drum: the family the note goes to), a volume 0..99 (velocity) and an effect with a
 * value. Channels are monophonic: a new note chokes the previous one.
 *
 * Playing (the owner: the tracker plays live): notes go the way every stock sequencer's do, MakeMidiNote
 * (0x01A85014) then VoiceCtl::NoteOn (0x01A85750, 0) / NoteOff (0x01A85840), which dispatch on the bank word
 * [0x01DE4C40] (0 synth, 1 drum), set here around each call so a drum channel plays the kit while the synth is the
 * current instrument. The stock loads ONE synth engine at a time, so every synth channel plays the current synth
 * sound and every drum channel the current kit: the first pass's main limit (per-channel engines need the instance
 * work of the handover's F1-F3). The clock is counted in the audio block (coop_tracker_block: samples per tick from
 * the tempo, 4 rows a beat), the notes are dispatched by the main loop (coop_tracker_service from coop_fx_service):
 * steady timing, no stock calls from the interrupt.
 *
 * Effects (a word and a value, per cell, at tick resolution): ARP x y (the note, +x, +y semitones, a tick each),
 * RTG n (retrigger every n ticks), CUT n (note off at tick n), DLY n (note on at tick n), VOL v (channel volume
 * 0..99 from this row), PRB p (the note plays with probability p %). Slides need per-voice pitch control the stock
 * does not expose to a sequencer: not in v1.
 *
 * Storage: /yaffs2/user/coop_song.bin ("CSNG" 1), written by the main loop two seconds after the last edit (and on
 * leaving the tracker), read after the tape mounts. Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_tracker.h"

#define BANK          (*(volatile int *)0x01DE4C40u)                     /* 0 synth, 1 drum: the UI's play-mode request; NoteOn dispatches on it */
/* MakeMidiNote 0x01A85014 adds 53 and the synth's octave to a key: coop fills the note itself (absolute MIDI) */
#define NOTE_ON       ((void (*)(void *, int))0x01A85750u)                /* VoiceCtl::NoteOn(&n, 0) */
#define NOTE_OFF      ((void (*)(void *))0x01A85840u)                     /* VoiceCtl::NoteOff(&n) */
#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE       ((int (*)(int))0x01A99908u)
#define Y_READ        ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE       ((int (*)(int, const void *, int))0x01A99F84u)
#define SONG_FILE     "/yaffs2/user/coop_song.bin"
#define SAVE_DELAY_BLOCKS 1400                                            /* two seconds of audio blocks after the last edit */

struct trk_song g_song;                                                   /* the song (coop_tracker.h) */
struct trk_state g_trk;                                                   /* the transport, cursor and views */
volatile uint32_t g_trk_ticks;                                            /* ticks counted by the audio block */
static uint32_t s_ticks_done;                                             /* ticks the main loop has played */
static int32_t s_tick_acc;                                                /* samples toward the next tick */
static volatile int s_dirty, s_save_pending;
static int32_t s_dirty_age;
static uint32_t s_rng = 0x2545F491u;
static volatile uint32_t s_blocks;                                          /* audio blocks, counted in coop_tracker_block */
volatile uint32_t g_trk_rows, g_trk_notes;                                /* stats / tests */

/* a MidiNote as MakeMidiNote fills it: note, velocity, a flag, a word (16 bytes: the stock's layout, note at +0) */
struct midi_note { int32_t note; int32_t velocity; uint8_t on; uint8_t pad[3]; int32_t extra; };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* ---- the voices (main loop) ---- */
/* The core has ONE live play mode, [0xFF801038] (0 synth, 1 drum): its voice loop renders that family, and the note
 * paths assert on the other one ("VoiceCtl.cpp:387 core_playMode == shared::PmDrum", found in op1emu when t1 flipped
 * the request word per note). So a note goes out only when its family is the live one; the rest are skipped. Both
 * families at once need the handover's engine-instance work (F1-F3). */
#define LIVE_MODE     (*(volatile int *)0xFF801038u)
extern "C" int coop_tracker_live_family(void) { return LIVE_MODE == 1 ? 1 : 0; }
extern "C" void coop_xvoice_note(int slot, int eng, int note, int vel, int on);   /* coop_xvoice.cc (F1 / F2): coop's extra voices, inst >= 2 */
extern "C" void coop_xvoice_boot(void);
extern "C" void coop_xvoice_set(int slot, int eng, const uint8_t *knob_pct, int pan_pct);   /* the slot's knob words and pan */
static void raw_note(int c, int inst, int note, int vel, int on)
{
    if (inst >= 2) {                                                        /* F2: the channel's own voice, outside the core */
        if (on) coop_xvoice_set(c, inst - 2, g_song.ch_knob[c], g_song.ch_pan[c]);   /* its patch: the channel's knobs and pan */
        coop_xvoice_note(c, inst - 2, note, vel, on);
        return;
    }
    if ((inst ? 1 : 0) != coop_tracker_live_family() || BANK != LIVE_MODE) return;   /* the other family, or a switch under way */
    struct midi_note n;
    n.note = note; n.velocity = vel; n.on = 1; n.pad[0] = n.pad[1] = n.pad[2] = 0; n.extra = 0;
    if (on) NOTE_ON(&n, 0); else NOTE_OFF(&n);
}
static void ch_off(int c)
{
    struct trk_chan *ch = &g_trk.ch[c];
    if (ch->sounding) { raw_note(c, ch->inst_sounding, ch->note_sounding, 64, 0); ch->sounding = 0; }
}
static void ch_on(int c, int inst, int note, int vol)
{
    ch_off(c);
    int vel = clampi((vol * 127 + 49) / 99, 1, 127);
    raw_note(c, inst, note, vel, 1);
    g_trk_notes++;
    struct trk_chan *ch = &g_trk.ch[c];
    ch->sounding = 1; ch->note_sounding = note; ch->inst_sounding = inst;
}
extern "C" void coop_tracker_all_off(void) { for (int c = 0; c < TRK_CH; c++) ch_off(c); }

/* a preview from the keyboard (the cell's instrument): the key thread asks, the main loop plays */
static volatile uint32_t s_prev_ring[8];                                   /* ch << 20 | inst << 16 | note << 8 | on, 0 = empty */
static volatile int s_prev_wr, s_prev_rd;
extern "C" void coop_tracker_preview(int ch, int inst, int note, int on)
{
    int w = s_prev_wr;
    if (((w + 1) & 7) == s_prev_rd) return;                                 /* full: the tap is lost, nothing hangs */
    s_prev_ring[w] = 0x01000000u | ((uint32_t)(ch & 7) << 20) | ((uint32_t)(inst & 15) << 16) | ((uint32_t)(note & 127) << 8) | (uint32_t)(on ? 1 : 0);
    s_prev_wr = (w + 1) & 7;
}

/* ---- the song data ---- */
extern "C" struct trk_cell *trk_cell_at(int pat, int row, int ch) { return &g_song.pat[pat & 15][row & 63][ch]; }
extern "C" int trk_pat_len(int pat) { int n = g_song.pat_len[pat & 15]; return n < 1 ? 1 : (n > 64 ? 64 : n); }
extern "C" void coop_tracker_touch(void) { s_dirty = 1; s_dirty_age = 0; }

static void defaults(void)
{
    for (int p = 0; p < 16; p++) { g_song.pat_len[p] = 16; for (int r = 0; r < 64; r++) for (int c = 0; c < TRK_CH; c++) { struct trk_cell *x = &g_song.pat[p][r][c]; x->note = 0; x->inst = c >= 4 ? 1 : 0; x->vol = 80; x->fx = 0; x->fxv = 0; } }
    g_song.order_len = 1;
    for (int i = 0; i < 32; i++) g_song.order[i] = 0;
    g_song.bpm = 120; g_song.speed = 6; g_song.octave = 1; g_song.orient = 0; g_song.on = 0;
    for (int c = 0; c < TRK_CH; c++) { g_song.ch_inst[c] = c >= 4 ? 1 : 0; g_song.ch_vol[c] = 80; g_song.ch_pan[c] = 0; for (int k = 0; k < 4; k++) g_song.ch_knob[c][k] = 50; }
    g_song.magic[0] = 'C'; g_song.magic[1] = 'S'; g_song.magic[2] = 'N'; g_song.magic[3] = 'G'; g_song.version = 2;
}

/* ---- the clock (audio block) ---- */
extern "C" void coop_tracker_block(void)
{
    s_blocks++;
    if (s_dirty && !s_save_pending) { if (++s_dirty_age > SAVE_DELAY_BLOCKS) { s_dirty = 0; s_save_pending = 1; } }
    if (!g_trk.playing) return;
    int bpm = clampi(g_song.bpm, 40, 300), speed = clampi(g_song.speed, 1, 16);
    int32_t per_tick = (44100 * 60) / (bpm * 4 * speed);                  /* samples a tick: 4 rows a beat */
    s_tick_acc += 64;
    while (s_tick_acc >= per_tick) { s_tick_acc -= per_tick; g_trk_ticks++; }
}

/* ---- the sequencer (main loop) ---- */

static void row_start(void)
{
    int pat = g_song.order[g_trk.pos & 31] & 15, row = g_trk.row;
    g_trk_rows++;
    for (int c = 0; c < TRK_CH; c++) {
        struct trk_cell *x = &g_song.pat[pat][row & 63][c];
        struct trk_chan *ch = &g_trk.ch[c];
        ch->fx = x->fx; ch->fxv = x->fxv; ch->arp_step = 0; ch->pending = 0;
        if (x->fx == TRK_FX_VOL) g_song.ch_vol[c] = clampi(x->fxv, 0, 99);
        if (x->note == TRK_NOTE_OFF) { ch_off(c); continue; }
        if (!x->note) continue;
        if (x->fx == TRK_FX_PRB && (int)(rnd() % 100) >= clampi(x->fxv, 0, 100)) continue;   /* the dice say no */
        ch->note_cell = x->note; ch->inst_cell = x->inst; ch->vol_cell = x->vol ? x->vol : g_song.ch_vol[c];
        if (x->fx == TRK_FX_DLY && x->fxv > 0) { ch->pending = 1; continue; }   /* later in the row */
        ch_on(c, x->inst, x->note, ch->vol_cell);
    }
}
static void row_tick(int t)                                                /* t = 1.. inside the row */
{
    for (int c = 0; c < TRK_CH; c++) {
        struct trk_chan *ch = &g_trk.ch[c];
        switch (ch->fx) {
        case TRK_FX_ARP: {
            if (!ch->sounding) break;
            int a = ch->fxv / 10, b = ch->fxv % 10, step = ++ch->arp_step % 3;   /* "37": the note, +3, +7 */
            int note = ch->note_cell + (step == 1 ? a : step == 2 ? b : 0);
            ch_on(c, ch->inst_cell, clampi(note, 1, 127), ch->vol_cell);
            break; }
        case TRK_FX_RTG: if (ch->fxv > 0 && t % ch->fxv == 0 && ch->note_cell) ch_on(c, ch->inst_cell, ch->note_cell, ch->vol_cell); break;
        case TRK_FX_CUT: if (t == ch->fxv) ch_off(c); break;
        case TRK_FX_DLY: if (ch->pending && t == ch->fxv) { ch->pending = 0; ch_on(c, ch->inst_cell, ch->note_cell, ch->vol_cell); } break;
        default: break;
        }
    }
}
static void advance_row(void)
{
    int pat = g_song.order[g_trk.pos & 31] & 15;
    if (++g_trk.row >= trk_pat_len(pat)) {
        g_trk.row = 0;
        if (++g_trk.pos >= (g_song.order_len < 1 ? 1 : g_song.order_len)) { g_trk.pos = 0; if (g_trk.render == 2) g_trk.render = 3; }   /* a render ends with the song */
    }
}
extern "C" void coop_tracker_play(int from_start)
{
    if (from_start) { g_trk.pos = 0; g_trk.row = 0; }
    else { g_trk.pos = g_trk.cur_pos; g_trk.row = g_trk.cur_row; }
    g_trk.tick = 0; s_tick_acc = 0; s_ticks_done = g_trk_ticks;
    g_trk.playing = 1; g_trk.row_pending = 1;                              /* the first row plays on the next service pass */
}
/* Render to tape (the owner's decision 5, one way): the stock's own recording. coop pushes the keys the user would
 * press (KeyHandler::PushKey 0x019C2F24, as the test runner does): rec held, play, then rec up, which starts the tape
 * recording the instrument; those keys are marked so coop_tracker_key lets them through to the stock. When the play
 * release has gone through, the song plays from its start; when the order list wraps, the tracker stops and pushes
 * stop: the tape holds one pass of the song. */
#define PUSH_KEY ((void (*)(int))0x019C2F24u)
#define WAKE_KEYS ((void (*)(void))0x019C2F04u)                                /* post the KeyHandler semaphore (the runner does) */
#define KEY_REC 0x21
#define KEY_PLAY 0x22
#define KEY_STOP 0x23
static volatile uint8_t s_pass[64];
volatile uint32_t g_trk_renders;
/* the pushed keys are spaced ~100 ms apart (the stock reads rec held across frames: back-to-back pushes in one
 * KeyHandler drain recorded nothing in op1emu): a small queue the main loop drains one key per INJ_GAP blocks */
#define INJ_GAP 70
static uint16_t s_inj[8]; static int s_inj_n, s_inj_rd; static uint32_t s_inj_due;

static void push_key(int code, int pressed) { if (s_inj_n < 8) s_inj[s_inj_n++] = (uint16_t)((code & 63) | (pressed ? 0x100 : 0)); }
static void inj_service(void)
{
    if (s_inj_rd >= s_inj_n) { s_inj_rd = s_inj_n = 0; return; }
    if ((int32_t)(s_blocks - s_inj_due) < 0) return;
    if (*(volatile uint8_t *)0x01DD7F78u) return;                           /* Shift still down (Shift + play asked for this): shifted rec / play mean other things */
    uint16_t k = s_inj[s_inj_rd++];
    s_pass[k & 63]++;
    PUSH_KEY(k);
    WAKE_KEYS();
    s_inj_due = s_blocks + INJ_GAP;
}
extern "C" int coop_tracker_injected(int code)
{
    if (!s_pass[code & 63]) return 0;
    s_pass[code & 63]--;
    return 1;
}
extern "C" void coop_tracker_render(void)
{
    if (g_trk.playing) coop_tracker_stop();
    g_trk.render = 1;
    g_trk_renders++;
    push_key(KEY_REC, 1); push_key(KEY_PLAY, 1); push_key(KEY_PLAY, 0); push_key(KEY_REC, 0);
}
extern "C" void coop_tracker_stop(void)
{
    g_trk.playing = 0;
    coop_tracker_all_off();
    if (g_trk.render) { g_trk.render = 0; push_key(KEY_STOP, 1); push_key(KEY_STOP, 0); }   /* the tape stops with the song */
}
extern "C" void coop_tracker_service(void)                                 /* main loop (coop_fx_service), every pass */
{
    inj_service();                                                         /* the render's pushed keys, spaced */
    while (s_prev_rd != s_prev_wr) {                                       /* the keyboard's previews, in order */
        uint32_t v = s_prev_ring[s_prev_rd]; s_prev_rd = (s_prev_rd + 1) & 7;
        raw_note((v >> 20) & 7, (v >> 16) & 15, (v >> 8) & 127, (v & 1) ? 100 : 64, v & 1);
    }
    if (s_save_pending) { s_save_pending = 0; coop_tracker_save(); }
    if (!g_trk.playing) return;
    if (g_trk.row_pending) { g_trk.row_pending = 0; row_start(); }
    int speed = clampi(g_song.speed, 1, 16);
    int guard = 64;                                                        /* catch up at most this many ticks a pass */
    while (s_ticks_done != g_trk_ticks && guard-- > 0) {
        s_ticks_done++;
        if (++g_trk.tick >= speed) { g_trk.tick = 0; advance_row(); row_start(); }
        else row_tick(g_trk.tick);
    }
    if (g_trk.render == 3) coop_tracker_stop();                            /* the song ended: the tape stops too */
}

/* ---- the file (main thread) ---- */
extern "C" int coop_tracker_save(void)
{
    int fd = Y_OPEN(SONG_FILE, 0x242, 0x180);
    if (fd < 0) return 0;
    int ok = Y_WRITE(fd, &g_song, (int)sizeof g_song) == (int)sizeof g_song;
    return (Y_CLOSE(fd) == 0) && ok;
}
extern "C" void coop_tracker_boot(void)                                    /* after the tape mounts */
{
    defaults();
    g_trk.edit = 1;                                                        /* the keyboard writes until rec switches it off */
    coop_xvoice_boot();                                                    /* F1: the extra FM voice object (main thread) */
    int fd = Y_OPEN(SONG_FILE, 0, 0);
    if (fd < 0) return;
    const int hdr = (int)(sizeof g_song - sizeof g_song.pat);             /* the header first, into a small buffer (no 36 KB on the stack) */
    uint8_t h[128];
    int got = Y_READ(fd, h, hdr);
    if (got != hdr || h[0] != 'C' || h[1] != 'S' || h[2] != 'N' || h[3] != 'G' || h[4] != 2) { Y_CLOSE(fd); return; }
    got = Y_READ(fd, &g_song.pat, (int)sizeof g_song.pat);                 /* the patterns straight into place */
    Y_CLOSE(fd);
    if (got != (int)sizeof g_song.pat) { defaults(); return; }
    { uint8_t *d = (uint8_t *)&g_song; for (int i = 0; i < hdr; i++) d[i] = h[i]; }
    g_song.bpm = clampi(g_song.bpm, 40, 300); g_song.speed = clampi(g_song.speed, 1, 16);
    g_song.order_len = clampi(g_song.order_len, 1, 32); g_song.orient = clampi(g_song.orient, 0, 2);
}
extern "C" void coop_tracker_request_save(void) { s_save_pending = 1; s_dirty = 0; }
extern "C" int coop_tracker_on(void) { return g_song.on; }
extern "C" void coop_tracker_set_on(int on) { g_song.on = on ? 1 : 0; if (!on) coop_tracker_stop(); coop_tracker_request_save(); }
