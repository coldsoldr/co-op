/* Core L1 pool (kennel/kn_l1.c, ARCHITECTURE.md "L1 pool"): on-chip data SRAM for plugins' hottest per-sample data.
 * R246 keeps the Lander game's 40 sound effects (24,757 bytes, 8-bit PCM) in L1 data B 0xFF90081C..0xFF906908, read
 * by one function only (0x019D3A6E, "play sample", through the table at 0xFF906908 / sizes at 0xFF9069A8), never
 * written, never a DMA target (OP1-Emulator run/l1pool-2026-10-29/l1b-audit.md). With the Lander off (kennel's
 * default, kn_lander_enabled != 1) kn_l1_init points every entry at a dummy with size 0 and the range is ours:
 *   0xFF900820  shared: a quarter of OP1_SINE_TABLE (2,049 int16, op1_l1_sine_quarter), 4,128 B
 *   0xFF901840  OP1_L1_SYNTH   8,192 B     the synth engine
 *   0xFF903840  OP1_L1_FX      8,320 B     the synth or drum FX (positions 0 / 1: only one plays at a time)
 *   0xFF9058C0  OP1_L1_MASTER  4,160 B     the master FX (position 2)
 *   0xFF907E00  OP1_L1_DRUM      512 B     L1 data B's top, which the LDR never loads (free in any build)
 * A partition is a lease: a plugin claims it every block it renders (op1_l1_claim); the holder keeps it while it does,
 * anyone else gets 0 (and keeps its data in SDRAM) until the holder has not claimed for KN_L1_IDLE blocks, so two
 * engines rendering in the same block (release tails after a sound switch) never take turns. `fresh` says the
 * partition is new to the caller (first claim, taken over, or the pool switched back on): copy the data in again.
 * op1_l1_claim: constant tables and per-block scratch only (nothing that must survive losing the lease).
 * op1_l1_claim_state (OP1_L1_API 2): also the plugin's persistent state, bytes [0, state_bytes) of the partition,
 * whose home is the plugin's own SDRAM copy (the home is the owner). The core copies home -> L1 when the plugin takes
 * the partition and L1 -> home exactly once when it loses it: when another owner takes over (after the idle rule,
 * inside that owner's claim, before it gets the pointer), when page 7's switch goes off (kn_l1_tick, at the block's
 * end) or when the holder claims with another state size. Takeovers and copies happen only inside the audio callback
 * (kn_l1_begin .. kn_l1_tick); outside it a claim gets the holder its partition and anyone else 0.
 * A synth note_on can run on the key or a sequencer thread (R246's core_note_on 0x01967854) and be preempted by the
 * callback: dispatcher.c brackets it with kn_l1_thread_enter / _leave, and while such a call runs the callback
 * neither takes a partition over (the claimant gets 0 and works on its home) nor applies page 7's switch.
 * No pool (Lander on, a table that isn't the stock one, page 7's switch off): both claims return 0.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef KN_L1_H
#define KN_L1_H
#include <stdint.h>

#ifndef OP1_L1_API                               /* (op1_plugin.h has the same, for plugins) */
enum { OP1_L1_SYNTH, OP1_L1_FX, OP1_L1_MASTER, OP1_L1_DRUM, OP1_L1_PARTS };
#endif
#define KN_L1_SINE_BYTES   4128u
#define KN_L1_SYNTH_BYTES  8192u
#define KN_L1_FX_BYTES     8320u                 /* the synth / drum FX */
#define KN_L1_MASTER_BYTES 4160u
#define KN_L1_DRUM_BYTES   512u
#define KN_L1_IDLE         2u                    /* blocks without a claim before another owner may take a partition */

void kn_l1_init(void);                           /* kn_boot_hook, before any plugin code runs */
void kn_l1_begin(void);                          /* the audio callback starts (dispatcher.c kn_audio_begin) */
void kn_l1_tick(void);                           /* it ends: once per audio block (dispatcher.c kn_audio_end) */
uint32_t kn_l1_thread_enter(void);               /* a plugin call starts; on another thread (core_note_on) it holds
                                                  * off takeovers and the switch until kn_l1_thread_leave(its result) */
void kn_l1_thread_leave(uint32_t t);
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh);
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh);
const int16_t *op1_l1_sine_quarter(void);        /* OP1_SINE_TABLE[0..2048] in L1, or 0 */
uint32_t kn_l1_ready(void);                      /* 1 when the pool is ours */
void kn_l1_enable(int on);                       /* page 7's switch (UI thread): off = every claim returns 0, from the
                                                  * end of the block it was moved in (state leases go home there) */
uint32_t kn_l1_enabled(void);                    /* the switch's position */

#ifdef OP1_HOST
extern uint32_t kn_l1_host_tab[80], kn_l1_host_lander;   /* the test's table (40 pointers, 40 sizes) and option */
extern uint8_t kn_l1_host_mem[], kn_l1_host_top[];        /* stand in for 0xFF900820.. and 0xFF907E00.. */
extern int16_t kn_l1_host_sine[];                         /* stands in for OP1_SINE_TABLE */
#endif
#endif
