/* Diagnostic counters (kennel/kn_diag.c, ARCHITECTURE.md "Diagnostic counters"): hardware event counters split like
 * the resource monitor's page 5, CPLB replacements, the worst and the average audio block (monitor page 7).
 * The ISR side is integers only, no libc: a few MMR / L1 loads at the split points that already read CYCLES.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#ifndef KN_DIAG_H
#define KN_DIAG_H
#include <stdint.h>

/* ---- performance monitor (Blackfin core MMRs 0xFFE08000 PFCTL, 0xFFE08100 PFCNTR0, 0xFFE08104 PFCNTR1) ----
 * counter 0: event 0x82 "code fill" (I-cache line fills / stalls), counter 1: event 0x9A "data cache fills, bank A"
 * (L1 data A is the only data cache on the OP-1). PFCNTx = 1 counts occurrences (misses), 0 counts the cycles the event
 * is active (stall cycles). Event table: Linux arch/blackfin/kernel/perf_event.c (ADI), bit layout bfin_pfmon.h. */
#define KD_PFCTL     0xFFE08000u
#define KD_PFCNTR0   0xFFE08100u
#define KD_PFCNTR1   0xFFE08104u
#define KD_EV_ICODE  0x82u
#define KD_EV_DFILL  0x9Au
/* page 7's green: what the two counters count. stall / count: the fill events above as stall cycles or occurrences;
 * diag-iso adds (event legend of Linux perf_event.c; 's' events counted as cycles, 'o' as occurrences):
 * mem  0x90 "processor to memory stalls" (s) / 0x98 "data stalls" (s)
 * dma  0x80 "code memory fetches stalled due to DMA" (o) / 0x97 "data collision stalls" (s)
 * wbuf 0x92 "data store buffer full stalls" (s) / 0x93 "data memory write buffer full stalls" (s) */
enum { KD_MODE_STALL, KD_MODE_COUNT, KD_MODE_MEM, KD_MODE_DMA, KD_MODE_WBUF, KD_MODES };
extern const uint8_t kd_mode_ev[KD_MODES][2], kd_mode_cnt[KD_MODES][2];   /* event, 1 = occurrences, per counter */

/* ---- CPLB manager's round-robin pointers (R246 L1 data, docs/firmware/clocks-memory.md 4.2): {first, victim, end},
 * each the address of a DCPLB_ADDRn / ICPLB_ADDRn register; the victim moves 4 bytes per replacement ---- */
#define KD_CPLB_D    0xFF802D74u
#define KD_CPLB_I    0xFF802D80u

/* metrics of a class / block: cycles, counter 0, counter 1, D-CPLB and I-CPLB replacements */
enum { KD_CY, KD_P0, KD_P1, KD_CD, KD_CI, KD_M };
enum { KD_INST, KD_FX, KD_REST, KD_ALL, KD_C };  /* plugin/iter synth render + note_on, synth/drum/master FX, the rest */

typedef struct { uint32_t p0, p1, cd, ci, n; } kd_snap;   /* n: audio blocks ended (kd_blocks); KD_SNAP_OFF: not taken */
extern volatile uint32_t kd_blocks;
/* The counters work only while page 7 shows: kd_live() (thread level, sysmon.c: every frame on page 7, its open, a push,
 * turning to it) keeps them on for KD_LIVE_BLOCKS more blocks (about 6 s at 689 blocks a second on the unit; the GUI
 * draws about 30 frames a second). kd_act: on for the block running now (set at kd_block_begin). Off, every tap below
 * is a load and a branch; the first block on again starts the worst block afresh and the average from that block. */
#define KD_LIVE_BLOCKS 4096u
#define KD_SNAP_OFF    0xFFFFFFFFu
extern volatile uint32_t kd_act;
void kd_live(void);

#ifdef OP1_HOST
extern uint32_t kd_host_pf[2], kd_host_cplbtab[2][3];   /* the test's counters and CPLB pointers {first, victim, end} */
extern uint16_t kd_host_amgctl;                  /* stands in for EBIU_AMGCTL */
extern uint32_t kd_host_fill[4];          /* K1b: the cycles a host "fill" takes per bank (+ 30 when dirty) */
extern uint32_t kd_host_imem, kd_host_icplb_addr[16], kd_host_icplb_data[16], kd_host_icplb_tab[48];   /* KD_SW_IC */
extern uint32_t kd_host_ic_writes, kd_host_ic_bad, kd_host_ic_pulses;   /* slot writes, writes with ENICPLB on, LRUPRIORST pulses */
static inline void kd_snap_take(kd_snap *s)
{ s->p0 = kd_host_pf[0]; s->p1 = kd_host_pf[1]; s->cd = kd_host_cplbtab[0][1]; s->ci = kd_host_cplbtab[1][1]; s->n = kd_blocks; }
#else
extern volatile uint32_t kd_pf_live;            /* set by kd_init on the OP-1 (not on bcore: harness/tier2/boot.c) */
static inline void kd_snap_take(kd_snap *s)
{
    if (kd_pf_live) { s->p0 = *(volatile uint32_t *)KD_PFCNTR0; s->p1 = *(volatile uint32_t *)KD_PFCNTR1; }
    else s->p0 = s->p1 = 0;
    s->cd = ((volatile uint32_t *)KD_CPLB_D)[1]; s->ci = ((volatile uint32_t *)KD_CPLB_I)[1];
    s->n = kd_blocks;
}
#endif
/* the dispatcher's taps: a snapshot only while counting (kd_act), else just the mark that none was taken */
static inline void kd_snap_now(kd_snap *s) { if (kd_act) kd_snap_take(s); else s->n = KD_SNAP_OFF; }

/* ISR side (dispatcher.c, screens/sysmon_core.c) */
void kd_charge(int cls, const kd_snap *s0, uint32_t cycles);   /* a timed call of class cls ended */
void kd_voice(int engine, uint32_t cycles);      /* one voice render (engine order, 0 = stock iter) */
void kd_note_on(int voice, int engine, uint32_t cycles);
void kd_block_begin(void);
void kd_block_end(uint32_t cycles, uint32_t nominal, uint32_t fx_calls);

/* ---- page 7's switches: white selects one, orange flips it (right on, left off). None is persisted: every boot
 * starts with the default in brackets.
 *   KD_SW_L1   the core L1 pool for every plugin (kennel/kn_l1.c)                                     [on]
 *   KD_SW_STK  R246's audio callback on its own L1 stack (kennel/kn_astack.c)                         [off]
 *   KD_SW_CDP  EBIU_AMGCTL bit 8 CDPRIO: 1 = DMA wins external-bus conflicts against the core, 0 = the core wins
 *              (ADI BF52x HRM / Linux Kconfig C_CDPRIO "DMA has priority over core for ext. accesses")  [as R246 left it: 1]
 *   KD_SW_K1B  the per-bank SDRAM fill-latency probe at each block's start (below)                      [off]
 *   KD_SW_IC   I-cache LRU priority for plugin code: CPLB_LRUPRIO on the code arena's instruction CPLB (below) [off] ---- */
enum { KD_SW_L1, KD_SW_STK, KD_SW_CDP, KD_SW_K1B, KD_SW_IC, KD_SWS };
#define KD_AMGCTL    0xFFC00A00u                 /* 16-bit MMR */
#define KD_CDPRIO    0x0100u

/* ---- KD_SW_IC: plugin code first in the instruction cache (ARCHITECTURE.md "Diagnostic counters", switch ic).
 * The I-cache (16 KB, 4 ways x 128 sets, 32-byte lines) keeps one priority bit per line, stored from the line's
 * ICPLB_DATAx CPLB_LRUPRIO at the fill. Replacement (ADSP-BF533 HRM rev 3.4 p. 6-21, the BF52x core is the same): a
 * high line replaces an invalid way, then the least recent low, then the least recent high; a low replaces only
 * invalid ways or lows; if all four ways of the set are high, a low is fetched but NOT cached. IMEM_CONTROL bit 13
 * LRUPRIORST = 1 turns every cached high into a low (it must be 0 again for new fills to store the bit, HRM p. 6-9).
 * On: R246's instruction CPLB table entry for the code arena's 4 MB page (kn_boot.c kn_arena_lo: a heap-0 page
 * 0x00000000 / 0x00400000 / 0x00800000 / 0x00C00000, or 0x02800000 for the fixed KN_ARENA_LO..HI; refused when the
 * arena spans two pages; in R246's table at 0xFF802CCC, docs/firmware/clocks-memory.md 4.2) and the live ICPLB slot
 * holding that page get LRUPRIO: 0x00031005 -> 0x00031105 (every SDRAM page has 0x00031005 in R246's table).
 * Instruction fetches from a heap-0 page are plugin code only (heap 0 holds data and, since 2026-10-31, the arena). Every plugin's code (render, note_on, screens) then fills as high: R246's code
 * (page 0x01800000) and kennel's core (0x01C00000) can no longer evict it. The live slot is written the way R246's
 * own CPLB manager writes it (0xFFA09648..0xFFA096AA): ENICPLB off, SSYNC, ICPLB_DATAx, ENICPLB on, SSYNC; plus
 * interrupts masked and an 8-byte aligned store to IMEM_CONTROL behind an SSYNC (Linux lock.S). If R246's CPLB
 * manager ever reloads that slot it reloads the patched table entry. Off: both back to 0x00031005, then an
 * LRUPRIORST pulse (1, then 0) so no stale high is left. Refused ("ic -") when the table entry or the live slot is not
 * exactly the expected value. Off at every boot (R246 loads its table from the LDR), never persisted, nothing written
 * at boot, nothing in the audio path. Worst case: sets whose four ways hold plugin lines fetch R246's code that maps
 * there uncached (a low is never cached in a full-high set): REST and stock engines get slower; switching ic off
 * (or a reboot) undoes it. ---- */
#define KD_IMEM_CONTROL 0xFFE01004u
#define KD_ICPLB_ADDR0  0xFFE01100u
#define KD_ICPLB_DATA0  0xFFE01200u
#define KD_ICPLB_N      16
#define KD_ICPLB_TAB    0xFF802CCCu              /* R246's instruction CPLB table: {addr, data} pairs, 0xFFFFFFFF ends it */
#define KD_ICPLB_TAB_N  24                       /* entries looked at, at most (R246 has 18 + the end) */
#define KD_IC_PAGE      0x02800000u              /* the 4 MB page of the fixed code arena (kn_mem.h KN_ARENA_LO) */
#define KD_IC_STOCK     0x00031005u              /* CPLB_VALID | CPLB_USER_RD | CPLB_L1_CHBL | PAGE_SIZE_4MB */
#define KD_LRUPRIO      0x00000100u              /* ICPLB_DATAx CPLB_LRUPRIO (Linux def_LPBlackfin.h) */
#define KD_LRUPRIORST   0x00002000u              /* IMEM_CONTROL bit 13 */
#define KD_ENICPLB      0x00000002u              /* IMEM_CONTROL bit 1 */
enum { KD_IC_OFF, KD_IC_ON, KD_IC_NONE };       /* none: refused, the CPLBs were not as expected */

/* ---- K1b: what one SDRAM line fill costs, per SDRAM internal bank (address bits 25:24 with the OP-1's 64 MB,
 * 10-bit-column SDBCTL; OP1-Emulator run/l1-opt-2026-10-30/systemic/report.md section 7). At kd_block_begin, with
 * interrupts masked, one kind and one bank per block (eight blocks for the set), KD_K1B_LINES lines each:
 *   clean: FLUSHINV the line F and two other lines of its cache set (C1 = F + 256 KB, C2 = F + 512 KB, other SDRAM
 *          rows of the same bank), load C1 and C2 (the set now holds two clean lines), then time a dependent load of
 *          F (minus the same load again, now a hit): a fill with a clean victim, after a row change in that bank.
 *   dirty: the same with kennel's own line V (core BSS, bank 1, same cache set) loaded and stored first, so V is the
 *          set's least recent line and dirty: F's fill evicts it (the victim goes back to bank 1).
 * F, C1, C2 are only read (FLUSHINV writes a dirty line back before dropping it, so nothing is lost); the only store
 * is to V. Lines 16 KB apart (same set whatever the set mapping) in 4 MB CPLB pages R246 keeps hot. ---- */
#define KD_K1B_LINES 8
#define KD_BANKS     4
enum { KD_K1B_CLEAN, KD_K1B_DIRTY, KD_K1B_KINDS };
extern const uint32_t kd_k1b_base[KD_BANKS];

void kd_init(void);                              /* kn_boot_hook: counters on, CPLB pointer ranges */

/* page 7 (screens/sysmon.c, exported in kennel/kn_exports.c) */
typedef struct {
    uint32_t v[KD_C][KD_M];                       /* per class: cycles, counters, CPLB replacements */
    uint32_t nominal, notes, fx_calls;
} kd_block;
typedef struct {
    kd_block worst, avg;                          /* avg: values x 1 (from a x256 running sum) */
    uint32_t voice_max, voice_engine, on_max, on_engine;   /* since the reset */
    uint32_t blocks, cplb_d, cplb_i;              /* since the reset */
    uint32_t mode;
    uint32_t mode_cnt[2];                         /* the mode's counters: 1 = occurrences, 0 = cycles */
    uint32_t l1_on, l1_ready;                     /* the L1 pool (kn_l1.c): switched on, taken over at boot */
    uint32_t sw[KD_SWS];                          /* the switches' positions (KD_SW_STK: kn_astack_state() == KN_AS_ON) */
    uint32_t stk_state, stk_used;                 /* kn_astack.h KN_AS_*, deepest use since the last fill (bytes) */
    uint32_t k1b_avg[KD_K1B_KINDS][KD_BANKS], k1b_max[KD_K1B_KINDS][KD_BANKS], k1b_n[KD_K1B_KINDS][KD_BANKS];
    uint32_t ic_state;                            /* KD_IC_* */
} kd_view_t;
void kd_view(kd_view_t *v);
void kd_reset(void);                             /* page 7 push: worst block and maxima start again */
void kd_set_mode(int mode);
void kd_set_l1(int on);                          /* the L1 pool on / off (= kd_set_switch(KD_SW_L1, on)) */
void kd_set_switch(int sw, int on);              /* page 7 orange on the switch white selected */
const char *kd_engine_name(int order);

#endif
