/*
 * coop-fw stereo tape, milestone T1 (docs/tape-stereo.md section 4): stereo tracks as lane pairs on the
 * stock tape engine.
 *
 * The stock cache keeps, per node, 4 x 64 KiB of chunk data (track t at data + t*0x10000) and the stock
 * L1 record function 0xffa06334 overdubs a mono block into lane W[0xff802964] (the selected track):
 * p = data + (lane << 16) + 2*offset. With the node data doubled to 8 x 64 KiB (insert tool patch at
 * 0x01a54172) a stereo track t is the lane pair (2t = L, 2t+1 = R), and the stock function records
 * stereo when it is called twice per block -- once per channel with the lane index swapped, the
 * record state restored in between and each channel's own input ring. The play mix is the stock loop
 * over 8 lanes (LC1 patched 4 -> 8) with lane gains (gL_t, 0) / (0, gR_t) that coop_tape_gains builds
 * from the stock pan law's 4 (gL, gR) pairs (the mixer's pan becomes balance).
 *
 * Record input: L = M + dL, R = M + dR where M is the mono bus pointer the stock call passes and dL/dR
 * are the stereo bus's decimated difference buses for this block (stereo_bus.cc). Spread 0 -> both
 * lanes record exactly what stock records. g_coop_tape_stereo_in = 0 forces L = R = M (the bit-exact
 * regression image).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_stereo.h"

#define STOCK_RECORD    ((void (*)(const int16_t *))0xffa06334u)   /* l1_tape_record(mono_in) */
#define TAPE_TRACK      ((volatile int32_t *)0xff802964u)          /* selected track (kennel TAPE_TRACK) */
#define REC_STATE       ((volatile uint32_t *)0x018c17d8u)         /* d_tape_rec_state, 0x20 B */
#define REC_STATE_WORDS 8
#define TAPE_STATE      ((volatile uint32_t *)0xff802960u)         /* tape state words up to the ring */
#define TAPE_STATE_WORDS 37                                        /* 0xff802960..0xff8029f4 */
#define REC_RING        ((volatile uint32_t *)0xff8029f4u)         /* record input ring (history + 64 new) */
#define REC_RING_WORDS  41                                         /* 0xff8029f4..0xff802a98 */
#define TRACKS 4

/* --- controls (.data: inject_app --set-word) --- */
__attribute__((section(".data"))) volatile uint32_t g_coop_tape_stereo = 1;     /* 0 = single stock call on lane t (mono lanes 0-3, stock layout) */
__attribute__((section(".data"))) volatile uint32_t g_coop_tape_stereo_in = 1;  /* 0 = L = R = M (mono input, stereo layout) */

/* --- counters --- */
volatile uint32_t g_tape_rec_calls, g_tape_rec_cycles_max, g_tape_state_mismatch, g_tape_gains_calls;
volatile uint32_t g_tape_rec_side_blocks, g_tape_mono_ptr, g_tape_state_mismatch2, g_tape_mismatch_word;

/* --- state --- */
static int16_t g_inL[COOP_MBLK], g_inR[COOP_MBLK];
static uint32_t g_ringL[REC_RING_WORDS], g_ringR[REC_RING_WORDS];
static uint32_t g_rec_snap[REC_STATE_WORDS], g_tape_snap[TAPE_STATE_WORDS];
static uint32_t g_rec_after[REC_STATE_WORDS], g_tape_after[TAPE_STATE_WORDS];
static int g_rings_inited;
static int16_t g_lane_gains[16];          /* [0..7] = L-side gains per lane, [8..15] = R-side gains per lane */
static int16_t g_lane_meters[8];
static volatile int16_t *g_stock_meters;  /* the 4 stock peak words the UI reads */

extern "C" int coop_stereo_side_bufs(const int16_t **l, const int16_t **r);   /* stereo_bus.cc */

static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

static void copy_words(volatile uint32_t *dst, const volatile uint32_t *src, int n)
{
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

/* Replaces the two `CALL 0xffa06334` sites (0xffa0217a synth, 0xffa02dca drums) via coop_tape_rec_thunk. */
extern "C" void coop_tape_record(const int16_t *mono)
{
    uint32_t t0 = coop_cycles();
    g_tape_rec_calls++;
    g_tape_mono_ptr = (uint32_t)(uintptr_t)mono;
    if (!g_coop_tape_stereo) {                 /* stock behaviour: one mono pass on lane t */
        STOCK_RECORD(mono);
        return;
    }
    const int16_t *dL = 0, *dR = 0;
    int side = g_coop_tape_stereo_in && coop_stereo_side_bufs(&dL, &dR);
    if (side) {
        g_tape_rec_side_blocks++;
        for (int i = 0; i < COOP_MBLK; i++) {
            g_inL[i] = coop_fr16_add(mono[i], dL[i]);
            g_inR[i] = coop_fr16_add(mono[i], dR[i]);
        }
    } else {
        for (int i = 0; i < COOP_MBLK; i++) { g_inL[i] = mono[i]; g_inR[i] = mono[i]; }
    }
    if (!g_rings_inited) {                     /* both channels start from the stock ring's history */
        copy_words(g_ringL, REC_RING, REC_RING_WORDS);
        copy_words(g_ringR, REC_RING, REC_RING_WORDS);
        g_rings_inited = 1;
    }
    int32_t track = *TAPE_TRACK;
    if ((uint32_t)track >= TRACKS) { STOCK_RECORD(mono); return; }

    copy_words(g_rec_snap, REC_STATE, REC_STATE_WORDS);
    copy_words(g_tape_snap, TAPE_STATE, TAPE_STATE_WORDS);
    /* pass 1: left lane */
    copy_words(REC_RING, g_ringL, REC_RING_WORDS);
    *TAPE_TRACK = 2 * track;
    STOCK_RECORD(g_inL);
    copy_words(g_ringL, REC_RING, REC_RING_WORDS);
    copy_words(g_rec_after, REC_STATE, REC_STATE_WORDS);
    copy_words(g_tape_after, TAPE_STATE, TAPE_STATE_WORDS);
    /* pass 2: right lane, from the same starting state */
    copy_words(REC_STATE, g_rec_snap, REC_STATE_WORDS);
    copy_words(TAPE_STATE, g_tape_snap, TAPE_STATE_WORDS);
    copy_words(REC_RING, g_ringR, REC_RING_WORDS);
    *TAPE_TRACK = 2 * track + 1;
    STOCK_RECORD(g_inR);
    copy_words(g_ringR, REC_RING, REC_RING_WORDS);
    *TAPE_TRACK = track;
    for (int i = 0; i < REC_STATE_WORDS; i++)   /* both passes must end in the same record state */
        if (REC_STATE[i] != g_rec_after[i]) { g_tape_state_mismatch++; break; }
    for (int i = 0; i < TAPE_STATE_WORDS; i++)
        if (TAPE_STATE[i] != g_tape_after[i]) { g_tape_state_mismatch2++; g_tape_mismatch_word = i + 1; break; }
    /* pass 2 contributes lane data and its dirty bit only: the engine state is what pass 1 left */
    copy_words(REC_STATE, g_rec_after, REC_STATE_WORDS);
    copy_words(TAPE_STATE, g_tape_after, TAPE_STATE_WORDS);
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_tape_rec_cycles_max) g_tape_rec_cycles_max = dt;
}

/* From coop_tape_gains_thunk, in the tick right after the stock pan law (site 0xffa06de0), with the
 * tick's SP: [sp+0x5c] -> int16 gL[4], gR[4] (the stack array); [sp+0x60] -> the 4 stock peak meters.
 * Builds the 8-lane gain table the patched loops read (gL at +0, gR at +0x10 bytes) and gives the
 * loops an 8-word meter array, folding the previous block's lane peaks into the stock words. */
extern "C" void coop_tape_gains(uint32_t sp)
{
    g_tape_gains_calls++;
    int stereo = g_coop_tape_stereo != 0;      /* the loops run 8 lanes either way (LC1 patched): always hand them a table */
    int16_t **slot_g = (int16_t **)(uintptr_t)(sp + 0x5c);
    int16_t **slot_m = (int16_t **)(uintptr_t)(sp + 0x60);
    const int16_t *g = *slot_g;
    for (int i = 0; i < 16; i++) g_lane_gains[i] = 0;
    for (int t = 0; t < TRACKS; t++) {
        if (stereo) {
            g_lane_gains[2 * t] = g[t];            /* L lane: left gain only */
            g_lane_gains[8 + 2 * t + 1] = g[4 + t];/* R lane: right gain only */
        } else {
            g_lane_gains[t] = g[t];                /* stock lanes 0-3, lanes 4-7 silent */
            g_lane_gains[8 + t] = g[4 + t];
        }
    }
    volatile int16_t *stock_m = (volatile int16_t *)*slot_m;
    if (g_stock_meters == stock_m) {           /* fold last block's lane peaks (the loop keeps a running max) */
        for (int t = 0; t < TRACKS; t++) {
            int16_t m;
            if (stereo) {
                int16_t a = g_lane_meters[2 * t], b = g_lane_meters[2 * t + 1];
                m = a > b ? a : b;
            } else {
                m = g_lane_meters[t];
            }
            if (m > stock_m[t]) stock_m[t] = m;
        }
        for (int i = 0; i < 8; i++) g_lane_meters[i] = 0;
    } else {
        g_stock_meters = stock_m;
        for (int i = 0; i < 8; i++) g_lane_meters[i] = 0;
    }
    *slot_g = g_lane_gains;
    *slot_m = g_lane_meters;
}
