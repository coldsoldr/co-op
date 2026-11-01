/* Diagnostic counters (ARCHITECTURE.md "Diagnostic counters", kn_diag.h): why plugin synths / FX spike on the unit.
 * The performance monitor counts code fills (event 0x82) and data cache fills of bank A (0x9A), as stall cycles or as
 * occurrences (page 7's mode). The dispatcher's timed calls (synth render / note_on = INST, FX renders = FX) and the
 * audio callback (sysmon_core.c) take a snapshot of both counters and of the CPLB manager's victim pointers where they
 * already read CYCLES; REST is the callback minus INST and FX. Per block: the worst block since a reset (by cycles) and
 * a running average (x256 sums, >> 8), the slowest single voice render and note_on. Monitor page 7 shows them.
 * (The hang ring, watchdog and hang log of 3.1i were removed on 2026-10-29: too complicated for what they gave.)
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "kn_diag.h"
#include "kn_l1.h"
#include "kn_astack.h"
#include "../kn_mem.h"

/* ---------------- hardware ---------------- */
#ifdef OP1_HOST
uint32_t kd_host_pf[2], kd_host_cplbtab[2][3], kd_host_pfctl;
uint16_t kd_host_amgctl = 0x00F0 | KD_CDPRIO;
uint32_t kd_host_fill[4];
uint32_t kd_host_imem = 0x6, kd_host_icplb_addr[16], kd_host_icplb_data[16], kd_host_icplb_tab[48];
uint32_t kd_host_ic_writes, kd_host_ic_bad, kd_host_ic_pulses;
#define AMGCTL          (kd_host_amgctl)
#define SSYNC()         ((void)0)
#define PFCTL_W(v)      (kd_host_pfctl = (v))
#define CPLB_TAB(i)     (kd_host_cplbtab[i])
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
#define AMGCTL          (*(volatile uint16_t *)KD_AMGCTL)
#define SSYNC()         __asm__ volatile ("ssync;" : : : "memory")
#define PFCTL_W(v)      (*(volatile uint32_t *)KD_PFCTL = (v))
#define CPLB_TAB(i)     ((volatile uint32_t *)((i) ? KD_CPLB_I : KD_CPLB_D))
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
#endif

/* ---------------- state ---------------- */
/* touched every block whether page 7 shows or not: in the core's hot lines (kn_mem.h kn_hot) */
static volatile int ready kn_hot;               /* kd_init ran */
volatile uint32_t kd_act kn_hot;                /* counting this block (kn_diag.h) */
static volatile uint32_t kd_until kn_hot;       /* kd_blocks value until which page 7 counts as showing */
static uint32_t kd_fresh kn_hot;                /* the first block on after off: start the worst block and the average */
static uint32_t pf_mode = KD_MODE_STALL;
static uint32_t span[2];                        /* CPLB victim ranges (bytes) */
static kd_snap blk0 kn_hot;
static uint32_t acc[2][KD_M] kn_hot;            /* INST, FX of the running block */
static uint32_t blk_notes kn_hot;
static kd_block worst;
static uint32_t avg_sum[KD_C][KD_M];            /* x256 */
static uint32_t voice_max, voice_eng, on_max, on_eng;
static uint32_t since_blocks, since_cd, since_ci;
static volatile uint32_t reset_req;
const uint8_t kd_mode_ev[KD_MODES][2] = { { KD_EV_ICODE, KD_EV_DFILL }, { KD_EV_ICODE, KD_EV_DFILL }, { 0x90, 0x98 },
                                          { 0x80, 0x97 }, { 0x92, 0x93 } };
const uint8_t kd_mode_cnt[KD_MODES][2] = { { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 0 }, { 0, 0 } };
volatile uint32_t kd_blocks kn_hot;             /* audio blocks ended: a timed call that spans one was preempted by it */
static uint32_t split kn_hot;                   /* the last kd_charge spanned an audio block: not counted */

static uint32_t steps(int i, uint32_t a, uint32_t b)   /* replacements between two victim pointers */
{
    if (a == b) return 0;
    if (!span[i]) return 1;
    uint32_t d = b - a;
    if ((int32_t)d < 0) d += span[i];
    return d >> 2;
}

/* ---------------- performance monitor ---------------- */
#ifndef OP1_HOST
volatile uint32_t kd_pf_live;
extern const int kd_no_hw __attribute__((weak));        /* defined by the bcore matrix guest only */
#define PF_OK() (&kd_no_hw == 0)
#else
#define PF_OK() 1
#endif
static void pf_set(uint32_t mode)
{
    pf_mode = mode;
    if (!PF_OK()) return;
    /* PFPWR (0) | PFCEN0 = 3 (3) | PFMON0 (5) | PFCEN1 = 3 (14) | PFMON1 (16) | PFCNT0 (24) | PFCNT1 (25) */
    PFCTL_W(1u | 3u << 3 | (uint32_t)kd_mode_ev[mode][0] << 5 | 3u << 14 | (uint32_t)kd_mode_ev[mode][1] << 16 |
            (uint32_t)kd_mode_cnt[mode][0] << 24 | (uint32_t)kd_mode_cnt[mode][1] << 25);

#ifndef OP1_HOST
    kd_pf_live = 1;
#endif
}

/* ---------------- ISR side ---------------- */
void kd_charge(int cls, const kd_snap *s0, uint32_t cycles)
{
    if (!kd_act) return;
    kd_snap s; kd_snap_take(&s);                /* s0 taken while off (KD_SNAP_OFF): not counted, as a split */
    split = s.n != s0->n;                       /* note_on on the key thread, interrupted by the audio callback */
    if (split) return;
    uint32_t *a = acc[cls];
    a[KD_CY] += cycles; a[KD_P0] += s.p0 - s0->p0; a[KD_P1] += s.p1 - s0->p1;
    a[KD_CD] += steps(0, s0->cd, s.cd); a[KD_CI] += steps(1, s0->ci, s.ci);
}
void kd_voice(int engine, uint32_t cycles)
{
    if (!kd_act) return;
    if (cycles > voice_max) { voice_max = cycles; voice_eng = (uint32_t)engine; }
}
void kd_note_on(int voice, int engine, uint32_t cycles)
{
    (void)voice;
    if (!kd_act) return;
    blk_notes++;
    if (split) return;
    if (cycles > on_max) { on_max = cycles; on_eng = (uint32_t)engine; }
}

/* ---------------- K1b: SDRAM fill latency per bank (kn_diag.h) ---------------- */
/* F of line k in bank b: kd_k1b_base[b] (16 KB aligned) + V's offset in its 16 KB + k x 16 KB; C1 / C2 at +256 / +512 KB.
 * Every address stays inside the base's 4 MB CPLB page and its bank: heap 0 (thread stacks' page; the code arena's
 * bank since 2026-10-31), the core image, the fixed arena range (framebuffer A's bank), heap 1 (framebuffer B's page). */
const uint32_t kd_k1b_base[KD_BANKS] = { 0x00100000u, 0x01E00000u, 0x02980000u, 0x03100000u };
static uint32_t k1b_victim[8] __attribute__((aligned(32)));   /* V: kennel's own line (bank 1, core BSS) */
static volatile uint32_t k1b_on kn_hot;
static uint32_t k1b_step;                       /* which kind / bank this block: bits 1..0 bank, bit 2 kind */
static uint32_t k1b_sum[KD_K1B_KINDS][KD_BANKS], k1b_max[KD_K1B_KINDS][KD_BANKS], k1b_n[KD_K1B_KINDS][KD_BANKS];
#ifdef OP1_HOST
static uint32_t k1b_line(uint32_t bank, uint32_t k, int dirty)
{ (void)k; if (dirty) k1b_victim[0]++; return kd_host_fill[bank] + (dirty ? 30u : 0u); }
#else
static inline void flushinv(uint32_t a) { __asm__ volatile ("flushinv [%0];" : : "a"(a) : "memory"); }
static inline uint32_t timed_load(uint32_t a)   /* cycles of a load whose value the next instruction needs */
{
    uint32_t t0, t1, v;
    __asm__ volatile ("%0 = CYCLES;\n\t%2 = [%3];\n\t%2 += 1;\n\t%1 = CYCLES;"
                      : "=&d"(t0), "=&d"(t1), "=&d"(v) : "a"(a) : "memory");
    return t1 - t0;
}
static uint32_t k1b_line(uint32_t bank, uint32_t k, int dirty)   /* one fill's cycles (the hit's taken off) */
{
    const uint32_t v = (uint32_t)(uintptr_t)k1b_victim;
    const uint32_t f = kd_k1b_base[bank] + (v & 0x3FE0u) + k * 0x4000u, c1 = f + 0x40000u, c2 = f + 0x80000u;
    (void)*(volatile uint32_t *)c1;              /* a CPLB miss (if any) happens here, not in the timed load */
    flushinv(f); flushinv(c1); flushinv(c2);
    SSYNC();
    if (dirty) { uint32_t x = *(volatile uint32_t *)v; *(volatile uint32_t *)v = x + 1; }
    else (void)*(volatile uint32_t *)c1;
    (void)*(volatile uint32_t *)c2;
    const uint32_t miss = timed_load(f), hit = timed_load(f);
    return miss > hit ? miss - hit : 0;
}
#endif
static void k1b_run(void)
{
    const uint32_t bank = k1b_step & 3, kind = (k1b_step >> 2) & 1;
    k1b_step++;
    uint32_t m = irq_off();
    uint32_t c[KD_K1B_LINES];
    for (uint32_t k = 0; k < KD_K1B_LINES; k++) c[k] = k1b_line(bank, k, (int)kind);
    irq_on(m);
    for (uint32_t k = 0; k < KD_K1B_LINES; k++) {
        uint32_t *s = &k1b_sum[kind][bank];
        if (k1b_n[kind][bank] == 0 && k == 0) *s = c[k] << 8;   /* the running average starts at the first sample */
        else *s += c[k] - (*s >> 8);
        if (c[k] > k1b_max[kind][bank]) k1b_max[kind][bank] = c[k];
    }
    k1b_n[kind][bank]++;
}
static void k1b_clear(void)
{
    for (int i = 0; i < KD_K1B_KINDS; i++) for (int b = 0; b < KD_BANKS; b++) k1b_sum[i][b] = k1b_max[i][b] = k1b_n[i][b] = 0;
}

void kd_block_begin(void)
{
    if (!ready || (int32_t)(kd_until - kd_blocks) <= 0) { kd_act = 0; return; }   /* page 7 not showing */
    if (!kd_act) { kd_act = 1; kd_fresh = 1; }
    if (k1b_on) k1b_run();
    kd_snap_take(&blk0);
    for (int c = 0; c < 2; c++) for (int m = 0; m < KD_M; m++) acc[c][m] = 0;
    blk_notes = 0;
}

void kd_block_end(uint32_t cycles, uint32_t nominal, uint32_t fx_calls)
{
    kd_blocks++;
    if (!kd_act) return;                        /* page 7 not showing (or kd_init not run: kd_act stays 0) */
    kd_snap s; kd_snap_take(&s);
    uint32_t all[KD_M] = { cycles, s.p0 - blk0.p0, s.p1 - blk0.p1, steps(0, blk0.cd, s.cd), steps(1, blk0.ci, s.ci) };
    if (reset_req || kd_fresh) {
        reset_req = 0;
        worst.v[KD_ALL][KD_CY] = 0; voice_max = on_max = 0; voice_eng = on_eng = 0; since_blocks = since_cd = since_ci = 0;
        k1b_clear();
    }
    since_blocks++; since_cd += all[KD_CD]; since_ci += all[KD_CI];
    uint32_t v[KD_C][KD_M];
    for (int m = 0; m < KD_M; m++) {
        uint32_t i = acc[KD_INST][m], f = acc[KD_FX][m], t = all[m];
        v[KD_INST][m] = i; v[KD_FX][m] = f; v[KD_ALL][m] = t; v[KD_REST][m] = t > i + f ? t - i - f : 0;
    }
    if (cycles > worst.v[KD_ALL][KD_CY]) {
        for (int c = 0; c < KD_C; c++) for (int m = 0; m < KD_M; m++) worst.v[c][m] = v[c][m];
        worst.nominal = nominal; worst.notes = blk_notes; worst.fx_calls = fx_calls;
    }
    if (kd_fresh) {                             /* nothing counted while off: the average starts at this block */
        kd_fresh = 0;
        for (int c = 0; c < KD_C; c++) for (int m = 0; m < KD_M; m++) avg_sum[c][m] = v[c][m] << 8;
    } else
        for (int c = 0; c < KD_C; c++) for (int m = 0; m < KD_M; m++) avg_sum[c][m] += v[c][m] - (avg_sum[c][m] >> 8);
}

void kd_live(void) { kd_until = kd_blocks + KD_LIVE_BLOCKS; }   /* thread level: page 7 shows (kn_diag.h) */

void kd_init(void)
{
    for (int i = 0; i < 2; i++) {
        volatile uint32_t *t = CPLB_TAB(i);
        span[i] = t[2] > t[0] && t[2] - t[0] < 0x100 ? t[2] - t[0] : 0;
    }
    worst.v[KD_ALL][KD_CY] = 0; voice_max = on_max = 0; since_blocks = since_cd = since_ci = 0;
    for (int c = 0; c < KD_C; c++) for (int k = 0; k < KD_M; k++) avg_sum[c][k] = 0;
    pf_set(KD_MODE_STALL);
    k1b_on = 0; k1b_clear();
    kd_act = 0; kd_fresh = 0; kd_until = kd_blocks;   /* off until page 7 shows */
    ready = 1;
}

/* ---------------- KD_SW_IC: CPLB_LRUPRIO on the code arena's page (kn_diag.h) ---------------- */
_Static_assert((KN_ARENA_LO & ~0x3FFFFFu) == KD_IC_PAGE && ((KN_ARENA_HI - 1u) & ~0x3FFFFFu) == KD_IC_PAGE,
               "the fixed code arena is one 4 MB instruction CPLB page");
static volatile uint32_t ic_state = KD_IC_OFF;
/* the arena's 4 MB page (kn_boot.c kn_arena_lo / _hi: heap 0, or the fixed range's KD_IC_PAGE); 0 when the arena
 * spans two pages (then ic refuses: "ic -"). Host tests have no kn_boot.c: the fixed range. */
extern volatile uint32_t kn_arena_lo __attribute__((weak)), kn_arena_hi __attribute__((weak));
static uint32_t ic_page(void)
{
    const uint32_t lo = &kn_arena_lo ? kn_arena_lo : KN_ARENA_LO, hi = (&kn_arena_hi ? kn_arena_hi : KN_ARENA_HI) - 1u;
    return (lo & ~0x3FFFFFu) == (hi & ~0x3FFFFFu) ? lo & ~0x3FFFFFu : 0xFFFFFFFFu;
}
#ifdef OP1_HOST
#define IC_TAB          (kd_host_icplb_tab)
#define IC_ADDR         (kd_host_icplb_addr)
#define IC_DATA         (kd_host_icplb_data)
static void kd_icplb_write(volatile uint32_t *data, uint32_t v)   /* stands in for the asm below, step by step */
{
    kd_host_imem &= ~KD_ENICPLB;
    if (kd_host_imem & KD_ENICPLB) kd_host_ic_bad++;
    *data = v;
    kd_host_ic_writes++;
    kd_host_imem |= KD_ENICPLB;
}
static void kd_lrupriorst_pulse(void)
{
    kd_host_imem |= KD_LRUPRIORST;
    kd_host_imem &= ~KD_LRUPRIORST;
    kd_host_ic_pulses++;
}
#else
#define IC_TAB          ((volatile uint32_t *)KD_ICPLB_TAB)
#define IC_ADDR         ((volatile uint32_t *)KD_ICPLB_ADDR0)
#define IC_DATA         ((volatile uint32_t *)KD_ICPLB_DATA0)
void kd_icplb_write(volatile uint32_t *data, uint32_t v);
void kd_lrupriorst_pulse(void);
/* kd_icplb_write(&ICPLB_DATAn, v): the sequence R246's CPLB manager uses for a slot (0xFFA09648..0xFFA096AA: ENICPLB
 * off, SSYNC, the slot, ENICPLB on, SSYNC; the HRM: "CPLBs must be disabled using this bit prior to updating their
 * descriptors"), with interrupts masked and, as Linux lock.S does for every IMEM_CONTROL store, an SSYNC before and an
 * 8-byte aligned store (.align 8 pads with NOPs: 0x0000 is NOP). Instruction fetches between the two IMEM_CONTROL
 * stores are uncached (no CPLB, no cache): this code runs from SDRAM like Linux's non-L1 CPLB manager; no exception
 * can be raised there (no ICPLB check while ENICPLB is 0). Clobbers R0..R3, P0, P1 (call-clobbered).
 * kd_lrupriorst_pulse(): IMEM_CONTROL bit 13 set, then clear (HRM p. 6-9: 1 turns every cached high into a low, 0
 * again so new fills store the bit), the same store discipline. Top-level asm: the disassembly check is of this. */
__asm__(
    ".text\n"
    ".align 4\n"
    ".global _kd_icplb_write\n"
    ".type _kd_icplb_write, STT_FUNC\n"
    "_kd_icplb_write:\n"
    "    CLI R3;\n"
    "    P0 = R0;                        /* &ICPLB_DATAn */\n"
    "    P1.L = 0x1004;\n"
    "    P1.H = 0xFFE0;                  /* IMEM_CONTROL */\n"
    "    R2 = [P1];\n"
    "    BITCLR (R2, 1);                 /* ENICPLB off */\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    [P0] = R1;                      /* the slot's new data */\n"
    "    SSYNC;\n"
    "    BITSET (R2, 1);                 /* ENICPLB on */\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    STI R3;\n"
    "    RTS;\n"
    ".size _kd_icplb_write, .-_kd_icplb_write\n"
    ".align 4\n"
    ".global _kd_lrupriorst_pulse\n"
    ".type _kd_lrupriorst_pulse, STT_FUNC\n"
    "_kd_lrupriorst_pulse:\n"
    "    CLI R3;\n"
    "    P1.L = 0x1004;\n"
    "    P1.H = 0xFFE0;                  /* IMEM_CONTROL */\n"
    "    R2 = [P1];\n"
    "    BITSET (R2, 13);                /* LRUPRIORST: every cached high becomes a low */\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    BITCLR (R2, 13);                /* back to 0: new fills store LRUPRIO again */\n"
    "    SSYNC;\n"
    "    .align 8\n"
    "    [P1] = R2;\n"
    "    SSYNC;\n"
    "    CSYNC;\n"
    "    STI R3;\n"
    "    RTS;\n"
    ".size _kd_lrupriorst_pulse, .-_kd_lrupriorst_pulse\n"
    ".previous\n");
#endif
/* the arena page's entry in R246's table and its live slot (-1: not loaded now), both holding exactly `want` */
static int ic_find(uint32_t page, uint32_t want, volatile uint32_t **tab, int *slot)
{
    volatile uint32_t *t = IC_TAB;
    *tab = 0; *slot = -1;
    if (page == 0xFFFFFFFFu) return 0;
    for (int i = 0; i < KD_ICPLB_TAB_N && t[2 * i] != 0xFFFFFFFFu; i++)
        if (t[2 * i] == page) { *tab = &t[2 * i + 1]; break; }
    if (!*tab || **tab != want) return 0;
    for (int s = 0; s < KD_ICPLB_N; s++)
        if (IC_ADDR[s] == page) { if (IC_DATA[s] != want) return 0; *slot = s; break; }
    return 1;
}
static void ic_set(int on)                       /* thread level (page 7) */
{
    if (!PF_OK()) return;                        /* the bcore guest: no CPLBs */
    if (on ? ic_state != KD_IC_OFF : ic_state != KD_IC_ON) return;
    const uint32_t from = on ? KD_IC_STOCK : KD_IC_STOCK | KD_LRUPRIO, to = on ? KD_IC_STOCK | KD_LRUPRIO : KD_IC_STOCK;
    volatile uint32_t *tab; int slot;
    static uint32_t page;                        /* fixed when ic first goes on (the arena never moves after boot) */
    if (on) page = ic_page();
    uint32_t m = irq_off();                      /* no other writer of the table or the slots can run */
    const int ok = ic_find(page, from, &tab, &slot);
    if (ok) {
        *tab = to;                               /* (L1 data A) what R246's CPLB manager reloads the slot from */
        if (slot >= 0) kd_icplb_write(&IC_DATA[slot], to);
    }
    irq_on(m);
    if (!on && ok) kd_lrupriorst_pulse();        /* no stale high is left behind */
    ic_state = ok ? (on ? KD_IC_ON : KD_IC_OFF) : KD_IC_NONE;
    reset_req = 1;
}

/* ---------------- page 7 ---------------- */
void kd_view(kd_view_t *o)
{
    uint32_t m = irq_off();
    o->worst = worst;
    for (int c = 0; c < KD_C; c++) for (int k = 0; k < KD_M; k++) o->avg.v[c][k] = avg_sum[c][k] >> 8;
    o->avg.nominal = worst.nominal; o->avg.notes = 0; o->avg.fx_calls = 0;
    o->voice_max = voice_max; o->voice_engine = voice_eng; o->on_max = on_max; o->on_engine = on_eng;
    o->blocks = since_blocks; o->cplb_d = since_cd; o->cplb_i = since_ci;
    o->l1_on = kn_l1_enabled(); o->l1_ready = kn_l1_ready();
    for (int i = 0; i < KD_K1B_KINDS; i++) for (int b = 0; b < KD_BANKS; b++) {
        o->k1b_avg[i][b] = k1b_sum[i][b] >> 8; o->k1b_max[i][b] = k1b_max[i][b]; o->k1b_n[i][b] = k1b_n[i][b];
    }
    irq_on(m);
    o->stk_state = kn_astack_state(); o->stk_used = kn_astack_used();
    o->sw[KD_SW_L1] = o->l1_on; o->sw[KD_SW_STK] = o->stk_state == KN_AS_ON;
    o->sw[KD_SW_CDP] = (AMGCTL & KD_CDPRIO) != 0; o->sw[KD_SW_K1B] = k1b_on;
    o->ic_state = ic_state; o->sw[KD_SW_IC] = ic_state == KD_IC_ON;
    o->mode = pf_mode;
    o->mode_cnt[0] = kd_mode_cnt[pf_mode][0]; o->mode_cnt[1] = kd_mode_cnt[pf_mode][1];
}
void kd_reset(void) { reset_req = 1; kn_astack_mark(); }   /* page 7 push / open (thread level): the stack's fill */
void kd_set_l1(int on) { kn_l1_enable(on); reset_req = 1; }
void kd_set_switch(int sw, int on)
{
    switch (sw) {
    case KD_SW_L1: kd_set_l1(on); break;
    case KD_SW_STK: (void)kn_astack_enable(on); break;
    case KD_SW_CDP: {                            /* read-modify-write of bit 8 only */
        uint32_t m = irq_off();
        AMGCTL = (uint16_t)(on ? (AMGCTL | KD_CDPRIO) : (AMGCTL & ~KD_CDPRIO));
        SSYNC();
        irq_on(m);
        reset_req = 1;
        break;
    }
    case KD_SW_K1B: k1b_on = on != 0; reset_req = 1; break;
    case KD_SW_IC: ic_set(on); break;
    default: break;
    }
}
void kd_set_mode(int mode)
{
    if (mode < 0 || mode >= KD_MODES || (uint32_t)mode == pf_mode) return;
    pf_set((uint32_t)mode);
    for (int c = 0; c < KD_C; c++) for (int k = 0; k < KD_M; k++) avg_sum[c][k] = 0;
    reset_req = 1;
}
const char *kd_engine_name(int order)
{
#ifdef OP1_HOST
    static const char *const names[] = { "iter", "oxbow", "mbira" };
    return order >= 0 && order < 3 ? names[order] : "?";
#else
    extern const char *op1_engine_names[];
    extern const int op1_engine_count;
    if (order <= 0 || order >= op1_engine_count) return "iter";
    const char *n = op1_engine_names[order - 1];
    return n && *n ? n : "?";
#endif
}
