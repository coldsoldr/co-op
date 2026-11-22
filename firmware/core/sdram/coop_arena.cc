/*
 * coop-fw e12: the plugin arena (core/include/coop_arena.h, docs/plugins.md "e12"). Main thread, boot only.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_arena.h"

extern "C" char __image_end[] __asm__("___image_end");                    /* coop_cb.ld */

/* a_top: the next free byte of range A (grows up); b_bot: the lowest taken byte of range B (grows down). */
static uint32_t g_a_top = COOP_ARENA_A_LO, g_b_bot = COOP_ARENA_B_HI;
static uint32_t g_sealed;
volatile uint32_t g_arena_takes, g_arena_refused, g_arena_bad;
/* Image-time test switch (--set-word g_coop_clip_max=N): the clipboard pool is at most N bytes, so a gate can lift
 * past it with a short recording. 0 = no limit. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_clip_max = 0; }

/* The run-time bounds: range A only above the image the linker placed (the script asserts it too), every block inside
 * its range. A block that fails them is never handed out. */
static int range_a_ok(void) { return (uint32_t)(uintptr_t)__image_end <= COOP_ARENA_A_LO; }

extern "C" void *coop_arena_take(uint32_t bytes)
{
    if (!bytes || bytes > COOP_ARENA_CAP || g_sealed) { g_arena_refused++; return 0; }
    uint32_t n = coop_arena_round(bytes), at = 0;
    if (range_a_ok() && g_a_top >= COOP_ARENA_A_LO && g_a_top <= COOP_ARENA_A_HI && COOP_ARENA_A_HI - g_a_top >= n) {
        at = g_a_top; g_a_top += n;
    } else if (g_b_bot <= COOP_ARENA_B_HI && g_b_bot >= COOP_ARENA_B_LO && g_b_bot - COOP_ARENA_B_LO >= n) {
        g_b_bot -= n; at = g_b_bot;
    } else { g_arena_refused++; return 0; }
    if ((at & (COOP_ARENA_ALIGN - 1u))
        || !((at >= COOP_ARENA_A_LO && at + n <= COOP_ARENA_A_HI) || (at >= COOP_ARENA_B_LO && at + n <= COOP_ARENA_B_HI))) {
        g_arena_bad++; return 0;                                           /* cannot happen: the pointers were checked above */
    }
    for (uint32_t i = 0; i < n; i += 4) *(volatile uint32_t *)(uintptr_t)(at + i) = 0;
    g_arena_takes++;
    return (void *)(uintptr_t)at;
}

extern "C" coop_arena_mark_t coop_arena_mark(void) { coop_arena_mark_t m = { g_a_top, g_b_bot }; return m; }
extern "C" void coop_arena_reset(coop_arena_mark_t m)
{
    if (g_sealed) return;
    if (m.a >= COOP_ARENA_A_LO && m.a <= g_a_top) g_a_top = m.a;
    if (m.b <= COOP_ARENA_B_HI && m.b >= g_b_bot) g_b_bot = m.b;
}
extern "C" void coop_arena_seal(void) { __asm__ __volatile__("ssync;" ::: "memory"); g_sealed = 1; }
extern "C" uint32_t coop_arena_used(void) { return (g_a_top - COOP_ARENA_A_LO) + (COOP_ARENA_B_HI - g_b_bot); }
extern "C" void coop_arena_stats(uint32_t *used_a, uint32_t *used_b, uint32_t *sealed)
{
    *used_a = g_a_top - COOP_ARENA_A_LO; *used_b = COOP_ARENA_B_HI - g_b_bot; *sealed = g_sealed;
}
/* The clipboard's right-channel pool, COOP_POOL_LO upwards: the floor until the scan has ended, then everything of
 * region B below the arena's lowest block. */
extern "C" uint32_t coop_clip_pool_bytes(void)
{
    uint32_t b = g_sealed ? g_b_bot : COOP_ARENA_B_LO;
    if (b < COOP_ARENA_B_LO || b > COOP_ARENA_B_HI) b = COOP_ARENA_B_LO;
    uint32_t n = b - COOP_POOL_LO, cap = g_coop_clip_max;
    return (cap && cap < n) ? cap : n;
}
