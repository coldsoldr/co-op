/*
 * coop-fw L1 data pool (milestone T4): the per-block hot data of the coop stages lives in L1 data bank B instead
 * of SDRAM, so the audio interrupt stops paying a cache miss (100-190 cycles, more under display DMA) for every
 * line it touches. The device showed the voice hook at 13x its emulated cost with its buses in SDRAM
 * (docs/performance.md, T3g).
 *
 * The region is the one kennel's L1 pool uses (kennel/kn_l1.c): the stock Lander game's 40 sound buffers,
 * 0xFF90081C..0xFF906908, with their table (pointers at 0xFF906908, sizes at 0xFF9069A8) right after them. The pool
 * is taken only when the table is exactly the stock layout (40 buffers back to back, word aligned, ending at the
 * table); then every size is set to 0 and every pointer to a dummy, as kennel does, so the game never reads or
 * writes the buffers again (its sounds go silent in coop). Anything else: no pool, every user keeps its SDRAM
 * copy, nothing changes. A bump allocator; nothing is ever freed.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_l1.h"

#define L1_TAB_N   40
#define L1_FIRST   0xFF90081Cu                  /* entry 0's buffer */
#define L1_END     0xFF906908u                  /* entry 39's buffer end = the table itself */
#define L1_POOL    0xFF900820u                  /* first 32-byte line in the range */
#define L1_LIMIT   0xFF902900u                  /* the bump pool ends where the sine table starts */
#define SINE_SDRAM ((const uint32_t *)0x0101E5E8u)   /* OP1_SINE_TABLE: 8192 x int16 (kennel op1_plugin.h) */
#define SINE_L1    ((uint32_t *)0xFF902900u)    /* its L1 copy (tools/l1_stereo_insert.py SINE_L1): 16 KiB to 0xFF906900 */
#define SINE_WORDS 4096
#define TAB_PTR    ((volatile uint32_t *)0xFF906908u)
#define TAB_SIZE   ((volatile uint32_t *)0xFF9069A8u)

volatile uint32_t g_l1_state, g_l1_used, g_l1_refused, g_l1_sine_copied;
static uint32_t l1_dummy[8];                    /* what the Lander's table points at (size 0: never read) */
static uint32_t l1_next;
static uint32_t l1_top = L1_LIMIT;             /* e4: plugin leases come down from the top, coop's own blocks go up */

static int l1_check_and_take(void)
{
    uint32_t at = L1_FIRST;
    for (int i = 0; i < L1_TAB_N; i++) {
        at = (at + 3u) & ~3u;
        if (TAB_PTR[i] != at || TAB_SIZE[i] == 0 || TAB_SIZE[i] > 0x2000u) return 0;
        at += TAB_SIZE[i];
    }
    if (at != L1_END) return 0;
    for (int i = 0; i < L1_TAB_N; i++) { TAB_SIZE[i] = 0; TAB_PTR[i] = (uint32_t)(uintptr_t)l1_dummy; }
    return 1;
}

/* The stock engines' sine lookups are retargeted at SINE_L1 by the package (T4c), so the copy must exist before the
 * first stock audio block: called from coop_stereo_block_start (before the stock callback) and from the tape mount
 * hook at boot. Unconditional: it does not depend on the pool (the slot is inside the Lander's buffers either way). */
extern "C" void coop_l1_sine_copy(void)
{
    if (g_l1_sine_copied) return;
    for (int i = 0; i < SINE_WORDS; i++) SINE_L1[i] = SINE_SDRAM[i];
    g_l1_sine_copied = 1;
}

extern "C" int coop_l1_state(void)
{
    if (g_l1_state == 0) {
        g_l1_state = l1_check_and_take() ? 1 : 2;
        l1_next = L1_POOL;
    }
    return (int)g_l1_state;
}

extern "C" void *coop_l1_alloc(uint32_t bytes)
{
    if (coop_l1_state() != 1) { g_l1_refused++; return 0; }
    uint32_t n = (bytes + 31u) & ~31u;
    if (l1_next + n > l1_top) { g_l1_refused++; return 0; }
    uint8_t *p = (uint8_t *)(uintptr_t)l1_next;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
    l1_next += n;
    g_l1_used = l1_next - L1_POOL;
    return p;
}

/* ---- e4: L1 for plugins (docs/engines.md "L1 for engines") ----
 * The lease: a synth engine's init (main thread, at load) may take a zeroed, 32-byte aligned block of the pool that it
 * keeps for the session. Leases come down from the top of the pool while coop's own blocks go up from the bottom
 * (some are taken after the plugin scan: the e4 gate found 3,200 bytes of them then), and a lease never leaves coop
 * less than COOP_L1_CORE_RESERVE bytes in all (coop used 3,200 with saw loaded [C op1emu e4]); engines may lease at
 * most COOP_L1_ENGINE_MAX together; what is left after that is for later effect plugins. Refused (0) from anywhere but an engine's init, past a limit, or with
 * no pool: the plugin keeps its SDRAM path.
 *
 * The scratch: while a plugin engine is the live engine the stock voice objects are iter's six (0xFFB00000 + 6 x 0x24
 * = 0xD8 bytes) and the stock effect objects start at 0xFFB00600, so 0xFFB000E0..0xFFB005FF of the L1 scratchpad is
 * unused [C op1emu e3, a saw note: no store to 0xFFB000D8..0xFFB005FF after the engine change; kennel rings used the
 * same window]. A plugin may use it as per-call scratch inside its own note_on / render: nothing is kept between
 * calls, the stock writes it when another engine is chosen. */
#define COOP_L1_CORE_RESERVE 3584u
#define COOP_L1_ENGINE_MAX   4096u
#define SCRATCH_BASE         0xFFB000E0u
#define SCRATCH_BYTES        0x520u
volatile uint32_t g_l1_leased, g_l1_lease_refused, g_l1_leases;
volatile int g_l1_lease_open;                   /* engine_host.cc sets it around an engine's init */

extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_l1_no_lease = 0; }   /* test images: refuse every lease */
extern "C" void *coop_l1_lease(uint32_t bytes)
{
    uint32_t n = (bytes + 31u) & ~31u;
    if (g_coop_l1_no_lease || !g_l1_lease_open || !n || coop_l1_state() != 1 || g_l1_leased + n > COOP_L1_ENGINE_MAX) { g_l1_lease_refused++; return 0; }
    uint32_t floor = l1_next > L1_POOL + COOP_L1_CORE_RESERVE ? l1_next : L1_POOL + COOP_L1_CORE_RESERVE;   /* coop's share */
    if (l1_top < floor + n) { g_l1_lease_refused++; return 0; }
    l1_top -= n;
    uint8_t *p = (uint8_t *)(uintptr_t)l1_top;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
    g_l1_leased += n; g_l1_leases++;
    return p;
}

extern "C" void *coop_l1_scratch(uint32_t *bytes)
{
    if (bytes) *bytes = SCRATCH_BYTES;
    return (void *)(uintptr_t)SCRATCH_BASE;
}

/* the pool's free bytes, for the stats */
extern "C" uint32_t coop_l1_free(void) { return coop_l1_state() == 1 ? l1_top - (l1_next < L1_POOL ? L1_POOL : l1_next) : 0; }
