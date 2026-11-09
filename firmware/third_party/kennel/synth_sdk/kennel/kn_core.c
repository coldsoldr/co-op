/*
 * kennel core services for features (op1_feature.h, docs/feature-hooks.md "Common hooks"): events, the clock tick,
 * the input filter, MIDI in / out, toasts, memory and the music context's owner. Callers in the core:
 *   kn_core_service    main loop (kn_tape.c ci_service): boot done, mode / slot / transport / tempo changes, key holds,
 *                      toast timer
 *   kn_features_tick   audio thread, every 64-sample block (kn_tape.c ci_metro_render)
 *   kn_features_input  key and encoder events (dispatcher.c key_event_hook / knob_event_hook)
 *   kn_midi_in         MIDI CC / START hooks (dispatcher.c)
 *   kn_screens_update  every frame: CALL screens_update at 0x019B2690 in gfx_render_frame (Gfx thread)
 * The text entry (kn_text.c) hangs off the same three: input first, its screen after the overlays, done on the main
 * loop. op1_undo_area hands out the SDRAM block below the plugin arena.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_synth.h"
#include "../op1_feature.h"
#include "kn_screens.h"
#include "../kn_mem.h"
#include "kn_input.h"

int kn_feature_count(void);
const op1_feature_t *kn_feature_at(int i);
void *kn_alloc(uint32_t size);
int kn_text_input(int type, int code, int pressed, int delta, int shift);   /* kn_text.c */
void kn_text_service(void), kn_text_draw(void *r);
void kn_storage_service(void);                                  /* kn_storage.c */
void kn_flash_service(void);                                    /* kn_flash.c */
void kn_sampler_service(void) __attribute__((weak));           /* kn_sampler.c: sampler tools' main loop jobs */

#define CUR_MODE       (*(volatile int *)0xFF907D58)          /* ModeSwitcher: active mode */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8)
#define SYNTH_SLOT     (*(volatile int *)0xFF8016D4)
#define TAPE_STATE     (*(volatile int32_t *)0x018C17D4)
#define SHIFT_HELD     (*(volatile uint8_t *)0x01DD7F78)
#define TX_ENQ         ((void (*)(int, const void *, unsigned))0x019E0D18)   /* usbmidi_tx_enqueue(_, packets, count) */
#define HOLD_CYCLES    (240u * 1000 * 1000)                   /* 600 ms at 400 MHz */

static uint64_t now_cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* ---- events ---- */
__attribute__((noinline)) void kn_event(int id, int32_t arg)     /* not inlined: emulator tests break on it */
{
    for (int i = 0; i < kn_feature_count(); i++) {
        const op1_feature_t *f = kn_feature_at(i);
        if (f->event) f->event(id, arg);
    }
}

static volatile int held_code = -1, hold_sent;
static volatile uint64_t held_since;
void kn_core_key(int code, int pressed)                         /* key_event_hook: remember the key held last */
{
    if (pressed) { held_code = code; held_since = now_cycles(); hold_sent = 0; }
    else if (code == held_code) held_code = -1;
}

/* The boot toast's text, or 0 when there is none: what the loader refused (kn_boot.c's counts). m holds 48 bytes.
 * Its own function so the harness's whole-build boot (harness/whole.py) reads the same text. */
int kn_boot_toast_text(char *m)
{
    extern volatile uint32_t kn_failed, kn_skipped;
    uint32_t n = kn_skipped ? kn_skipped : kn_failed;
    if (!n) return 0;
    char d[8], *o = m; int k = 0, single = n == 1;
    do d[k++] = (char)('0' + n % 10); while (n /= 10);
    while (k) *o++ = d[--k];
    const char *s = single ? " plugin file" : " plugin files";
    while (*s) *o++ = *s++;
    s = kn_skipped ? " skipped: restart to load" : " not loaded";
    while (*s) *o++ = *s++;
    *o = 0;
    return 1;
}
static void toast_show(const char *text, int frames);
static int toast_duration = 120;
static volatile int toast_new;                                  /* defined with the toast below */
static int32_t toast_left;                                      /* frames */
void kn_tapeview_service(void);                                 /* kn_midi.c */
void kn_core_service(void)                                      /* main loop */
{
    kn_tapeview_service();
    kn_input_service();
    static int booted, mode = -1, slot = -1, tape = -1;
    static int32_t tempo;
    if (!booted) {
        booted = 1; mode = CUR_MODE; slot = SYNTH_SLOT & 7; tape = TAPE_STATE;
        char m[48];
        if (kn_boot_toast_text(m)) toast_show(m, 360);
        kn_event(OP1_EV_BOOT_DONE, 0);
    }
    static int sampler_told;                                    /* sampler presets past the limit: once, after the boot toast */
    if (!sampler_told && !toast_new && toast_left <= 0) {
        extern int kn_sampler_guard_text(char *m);              /* kn_sampler_guard.c */
        extern void kn_log_line(const char *what, const char *msg);   /* kn_boot.c */
        char m[48];
        sampler_told = 1;
        if (kn_sampler_guard_text(m)) { toast_show(m, 360); kn_log_line("sampler presets", m); }
    }
    if (CUR_MODE != mode) { mode = CUR_MODE; kn_event(OP1_EV_MODE, mode); }
    if ((SYNTH_SLOT & 7) != slot) { slot = SYNTH_SLOT & 7; kn_event(OP1_EV_SLOT, slot); }
    if (TAPE_STATE != tape) { tape = TAPE_STATE; kn_event(OP1_EV_TRANSPORT, tape); }
    op1_beat_t b;
    op1_beat_clock(&b);
    if (b.len > 0 && b.len != tempo) { tempo = b.len; kn_event(OP1_EV_TEMPO, tempo); }
    int code = held_code;
    if (code >= 0 && !hold_sent && now_cycles() - held_since >= HOLD_CYCLES) { hold_sent = 1; kn_event(OP1_EV_KEY_HOLD, code); }
    kn_text_service();                                          /* a finished text entry's done */
    kn_storage_service();                                       /* the storage walk, a few entries per pass */
    kn_flash_service();                                         /* the flash check, a few pages per pass */
    /* Firmware-only audio flag; fake-core hosts have no R246 sampler state. */
#ifndef OP1_HOST
    extern void kn_samplerplus_gate(void);kn_samplerplus_gate();
#endif
    { extern void kn_ds_service(void) __attribute__((weak)); if(kn_ds_service)kn_ds_service(); }
    { extern void sb_service(void) __attribute__((weak)); if (sb_service) sb_service(); }   /* sample browser (screens/sbrowse.c) */
    kn_event(OP1_EV_SERVICE, 0);                                /* general feature main-loop work */
    if (kn_sampler_service) kn_sampler_service();               /* a sampler job's next chunk */
}

/* ---- clock tick (audio thread) ---- */
void kn_features_tick(void)
{
    int n = kn_feature_count();
    if (!n) return;
    op1_beat_t b;
    int have = 0;
    for (int i = 0; i < n; i++) {
        const op1_feature_t *f = kn_feature_at(i);
        if (!f->tick) continue;
        if (!have) { op1_beat_clock(&b); have = 1; }
        f->tick(&b);
    }
}

/* ---- input filter (KeyHandler thread) ---- */
int kn_features_input(int type, int code, int pressed, int delta)
{
    if (kn_text_input(type, code, pressed, delta, SHIFT_HELD)) return 1;
    if (kn_input_control(type,code,pressed)) return 1;   /* an open text entry takes it first */
    { extern int kn_ds_key(int,int) __attribute__((weak)); extern int kn_ds_knob(int,const int16_t *) __attribute__((weak));
      int16_t d=(int16_t)delta;
      if(type==OP1_IN_KEY&&kn_ds_key&&kn_ds_key(code,pressed))return 1;
      if(type==OP1_IN_KNOB&&kn_ds_knob&&kn_ds_knob(code,&d))return 1; }
    op1_input in = { (uint8_t)type, (uint8_t)code, (uint8_t)pressed, SHIFT_HELD, (int16_t)delta, (int16_t)CUR_MODE, CUR_SCREEN };
    for (int i = 0; i < kn_feature_count(); i++) {
        const op1_feature_t *f = kn_feature_at(i);
        if (f->input && f->input(&in)) return 1;
    }
    return 0;
}

/* ---- MIDI ---- */
void kn_midi_in(const uint8_t *msg, int len)
{
    for (int i = 0; i < kn_feature_count(); i++) {
        const op1_feature_t *f = kn_feature_at(i);
        if (f->midi_in) f->midi_in(msg, len);
    }
}

int op1_midi_send(const uint8_t *msg, int len)                  /* one USB-MIDI packet, cable 0 */
{
    if (len < 1 || len > 3 || !(msg[0] & 0x80)) return -1;
    uint32_t cin = msg[0] >= 0xF8 ? 0xF : msg[0] >= 0xF0 ? (len == 3 ? 0x3 : len == 2 ? 0x2 : 0x5) : msg[0] >> 4;
    uint32_t w = cin;
    for (int i = 0; i < len; i++) w |= (uint32_t)msg[i] << (8 * (i + 1));
    TX_ENQ(0, &w, 1);
    return 0;
}

/* ---- memory ---- */
void *op1_alloc(uint32_t size) { return kn_alloc(size); }

/* kn_mem.h KN_UNDO_LO..KN_UNDO_HI: above R246's last BSS (0x027FFB03, docs/firmware/clocks-memory.md) and below the
 * plugin arena; nothing else in kennel addresses it. One owner. */
#define UNDO_LO KN_UNDO_LO
#define UNDO_HI KN_UNDO_HI
void *op1_undo_area(const char *owner, uint32_t *size)
{
    static const char *taken;
    if (taken && taken != owner) return 0;
    taken = owner ? owner : "?";
    if (size) *size = UNDO_HI - UNDO_LO;
    return (void *)UNDO_LO;
}

/* FX position leases in the big-buffer pool (kn_mem.h): 1 MiB each for synth (0), drum (1) and master (2) FX, like
 * R246's op1_fx_scratch: only the active FX at a position uses it, the contents are undefined after an FX switch.
 * Cleared once at boot (kn_boot.c pool_clear); 0 before that or for a bad position. */
void *op1_fx_big(int pos, uint32_t *size)
{
    extern volatile uint32_t kn_pool_ready;
    int ok = kn_pool_ready && pos >= 0 && pos < KN_FX_BIG_COUNT;
    if (size) *size = ok ? KN_FX_BIG_SIZE : 0;
    return ok ? (void *)(KN_POOL_LO + (uint32_t)pos * KN_FX_BIG_SIZE) : 0;
}

/* ---- music context: one owner ---- */
static volatile uint32_t harmony_scale, harmony_chord;
static int music_name(const char *a,const char *b)
{ if(!a)return 0;while(*a&&*a==*b){a++;b++;}return *a==*b; }
int op1_harmony_context(int32_t out[4])
{
    if(!out)return 0;
    uint32_t sc=harmony_scale,ch=harmony_chord,selected=sc?sc:ch;
    out[0]=(int32_t)(selected&15);out[1]=(int32_t)((selected>>4)&0xfff);
    out[2]=sc!=0;out[3]=ch!=0;
    return selected!=0;
}
void op1_music_publish(const char *owner, int valid, int root, uint16_t scale_mask)
{
    /* One aligned word publishes each context; audio readers never spin on a writer. */
    uint32_t packed=valid?0x10000u|((uint32_t)(scale_mask&0xfff)<<4)|(uint32_t)(((root%12)+12)%12):0;
    if(music_name(owner,"scale"))harmony_scale=packed;
    if(music_name(owner,"chord"))harmony_chord=packed;
    if (!valid) {                                                /* withdraw only one's own key */
        if (op1_music.owner == owner) op1_music.valid = 0;
        return;
    }
    op1_music.valid = 0;                                         /* readers see valid last */
    op1_music.root = (uint8_t)(((root % 12) + 12) % 12);
    op1_music.scale_mask = scale_mask;
    op1_music.owner = owner;
    op1_music.valid = 1;
}

/* ---- toast: drawn over every screen after the frame's screens (Gfx thread) ---- */
static char toast_text[48];
static volatile int toast_new;
static void toast_show(const char *text, int frames)             /* publish after the text and duration */
{
    int i = 0;
    for (; text[i] && i < (int)sizeof toast_text - 1; i++) toast_text[i] = text[i];
    toast_text[i] = 0;
    toast_duration = frames;
    toast_new = 1;
}

void op1_toast(const char *text) { toast_show(text, 120); }        /* any thread, nonmodal */

#include "../screens/toast.h"
void kn_screens_update(void *table)                             /* CALL screens_update at 0x019B2690 (hook.S) */
{
    kn_input_screen_sync();
    kn_screens_frame(table);
    for (int i = 0; i < kn_feature_count(); i++) {             /* features' overlays */
        const op1_feature_t *f = kn_feature_at(i);
        if (f->overlay) f->overlay(op1_raster(), CUR_SCREEN);
    }
    kn_text_draw(op1_raster());                                 /* an open text entry, over everything */
    if (toast_new) {
        kn_toast_prepare(toast_text);
        toast_left = toast_duration;                            /* before toast_new = 0: the main loop reads both */
        toast_new = 0;
    }
    if (toast_left > 0) { toast_left--; kn_toast_draw(op1_raster()); }
}

/* op1_dbg (op1_plugin.h): empty on the OP-1. Kept as a real call (noinline, used, the arguments live) so an
 * emulator can log each call by the address of _op1_dbg in build/synth.sym, as op1r's OP1EMU_DBG does. */
__attribute__((noinline, used)) void op1_dbg(const char *text, int a, int b)
{
    __asm__ volatile ("" : : "r"(text), "r"(a), "r"(b) : "memory");
}
