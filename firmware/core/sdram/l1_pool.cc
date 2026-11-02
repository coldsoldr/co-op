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
#define L1_LIMIT   0xFF906900u                  /* last line boundary below the table */
#define TAB_PTR    ((volatile uint32_t *)0xFF906908u)
#define TAB_SIZE   ((volatile uint32_t *)0xFF9069A8u)

volatile uint32_t g_l1_state, g_l1_used, g_l1_refused;
static uint32_t l1_dummy[8];                    /* what the Lander's table points at (size 0: never read) */
static uint32_t l1_next;

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
    if (l1_next + n > L1_LIMIT) { g_l1_refused++; return 0; }
    uint8_t *p = (uint8_t *)(uintptr_t)l1_next;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
    l1_next += n;
    g_l1_used = l1_next - L1_POOL;
    return p;
}
