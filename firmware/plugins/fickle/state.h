/* fickle: the state in the slot's position buffer (sdk/coop_plugin.h e8), shared by the render (fickle.cc) and the
 * screen (fickle_screen.cc, e9: it reads only `disp`, through coop_fx_view_t.state while the view is live).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#ifndef COOP_FICKLE_STATE_H_
#define COOP_FICKLE_STATE_H_
#include <stdint.h>

namespace fickle {

const int32_t kBlock = 64;
const int32_t kLoopPairs = 15288;                      /* the micro-looper's buffer: L | R int16 pairs (59.7 KiB; 0.2:
                                                        * sized so the state is tape_echo's 134,784 bytes) */
const int32_t kWetPairs = 16384;                       /* the wet channel's line (64 KiB), a power of two */
const int32_t kWetMask = kWetPairs - 1;
const int32_t kApPairs = 1152;                         /* the reverb's four diffusers: 128 + 256 + 256 + 512 pairs */
const int32_t kSeam = 256;                             /* the loop seam's crossfade, clock samples (the pre-roll) */
const int32_t kLoopMargin = 512;                       /* a loop is at most kLoopPairs - this (pre-roll + stop fade) */
const int32_t kMinLoop = 1024;                         /* the shortest capture, clock samples */
const int32_t kEnvBins = 24;                           /* the loop's coarse envelope (the screen's ring ticks) */

enum WetMode { W_REVERB, W_DELAY, W_SLIP, W_MODES };
enum Route { R_IN, R_BOTH, R_LOOP, R_ROUTES };
enum LoopMode {
    L_TAPE1, L_TAPE2, L_TAPE_HALF, L_TAPE_FIFTH, L_REV1, L_REV2, L_REV_HALF, L_PING,
    L_ENV_SOFT, L_ENV_HARD, L_STRETCH, L_STR_SLOW, L_STR_REV, L_MODES
};
enum LoopState { S_LISTEN, S_PLAY, S_STOP };           /* S_STOP: fading out, recording again */

/* What the render publishes for the screen (sdk/coop_plugin.h "Display data"): whole words, written once a block after
 * the DSP (the clearing blocks too); the DSP never reads them. */
struct Display {
    int32_t clock_hz;                                  /* the clock in force, Hz (2000 .. 32000) */
    int32_t loop_pos;                                  /* the head in the loop, 0..32767 of its length: the read head
                                                        * while playing, the write head's place in the buffer while
                                                        * listening */
    int32_t loop_len;                                  /* the captured loop's length in clock samples; while listening
                                                        * what a capture would take now (0 = nothing yet) */
    int32_t loop_state;                                /* 0 listening, 1 playing, 2 dubbing, 3 fading out */
    int32_t wet_level;                                 /* the wet channel's output, peak over the block, 0..32767 */
    int32_t loop_level;                                /* the loop's output, peak over the block, 0..32767 */
    int32_t in_level;                                  /* the input, peak over the block, 0..32767 */
    int32_t head;                                      /* delay: the delay time over the line, slip: the head's travel
                                                        * through its refresh, reverb: the size over the line; 0..32767 */
    int32_t freeze;                                    /* 1 while the wet channel is frozen */
    int32_t trig;                                      /* counts the env retriggers (a change = a hit) */
    int32_t wet_mode;                                  /* the wet mode in force (0 reverb, 1 delay, 2 slip) */
    int32_t env[kEnvBins / 4];                         /* the loop's envelope: 24 bytes 0..255, bin b in byte b & 3 of
                                                        * word b >> 2 (a bin is a 24th of the loop, its peak as heard) */
};

struct Grain {
    int32_t pos;                                       /* in the loop, clock samples */
    int32_t age;                                       /* 0 .. kGrain */
};

struct State {
    uint32_t loop[kLoopPairs];                         /* never cleared: only what was recorded is ever read */
    uint32_t wet[kWetPairs];                           /* cleared over the first blocks */
    uint32_t ap[kApPairs];                             /* likewise */

    /* this block's clock samples (k = 0 .. ticks - 1): the input, the loop's output, the wet channel's in / out (one
     * spare: pass A stores every sample and advances only on a tick) */
    int32_t xl[kBlock + 1], xr[kBlock + 1];
    int32_t ol[kBlock], or_[kBlock];
    int32_t wl[kBlock], wr[kBlock];
    uint32_t hold[kBlock + 1];                         /* the clock-domain output (wet + loop) as L | R pairs: [0] the
                                                        * last block's last, [1 + k] this block's clock sample k */
    uint32_t tp[kBlock], tq[kBlock];                   /* the wet channel's block passes: ring runs */
    int32_t tl[kBlock], tr[kBlock];
    uint32_t holdx[kBlock];                            /* the hold for each 44.1 kHz sample */
    uint8_t tick[kBlock];                              /* 1 where a clock sample falls in the 44.1 kHz block */

    int32_t clear;                                     /* the next word of wet + ap to zero; kWetPairs + kApPairs = clean */

    /* the clock */
    uint32_t phase;                                    /* Q16 */
    int32_t inc;                                       /* Q16, eased towards the knob a block */
    int32_t pre_l, pre_r;                              /* the input pre-filter, Q4 */
    int32_t s1l, s1r, s2l, s2r;                        /* the output smoothing poles, Q4 */
    int32_t mix;                                       /* Q15, eased */
    uint32_t n;                                        /* the clock-domain counter: the wet line's and the diffusers'
                                                        * write index */

    /* the wet channel */
    int32_t wmode;                                     /* in force */
    int32_t wduck;                                     /* Q15, the switch fade */
    int32_t gx, gl;                                    /* the routing gains (input, loop), Q15 */
    int32_t lp_l, lp_r;                                /* the feedback damping (delay, reverb) */
    int32_t fb_l, fb_r;                                /* reverb: the tap sum fed back */
    int32_t size;                                      /* reverb: the size in force, clock samples */
    int32_t d, d_old, d_xf;                            /* delay: the time, the time faded from, the fade's count */
    int32_t sa, sb;                                    /* slip: the heads (Q16 in the line, wrapping at 2^30) */
    int32_t s_age, s_xf, s_r;                          /* slip: the time since the refresh, the fade's count, refresh */
    uint32_t n_frozen;                                 /* slip: the write point while frozen */
    int32_t frozen;

    /* the micro-looper */
    int32_t lstate;                                    /* LoopState */
    int32_t lmode;                                     /* in force */
    int32_t lmode_want;                                /* the word's, this block */
    int32_t lduck;                                     /* Q15, the mode switch fade */
    int32_t w;                                         /* the write head (buffer index) */
    int32_t filled;                                    /* clock samples recorded since the start (to kLoopPairs) */
    int32_t start, len;                                /* the loop */
    int32_t p;                                         /* the read position in the loop, Q16 */
    int32_t dir;                                       /* ping: +1 / -1 */
    int32_t xp, xcount;                                /* a jump's crossfade: the old position (Q16), its count */
    int32_t gain;                                      /* the loop's level, Q15 (fades in and out) */
    int32_t dubp;                                      /* the dub head, in the loop */
    int32_t dubg;                                      /* the dub's level, Q15 */
    int32_t envf;                                      /* env: the input's peak follower */
    int32_t armed, holdoff, slice;
    int32_t gate;                                      /* env soft: the loop's level, Q30 */
    Grain g[2];
    int32_t scan;                                      /* stretch: the scan position, Q16 in the loop */
    int32_t trig;
    int32_t ebin;
    int32_t bake;                                      /* seam pairs still to crossfade (BakeSeam), from the end */                                      /* the envelope bin the head was in last block */
    uint8_t env[kEnvBins];                             /* the loop's envelope as heard (Display.env) */

    Display disp;                                      /* e9: last */
};

}  // namespace fickle

#endif
