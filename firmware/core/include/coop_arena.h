/*
 * coop-fw e12: coop's own plugin memory (core/sdram/coop_arena.cc, docs/plugins.md "e12").
 *
 * Until e11 every plugin byte came from the stock heap 0, beside the stock tape's 12 MiB cache (the e6 boot hang, the
 * e7 budget). e12 takes none of it: the plugin images, the engines' voice state, the screen objects, the control
 * tables and the effect position buffers come from the SDRAM coop already owns, as one bump arena over two ranges:
 *
 *   A   COOP_ARENA_A_LO .. COOP_ARENA_A_HI   region A above the core's image (the linker script asserts the image
 *                                            ends below A_LO), filled first, upwards
 *   B   COOP_ARENA_B_LO .. COOP_ARENA_B_HI   the upper 2.5 MiB of region B, filled downwards from region A's start
 *                                            once A cannot hold a block
 *
 * Region B's lower part, COOP_POOL_LO .. COOP_ARENA_B_LO (1.5 MiB), is the stereo clipboard's floor (tape_edit.cc);
 * the clipboard also keeps whatever of B the arena did not take by the end of the boot scan (coop_arena_seal). The
 * floor's first MiB doubles as the boot scan's file staging block (the clipboard is empty then).
 *
 * Blocks are 32-byte aligned and sized, zeroed when taken, never freed; a failed load gives its blocks back by
 * resetting to a mark. Everything here runs on the main thread during the boot scan; after the seal nothing is taken.
 * Addresses: te-op1-cfw docs/memory-map.md §6 (regions A and B, free in R246), tools/inject_app.py REGION_START / END.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_ARENA_H
#define COOP_ARENA_H
#include <stdint.h>

#define COOP_POOL_LO     0x02800000u                 /* region B: the clipboard pool's base (always) */
#define COOP_ARENA_B_LO  0x02980000u                 /* the clipboard's floor ends here: 1.5 MiB */
#define COOP_ARENA_B_HI  0x02C00000u
#define COOP_ARENA_A_LO  0x02CE0000u                 /* coop_cb.ld: the core's image must end at or below this */
#define COOP_ARENA_A_HI  0x03000000u                 /* region A's end (the stock display buffer starts here) */
#define COOP_ARENA_ALIGN 32u
/* All of it: 3,276,800 + 2,621,440 = 5,898,240 bytes (5,760 KiB). The plugin memory budget (plugin_host.cc). */
#define COOP_ARENA_CAP   ((COOP_ARENA_A_HI - COOP_ARENA_A_LO) + (COOP_ARENA_B_HI - COOP_ARENA_B_LO))
#define COOP_CLIP_FLOOR  (COOP_ARENA_B_LO - COOP_POOL_LO)

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t a, b; } coop_arena_mark_t;
static inline uint32_t coop_arena_round(uint32_t n) { return (n + (COOP_ARENA_ALIGN - 1u)) & ~(COOP_ARENA_ALIGN - 1u); }
void *coop_arena_take(uint32_t bytes);               /* zeroed, 32-aligned; 0: no room (or sealed, or bytes 0) */
coop_arena_mark_t coop_arena_mark(void);
void coop_arena_reset(coop_arena_mark_t m);          /* give back everything taken since the mark */
void coop_arena_seal(void);                          /* end of the boot scan: the clipboard keeps the rest of region B */
uint32_t coop_arena_used(void);                      /* bytes taken, both ranges */
void coop_arena_stats(uint32_t *used_a, uint32_t *used_b, uint32_t *sealed);
uint32_t coop_clip_pool_bytes(void);                 /* the clipboard pool's size from COOP_POOL_LO (the floor before the seal) */
#ifdef __cplusplus
}
#endif
#endif
