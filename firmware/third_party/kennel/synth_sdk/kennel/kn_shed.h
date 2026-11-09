/* Soft overload: near the audio deadline, silence plugin synth voices one at a time instead of bypassing a whole
 * engine (kennel/kn_shed.c, ARCHITECTURE.md "Audio budget and overload").
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef KN_SHED_H
#define KN_SHED_H
#include <stdint.h>

/* Thresholds: cycles of the whole audio callback against the measured block (kn_audio_block_cycles(): 580,499 at the
 * unit's 44.1 kHz link, 1,160,997 at the emulator's default 22.05 kHz). A block at or over 90 % is hot; one under 75 %
 * clears the hot count; in between it is kept (hysteresis). kn_shed_hi / kn_shed_lo hold the current values (also
 * for a test harness to lower them); they are recomputed only when the block length changes. */
#define KN_SHED_HI_PCT 90
#define KN_SHED_LO_PCT 75
#define KN_SHED_BLOCKS 2          /* hot blocks (1.45 ms each on the unit) before one voice goes; restarts after it */
extern uint32_t kn_shed_hi, kn_shed_lo;
uint32_t kn_audio_block_cycles(void);              /* kennel/kn_input.c */

enum { KN_SHED_RENDER = 0, KN_SHED_RAMP = 1, KN_SHED_SILENT = 2 };

void kn_shed_begin(void);                          /* audio callback start */
int  kn_shed_end(uint32_t used);                   /* audio callback end (whole cycles): 1 = a voice was shed */
void kn_shed_note_on(int v);                       /* producer note_on reached voice v: it plays again */
int  kn_shed_render(int v);                        /* before a plugin voice render: KN_SHED_* */
void kn_shed_ramp(int16_t *out);                   /* after the KN_SHED_RAMP render: linear fade over the block */
void kn_shed_rendered(int v, uint32_t cycles);     /* a plugin voice rendered (not silenced) this block */
extern volatile uint32_t kn_shed_voices;           /* voices shed since boot (resource monitor) */
/* Last resort after shedding: a callback over the whole measured block (kn_audio_block_cycles()) for
 * KN_BYPASS_BLOCKS blocks in a row; 1 = bypass the heaviest plugin now (and the count restarts). */
#define KN_BYPASS_BLOCKS 64
int  kn_bypass_tick(uint32_t used);
/* The count behind kn_bypass_tick, for any owner of its own count (kn_drum_synth.c, per provider): a callback at or
 * under the measured block clears *count; KN_BYPASS_BLOCKS callbacks over it in a row return 1 and restart it. */
static inline int kn_overrun_tick(uint16_t *count, uint32_t used)
{
    if (used <= kn_audio_block_cycles()) { *count = 0; return 0; }
    if (++*count < KN_BYPASS_BLOCKS) return 0;
    *count = 0;
    return 1;
}
#endif
