/* Core L1 pool (kn_l1.h, ARCHITECTURE.md "L1 pool").
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "kn_l1.h"

#define L1_TAB_N    40
#define L1_FIRST    0xFF90081Cu                  /* entry 0's buffer */
#define L1_END      0xFF906908u                  /* entry 39's buffer end = the table itself */
#define L1_POOL     0xFF900820u                  /* first 32-byte line in the range */
#define L1_TOP      0xFF907E00u                  /* L1 data B's l1_last 512 B, not loaded by the LDR */
#define SINE_N      2049                         /* sin(2 pi k / 8192) x 32767, k = 0..2048 */

#ifdef OP1_HOST
uint32_t kn_l1_host_tab[80], kn_l1_host_lander;
uint8_t kn_l1_host_mem[24800] __attribute__((aligned(32))), kn_l1_host_top[512] __attribute__((aligned(32)));
int16_t kn_l1_host_sine[8192];
#define TAB_PTR     (kn_l1_host_tab)
#define TAB_SIZE    (kn_l1_host_tab + 40)
#define LANDER_ON() (kn_l1_host_lander == 1)
#define POOL_AT(o)  ((void *)(kn_l1_host_mem + (o)))
#define TOP_AT()    ((void *)kn_l1_host_top)
#define FW_SINE     ((const int16_t *)kn_l1_host_sine)
#else
#define TAB_PTR     ((volatile uint32_t *)0xFF906908u)
#define TAB_SIZE    ((volatile uint32_t *)0xFF9069A8u)
extern const volatile uint32_t kn_lander_enabled;   /* build option (hook.S), 1 = the Lander game is on */
#define LANDER_ON() (kn_lander_enabled == 1)
#define POOL_AT(o)  ((void *)(uintptr_t)(L1_POOL + (o)))
#define TOP_AT()    ((void *)(uintptr_t)L1_TOP)
#define FW_SINE     ((const int16_t *)0x0101E5E8)  /* OP1_SINE_TABLE */
#endif

/* partition offsets from L1_POOL (the sine quarter first); DRUM is the separate top block */
static const uint32_t L1_OFF[OP1_L1_PARTS] = {
    KN_L1_SINE_BYTES,
    KN_L1_SINE_BYTES + KN_L1_SYNTH_BYTES,
    KN_L1_SINE_BYTES + KN_L1_SYNTH_BYTES + KN_L1_FX_BYTES,
    0 };
static const uint32_t L1_CAP[OP1_L1_PARTS] = { KN_L1_SYNTH_BYTES, KN_L1_FX_BYTES, KN_L1_MASTER_BYTES, KN_L1_DRUM_BYTES };
_Static_assert(KN_L1_SINE_BYTES + KN_L1_SYNTH_BYTES + KN_L1_FX_BYTES + KN_L1_MASTER_BYTES <= (L1_END & ~31u) - L1_POOL,
               "partitions fit 0xFF900820..0xFF906900");
_Static_assert(KN_L1_SINE_BYTES >= SINE_N * 2 && KN_L1_SINE_BYTES % 32 == 0 && KN_L1_FX_BYTES % 32 == 0 && KN_L1_MASTER_BYTES % 32 == 0, "lines");
static uint32_t l1_ok, l1_on = 1, l1_tick, l1_cb, l1_sine_state;   /* l1_sine_state: 0 not yet, 1 filled, 2 no (unexpected table) */
static volatile uint32_t l1_want = 1;               /* page 7's switch (UI thread); kn_l1_tick applies it at a block's end */
/* plugin calls running on another thread right now (R246's core_note_on: a note_on on the key / sequencer thread, which
 * the callback can preempt). While one runs, the callback moves no partition: no takeover, no switch */
static volatile uint32_t l1_busy;
static const void *volatile l1_holder[OP1_L1_PARTS];
static uint32_t l1_last[OP1_L1_PARTS];
static uint32_t *l1_home[OP1_L1_PARTS], l1_words[OP1_L1_PARTS];   /* the holder's state lease: its home, words at 0 */
static uint32_t l1_dummy[8];                        /* what the Lander's table points at now (size 0: never read) */

static uint32_t *l1_at(int p) { return p == OP1_L1_DRUM ? TOP_AT() : POOL_AT(L1_OFF[p]); }
/* the holder's state goes back to its home (the only copy-back there is: at a takeover, the switch, a new size) */
static void l1_put_back(int p)
{
    uint32_t *h = l1_home[p], n = l1_words[p];
    const uint32_t *s = l1_at(p);
    for (uint32_t i = 0; i < n; i++) h[i] = s[i];
    l1_home[p] = 0; l1_words[p] = 0;
}
static void l1_bring_in(int p, uint32_t *home, uint32_t n)
{
    uint32_t *d = l1_at(p);
    for (uint32_t i = 0; i < n; i++) d[i] = home[i];
    l1_home[p] = n ? home : 0; l1_words[p] = n;
}

void kn_l1_init(void)
{
    l1_ok = 0;
    if (LANDER_ON()) return;
    /* the stock layout only: 40 buffers back to back (each word aligned) from L1_FIRST to L1_END */
    uint32_t at = L1_FIRST;
    for (int i = 0; i < L1_TAB_N; i++) {
        at = (at + 3u) & ~3u;
        if (TAB_PTR[i] != at || TAB_SIZE[i] == 0 || TAB_SIZE[i] > 0x2000u) return;
        at += TAB_SIZE[i];
    }
    if (at != L1_END) return;
    for (int i = 0; i < L1_TAB_N; i++) { TAB_SIZE[i] = 0; TAB_PTR[i] = (uint32_t)(uintptr_t)l1_dummy; }
    for (int p = 0; p < OP1_L1_PARTS; p++) { l1_holder[p] = 0; l1_last[p] = 0; l1_home[p] = 0; l1_words[p] = 0; }
    l1_sine_state = 0; l1_cb = 0;
    l1_ok = 1;
}

void kn_l1_begin(void) { l1_cb = 1; }
/* threads only modify l1_busy, and more than one may (key, sequencers): masked so no update is lost */
#ifdef __bfin__
static uint32_t l1_irq_off(void) { uint32_t m; __asm__ volatile("cli %0;" : "=d"(m)); return m; }
static void l1_irq_on(uint32_t m) { __asm__ volatile("sti %0;" : : "d"(m) : "memory"); }
#else
static uint32_t l1_irq_off(void) { return 0; }
static void l1_irq_on(uint32_t m) { (void)m; }
#endif
uint32_t kn_l1_thread_enter(void)
{
    if (l1_cb) return 0;                       /* inside the callback: nothing can preempt it */
    uint32_t m = l1_irq_off();
    l1_busy++;
    l1_irq_on(m);
    return 1;
}
void kn_l1_thread_leave(uint32_t t)
{
    if (!t) return;
    uint32_t m = l1_irq_off();
    l1_busy--;
    l1_irq_on(m);
}
void kn_l1_tick(void)
{
    l1_cb = 0;
    l1_tick++;
    uint32_t want = l1_want;
    if (want == l1_on || l1_busy) return;      /* a thread's plugin call is mid-way: the switch waits a block */
    /* the switch moved: at this block boundary no plugin is mid-render, so every state lease goes home now (off), and
     * whoever claims next starts fresh (on) */
    for (int p = 0; p < OP1_L1_PARTS; p++) {
        if (l1_ok && l1_words[p]) l1_put_back(p);
        l1_holder[p] = 0;
    }
    l1_on = want;
}
uint32_t kn_l1_ready(void) { return l1_ok; }
uint32_t kn_l1_enabled(void) { return l1_want; }    /* the switch's position (in effect from the next block's end) */
void kn_l1_enable(int on) { l1_want = on != 0; }    /* UI thread: only the request; kn_l1_tick does the rest */

/* home 0: a table / scratch claim (op1_l1_claim). A takeover happens only inside the audio callback (l1_cb): outside
 * it the holder gets its partition and anyone else 0, and nothing is copied. */
static void *l1_take(int part, const void *owner, uint32_t *home, uint32_t state, uint32_t bytes, int *fresh)
{
    if (fresh) *fresh = 0;
    if (!l1_ok || !l1_on || part < 0 || part >= OP1_L1_PARTS || bytes > L1_CAP[part] || !owner) return 0;
    if (state > bytes || (state & 3u) || ((uintptr_t)home & 3u)) return 0;
    uint32_t *at = l1_at(part);
    if (l1_holder[part] != owner) {
        if (!l1_cb || l1_busy) return 0;           /* no takeover outside the callback or under a thread's call */
        if (l1_holder[part] && l1_tick - l1_last[part] < KN_L1_IDLE) return 0;   /* someone else's, still in use */
        if (l1_words[part]) l1_put_back(part);       /* the old holder's state home first, then ours in */
        l1_holder[part] = owner;
        l1_bring_in(part, home, state >> 2);
        if (fresh) *fresh = 1;
    } else if (home && l1_words[part] != state >> 2) {   /* the holder's state changed size: home and in again */
        if (!l1_cb || l1_busy) return 0;
        if (l1_words[part]) l1_put_back(part);
        l1_bring_in(part, home, state >> 2);
        if (fresh) *fresh = 1;
    }
    l1_last[part] = l1_tick;
    return at;
}
void *op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)
{
    return l1_take(part, owner, 0, 0, bytes, fresh);
}
void *op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{
    if (fresh) *fresh = 0;
    if (!home || !state_bytes) return 0;
    return l1_take(part, home, home, state_bytes, bytes, fresh);
}

const int16_t *op1_l1_sine_quarter(void)
{
    if (!l1_ok || !l1_on) return 0;
    if (l1_sine_state == 0) {                       /* filled once, from the firmware's table, if it is the expected one */
        const int16_t *s = FW_SINE;
        l1_sine_state = s[0] == 0 && s[1] == 25 && s[1024] == 23170 && s[2048] == 32767 ? 1 : 2;
        if (l1_sine_state == 1) {
            int16_t *d = POOL_AT(0);
            for (int i = 0; i < SINE_N; i++) d[i] = s[i];
        }
    }
    return l1_sine_state == 1 ? (const int16_t *)POOL_AT(0) : 0;
}
