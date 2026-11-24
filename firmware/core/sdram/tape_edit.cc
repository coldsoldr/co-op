/*
 * coop-fw stereo tape, T3: lift / drop / erase for stereo tracks (docs/tape-stereo.md section 10).
 *
 * The stock edit functions work on the tape FILE in mono: lift(ch, start, stop, hdr) reads samples from
 * TRACK_BASE[ch] + pos*2 into the clipboard slot hdr[ch] (pool 0x02040000, header = 4 slot pointers) and zeroes
 * the range; drop(src, dst, pos, len, hdr) writes slot src at TRACK_BASE[dst] + pos*2; fast_tape_erase zeroes
 * 485 x 64 KiB per track. With stereo tracks (4 bytes per frame) the same frame positions are at pos*4, so the
 * three are replaced through region-C trampolines (callers: lift 0x191b89c and lift-all 0x1a549a0, drop
 * 0x191b67e and drop-all 0x1a54aee, erase 0x191b9f0):
 *   coop_lift   reads the frames, puts the LEFT samples in the stock slot (the clipboard service sized it for
 *               `len` samples, the UI and tape.db keep working in frames) and the RIGHT samples in a coop pool
 *               (region B: a clip that does not fit it keeps its left side only and drops as mono), zeroes the
 *               frames, invalidates the cache range.
 * e12 (docs/plugins.md "e12"): the pool is no longer all of region B. It starts at COOP_POOL_LO and is at least the
 * floor (1.5 MiB = 786,432 samples = 17.8 s of right channel); the plugin arena takes region B's top only when region
 * A is full, and the pool keeps the rest (the whole 4 MiB, 47.5 s, on a device whose plugins fit region A). Its size
 * is fixed once the boot scan has ended (coop_clip_pool_bytes). The four slots (one per track, as the stock's) share
 * it, and since e12 a slot's samples are given back when its track is lifted again: before, every lift took new
 * room until the tape was erased, so 47.5 s of lifts in a session ended stereo lifts for good.
 *   coop_drop   writes the frames back from the two pools (right = left when the coop pool has no copy).
 *   coop_erase  zeroes 485 x 128 KiB per track.
 * All three run on the main loop (the clipboard service), as stock.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "../include/coop_yio.h"            /* Y_READ / Y_WRITE: odd buffers never reach the stock */
#include "coop_arena.h"

#define Y_READ        coop_y_read
#define Y_WRITE       coop_y_write
#define Y_LSEEK       ((int32_t (*)(int, int32_t, int))0x01A9A0E4u)
#define INVALIDATE    ((void (*)(uint32_t, uint32_t))0x01A540C0u)    /* tape_cache_invalidate_clean_range(from, to) */
#define INVALIDATE_ALL ((void (*)(void))0x01A54078u)
#define TRACKS_REMADE ((void (*)(int))0x01A54020u)                  /* end of the stock erase: (0xf) */
#define ERASE_PROGRESS (*(volatile int32_t *)0x018C17C8u)            /* the stock erase writes its byte count here */
#define CACHE_FD      (*(volatile int32_t *)0x01B4EF44u)
#define TRACKS        4
#define FRAMES_PER_CHUNK 32768
#define CHUNKS        485
#define TRACK_FRAMES  (CHUNKS * FRAMES_PER_CHUNK)
#define COOP_POOL     ((int16_t *)(uintptr_t)COOP_POOL_LO)             /* region B, free [C] */
#define COOP_POOL_SAMPLES (coop_clip_pool_bytes() >> 1)               /* e12: what the arena left of region B */

extern volatile int32_t g_coop_track_base[TRACKS];                   /* tape_io.cc */
volatile uint32_t g_edit_lifts, g_edit_drops, g_edit_erases, g_edit_errors, g_edit_mono_fallbacks;
volatile uint32_t g_edit_pool_used, g_edit_pool_moves;                /* e12 stats: samples in use, slots moved down */
volatile uint32_t g_edit_dbg[8];                                     /* last lift: ch, start, stop, hdr slot, frames done, drop pos, last rw off, last rw result */

static int16_t *g_r_slot[TRACKS];                                    /* right-channel copy per clipboard slot, 0 = none */
static uint32_t g_r_len[TRACKS];
static uint32_t g_pool_used;                                         /* samples of the coop pool in use */
static uint32_t g_bounce[FRAMES_PER_CHUNK];                          /* 128 KiB: one stereo chunk */

/* e12: give slot ch's right-channel samples back: everything above them in the pool moves down (the slots are packed
 * from the pool's base, in the order they were lifted) and the slots that moved follow. Main loop, as lift and drop. */
static void slot_release(int ch)
{
    int16_t *p = g_r_slot[ch];
    uint32_t n = g_r_len[ch];
    g_r_slot[ch] = 0; g_r_len[ch] = 0;
    if (!p || !n) return;
    uint32_t off = (uint32_t)(p - COOP_POOL);
    if (off > g_pool_used || n > g_pool_used - off) { g_edit_errors++; return; }   /* cannot happen: leave the pool as it is */
    uint32_t tail = g_pool_used - (off + n);
    for (uint32_t i = 0; i < tail; i++) p[i] = p[n + i];
    for (int t = 0; t < TRACKS; t++) if (g_r_slot[t] && g_r_slot[t] > p) { g_r_slot[t] -= n; g_edit_pool_moves++; }
    g_pool_used -= n; g_edit_pool_used = g_pool_used;
}

static int rw_frames(int write, int ch, int32_t pos, int32_t n)      /* n frames of track ch at frame pos through g_bounce */
{
    int fd = CACHE_FD;
    int32_t off = g_coop_track_base[ch] + pos * 4;
    if (Y_LSEEK(fd, off, 0) != off) return -1;
    int bytes = n * 4;
    int r = write ? Y_WRITE(fd, g_bounce, bytes) : Y_READ(fd, g_bounce, bytes);
    g_edit_dbg[6] = (uint32_t)off; g_edit_dbg[7] = (uint32_t)r;
    return r == bytes ? 0 : -1;
}

/* Stands in for tape_clips lift (0x1a5485c): (ch, start, stop, hdr) in frames. */
extern "C" void coop_lift(int ch, int32_t start, int32_t stop, int16_t **hdr)
{
    g_edit_lifts++;
    if ((unsigned)ch >= TRACKS || start < 0 || stop <= start || stop > TRACK_FRAMES || !hdr) { g_edit_errors++; return; }
    int16_t *left = hdr[ch];
    uint32_t n = (uint32_t)(stop - start);
    g_edit_dbg[0] = (uint32_t)ch; g_edit_dbg[1] = (uint32_t)start; g_edit_dbg[2] = (uint32_t)stop; g_edit_dbg[3] = (uint32_t)(uintptr_t)left;
    int16_t *right = 0;
    slot_release(ch);                                                 /* e12: this track's old clip goes first */
    uint32_t pool = COOP_POOL_SAMPLES;
    if (g_pool_used <= pool && n <= pool - g_pool_used) { right = COOP_POOL + g_pool_used; g_pool_used += n; }
    else g_edit_mono_fallbacks++;
    g_edit_pool_used = g_pool_used;
    g_r_slot[ch] = right; g_r_len[ch] = right ? n : 0;
    for (uint32_t done = 0; done < n;) {
        uint32_t m = n - done < FRAMES_PER_CHUNK ? n - done : FRAMES_PER_CHUNK;
        if (rw_frames(0, ch, start + (int32_t)done, (int32_t)m) != 0) { g_edit_errors++; return; }
        for (uint32_t i = 0; i < m; i++) {
            uint32_t f = g_bounce[i];
            if (left) left[done + i] = (int16_t)f;
            if (right) right[done + i] = (int16_t)(f >> 16);
        }
        for (uint32_t i = 0; i < m; i++) g_bounce[i] = 0;
        if (rw_frames(1, ch, start + (int32_t)done, (int32_t)m) != 0) { g_edit_errors++; return; }
        done += m;
        g_edit_dbg[4] = done;
    }
    INVALIDATE((uint32_t)start, (uint32_t)stop);
}

/* Stands in for tape_clips drop (0x1a549b8): (src slot, dst track, pos, len, hdr), len in frames. */
extern "C" void coop_drop(int src, int dst, int32_t pos, int32_t len, int16_t **hdr)
{
    g_edit_drops++;
    g_edit_dbg[5] = (uint32_t)pos;
    if ((unsigned)src >= TRACKS || (unsigned)dst >= TRACKS || pos < 0 || len <= 0 || !hdr) { g_edit_errors++; return; }
    if (pos + len > TRACK_FRAMES) len = TRACK_FRAMES - pos;
    const int16_t *left = hdr[src];
    const int16_t *right = g_r_slot[src] && g_r_len[src] >= (uint32_t)len ? g_r_slot[src] : left;
    if (!left) { g_edit_errors++; return; }
    for (int32_t done = 0; done < len;) {
        int32_t m = len - done < FRAMES_PER_CHUNK ? len - done : FRAMES_PER_CHUNK;
        for (int32_t i = 0; i < m; i++)
            g_bounce[i] = (uint32_t)(uint16_t)left[done + i] | ((uint32_t)(uint16_t)right[done + i] << 16);
        if (rw_frames(1, dst, pos + done, m) != 0) { g_edit_errors++; return; }
        done += m;
    }
    INVALIDATE((uint32_t)pos, (uint32_t)(pos + len));
}

/* Stands in for fast_tape_erase (0x1a54b08): zero every track. */
extern "C" void coop_markers_clear(void);         /* coop_tape_ui.cc (u4): an erased tape has no markers */
extern "C" void coop_strip_clear(void);           /* coop_strip.cc (CS6): an erased tape is a new project: strips at defaults */
extern "C" void coop_erase(void)
{
    coop_markers_clear();
    coop_strip_clear();
    g_edit_erases++;
    INVALIDATE_ALL();
    ERASE_PROGRESS = 0;
    for (uint32_t i = 0; i < FRAMES_PER_CHUNK; i++) g_bounce[i] = 0;
    for (int c = 0; c < CHUNKS; c++)
        for (int t = 0; t < TRACKS; t++) {
            if (rw_frames(1, t, c * FRAMES_PER_CHUNK, FRAMES_PER_CHUNK) != 0) g_edit_errors++;
            ERASE_PROGRESS += FRAMES_PER_CHUNK * 4;
        }
    ERASE_PROGRESS = 0;
    g_pool_used = 0; g_edit_pool_used = 0;
    for (int t = 0; t < TRACKS; t++) { g_r_slot[t] = 0; g_r_len[t] = 0; }
    TRACKS_REMADE(0xf);
}
