/*
 * coop-fw p6c: code residency, lever 6 of docs/fx-data-placement-plan.md. The first device table (p6a) showed a
 * fifth to two fifths of every effect's block spent waiting for instruction fetches: coop's per-block code is
 * evicted from the 16 KB instruction cache between blocks. The Blackfin I-cache keeps one priority bit per line,
 * stored at the fill from the instruction CPLB's CPLB_LRUPRIO; replacement (ADSP-BF533 HRM rev 3.4 p. 6-21, the
 * BF52x core is the same): a high line replaces an invalid way, then the least recent low, then the least recent
 * high; a low replaces only invalid ways or lows; a set whose four ways are all high caches no low at all. So a
 * page's code marked high stays cached against every other page's code.
 *
 * coop's text is split at link time (coop_cb.ld): the per-block code (the audio callback, the stages, the kernels,
 * the effect renders, slot 2, the voice hooks, LFO 2's process, the tape tap) alone in the region-A page
 * 0x02c00000; everything the main loop and the key thread run (UI, settings, SysEx, loaders, the stats writer) in
 * region B 0x02810000, which has its own stock ICPLB page. The switch below (the settings menu's "code prio" row)
 * sets CPLB_LRUPRIO on the hot page's entry in R246's instruction CPLB table (L1 data A 0xFF802CCC, what the stock
 * CPLB manager reloads a slot from) and on the live ICPLB slot holding the page; off clears both and pulses
 * IMEM_CONTROL LRUPRIORST so no stale high is left. Only the hot page is marked: were the cold code high too, every
 * coop line ever fetched would stay until another coop line evicted it, sets would fill with four highs, and the
 * stock's own SDRAM code (the UI, the main loop) would run uncached there. The hot set is kept small (the ~12 KB
 * rule) for the same reason; the owner judges it on the device by the stats' p0 column and the UI's feel, with
 * the switch to compare. Off at every boot, never persisted: a reboot is always stock.
 *
 * The CPLB write sequence is kennel's (hundekraft 3.1 kn_diag.c KD_SW_IC, GPL-3.0-or-later with the OP-1 firmware
 * permission; it ran on the unit): R246's own slot update order (ENICPLB off, SSYNC, the slot, ENICPLB on, SSYNC;
 * HRM: "CPLBs must be disabled using this bit prior to updating their descriptors"), interrupts masked, and, as
 * Linux lock.S does for every IMEM_CONTROL store, an SSYNC before and an 8-byte aligned store. Between the two
 * IMEM_CONTROL stores fetches are uncached and unchecked (no ICPLB while ENICPLB is 0): legal from SDRAM. The
 * table and the slot must hold exactly the expected stock value or the switch refuses ("ic -"), so a different
 * firmware state is never written. KeyHandler thread (the menu). No libc, no float.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define HOT_PAGE        0x02C00000u                                   /* coop_cb.ld: the hot text's 4 MB ICPLB page */
#define IC_STOCK        0x00031005u                                   /* CPLB_VALID | CPLB_USER_RD | CPLB_L1_CHBL | PAGE_SIZE_4MB: every SDRAM page in R246's table */
#define IC_LRUPRIO      0x00000100u                                   /* ICPLB_DATAx CPLB_LRUPRIO */
#define IC_TAB          ((volatile uint32_t *)0xFF802CCCu)            /* R246's instruction CPLB table: {addr, data} pairs, 0xFFFFFFFF ends it */
#define IC_TAB_N        24                                            /* entries looked at, at most (R246 has 18 + the end) */
#define IC_ADDR         ((volatile uint32_t *)0xFFE01100u)            /* the 16 live slots */
#define IC_DATA         ((volatile uint32_t *)0xFFE01200u)
#define IC_N            16

enum { CP_OFF, CP_ON, CP_NONE };                                      /* none: refused, the CPLBs were not as expected */
static volatile uint32_t s_state = CP_OFF;
volatile uint32_t g_codeprio_sets, g_codeprio_refused;               /* stats */

extern "C" void coop_icplb_write(volatile uint32_t *data, uint32_t v);
extern "C" void coop_lrupriorst_pulse(void);
__asm__(
    ".text\n"
    ".align 4\n"
    ".global _coop_icplb_write\n"
    ".type _coop_icplb_write, STT_FUNC\n"
    "_coop_icplb_write:\n"
    "    CLI R3;\n"
    "    P0 = R0;\n"
    "    P1.L = 0x1004;\n"
    "    P1.H = 0xFFE0;\n"
    "    R2 = [P1];\n"
    "    BITCLR (R2, 1);\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    [P0] = R1;\n"
    "    SSYNC;\n"
    "    BITSET (R2, 1);\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    STI R3;\n"
    "    RTS;\n"
    ".size _coop_icplb_write, .-_coop_icplb_write\n"
    ".align 4\n"
    ".global _coop_lrupriorst_pulse\n"
    ".type _coop_lrupriorst_pulse, STT_FUNC\n"
    "_coop_lrupriorst_pulse:\n"
    "    CLI R3;\n"
    "    P1.L = 0x1004;\n"
    "    P1.H = 0xFFE0;\n"
    "    R2 = [P1];\n"
    "    BITSET (R2, 13);\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    BITCLR (R2, 13);\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    STI R3;\n"
    "    RTS;\n"
    ".size _coop_lrupriorst_pulse, .-_coop_lrupriorst_pulse\n"
    ".previous\n");

static uint32_t irq_off(void) { uint32_t m; __asm__ __volatile__("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ __volatile__("sti %0;" : : "d"(m)); }

/* the hot page's entry in R246's table and its live slot (-1: not loaded now), both holding exactly `want` */
static int find(uint32_t want, volatile uint32_t **tab, int *slot)
{
    *tab = 0; *slot = -1;
    for (int i = 0; i < IC_TAB_N && IC_TAB[2 * i] != 0xFFFFFFFFu; i++)
        if (IC_TAB[2 * i] == HOT_PAGE) { *tab = &IC_TAB[2 * i + 1]; break; }
    if (!*tab || **tab != want) return 0;
    for (int s = 0; s < IC_N; s++)
        if (IC_ADDR[s] == HOT_PAGE) { if (IC_DATA[s] != want) return 0; *slot = s; break; }
    return 1;
}

extern "C" int coop_codeprio_state(void) { return (int)s_state; }      /* 0 off, 1 on, 2 refused */
extern "C" void coop_codeprio_set(int on)                               /* KeyHandler thread (the settings menu) */
{
    if (on ? s_state != CP_OFF : s_state != CP_ON) return;
    const uint32_t from = on ? IC_STOCK : IC_STOCK | IC_LRUPRIO, to = on ? IC_STOCK | IC_LRUPRIO : IC_STOCK;
    volatile uint32_t *tab; int slot;
    uint32_t m = irq_off();                                             /* no other writer of the table or the slots can run */
    int ok = find(from, &tab, &slot);
    if (ok) {
        *tab = to;
        __asm__ __volatile__("ssync;" ::: "memory");
        if (slot >= 0) coop_icplb_write(&IC_DATA[slot], to);
    }
    irq_on(m);
    if (!on && ok) coop_lrupriorst_pulse();                             /* no stale high is left behind */
    s_state = ok ? (on ? CP_ON : CP_OFF) : CP_NONE;
    if (ok) g_codeprio_sets++; else g_codeprio_refused++;
}
