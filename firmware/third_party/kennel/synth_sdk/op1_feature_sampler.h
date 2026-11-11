/*
 * kennel core services for TE's sampler screens (kennel/kn_sampler.c, docs/feature-hooks.md "Sampler"): the synth
 * sampler (screen 6, engine 6) and the drum sampler kit (screen 0x10, engine 8). A feature reads and edits the
 * slot's sample buffer and its markers through these calls; the core keeps every R246 address (buffers, knob
 * words, the drum pad table, the patch, the background saver, base_freq) and runs long work on the main loop.
 * Include this header (it includes op1_feature.h) from feature code.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_SAMPLER_H
#define OP1_FEATURE_SAMPLER_H
#include "op1_feature.h"

#define OP1_SAMPLE_RATE 44100
enum { OP1_SAMPLER_SYNTH = 0, OP1_SAMPLER_DRUM = 1 };
/* markers, in samples from the start of the audio (after the buffer's guard samples) */
enum { OP1_SM_START = 0, OP1_SM_LOOP_A = 1, OP1_SM_LOOP_B = 2, OP1_SM_END = 3 };

typedef struct {
    uint8_t kind;               /* OP1_SAMPLER_SYNTH / OP1_SAMPLER_DRUM */
    uint8_t slot;               /* 0..7 */
    int8_t key;                 /* drum: the key played last, 0..23 (its markers are the ones shown); synth -1 */
    uint8_t busy;               /* 1 while the slot samples (keyboard to sample) or the tape records: no edits */
    int16_t *data;              /* the audio (after the guard samples) */
    int32_t cap;                /* audio capacity in samples (synth 6 s, drum 12 s) */
    uint32_t gen;               /* changes when the kind, slot, key or sound changes (and after op1_sampler_commit
                                 * with audio): reset views and per-sound memory when it does */
} op1_sampler_t;

/* 1 while TE's own sampler screen is the one shown (synth with a sampler sound, or drum with a sampler kit, and no
 * other screen swapped in its place, e.g. the sample editor or drum+ pages); fills *s. Any thread. */
int op1_sampler_now(op1_sampler_t *s);
/* a marker of the shown sound, in samples: synth OP1_SM_*; drum: start / end of `key` (-1 = the shown key) */
int32_t op1_sampler_marker(int marker, int key);
/* move a marker (heard at once; clamped to the buffer). Persist with op1_sampler_commit. */
void op1_sampler_set_marker(int marker, int key, int32_t pos);
/* save into the sound: the markers; with audio = 1 also the waveform overview (rebuilt on the following main loop
 * passes) and then the sample file (the stock background saver writes it). Main loop or KeyHandler. */
void op1_sampler_commit(int audio);
/* synth sampler: the sound's base frequency (the key frequency in Hz that plays the sample at its own speed) as the
 * patch stores it, an IEEE double split in two words (lo, hi). 0 = none. */
int op1_sampler_base_freq(uint32_t bits[2]);
/* set it: the patch value (double bits) and the live read step (the same frequency as an IEEE float, f32);
 * the sound counts as changed (saved like any edit). 0 ok, -1 not a synth sampler. */
int op1_sampler_set_base_freq(const uint32_t bits[2], uint32_t f32);
/* run `step(ctx)` on the main loop, once per pass, until it returns 0 (keep each call well under 2 ms). The core
 * measures it and logs "sampler: job <total> cycles, max <max> at <step>/<steps>" when it ends. 0 started, -1 a job is running. */
int op1_sampler_job(int (*step)(void *ctx), void *ctx);
int op1_sampler_job_running(void);

#endif
