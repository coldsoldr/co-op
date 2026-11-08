/* coop-fw: per-stage cycle accounting for the stats file's "cost" line (coop_stats.cc). Each stage adds its cycles
 * per call; the file prints the average, the peak and the count since boot. CYCLES counts core clocks, interrupts
 * included, so a stage's figure is what the block spends in it. */
#ifndef COOP_COST_H
#define COOP_COST_H
#include <stdint.h>

struct coop_cost { uint64_t sum; uint32_t n, max; uint64_t p0, p1; };   /* p0 / p1: the performance counters' deltas summed (below) */

#define COOP_COST_STAGE   0          /* S3 decimator + DC + mid / side (coop_instr_stage) */
#define COOP_COST_S2I     1          /* instrument slot 2 */
#define COOP_COST_S2M     2          /* master slot 2 */
#define COOP_COST_MASTER  3          /* the master stand-in, slot 2 included */
#define COOP_COST_PLUG    4          /* a plugin at slot 1 (instrument) */
#define COOP_COST_ST1     5          /* + type 0..7: the stock effect at slot 1 (its stereo class), by type */
#define COOP_COST_SYNTH   13         /* every synth voice render in a block, summed (one entry per block with a voice) */
#define COOP_COST_DRUM    14         /* the same for the drum voices */
#define COOP_COST_ENG     15         /* + 0..COOP_ENG_N-1: one voice render, by render function (first seen first) */
#define COOP_ENG_N        16
#define COOP_COST_N       (COOP_COST_ENG + COOP_ENG_N)

extern uint32_t g_coop_eng_fn[COOP_ENG_N];   /* the render function of each COOP_COST_ENG entry (0 = free) */

extern struct coop_cost g_coop_cost[COOP_COST_N];

static inline uint32_t coop_cost_now(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static inline void coop_cost_add(int i, uint32_t dt)
{
    struct coop_cost *c = &g_coop_cost[i];
    c->sum += dt; c->n++;
    if (dt > c->max) c->max = dt;
}

/* The Blackfin performance monitor (p6a, docs/fx-data-placement-plan.md lever 1): PFCNTR0 / PFCNTR1 count the two
 * events g_coop_pf_mode selects (coop_stats.cc coop_pf_boot; the layout and the event codes are kennel's kn_diag.c,
 * device-proven; the event kinds from Linux's perf_event.c): mode 0 (default) = "code fill stalls" (0x82) and "data
 * stalls" (0x98), both STALL CYCLES, so an entry's p0 / p1 say how many of its cycles waited for code / data; mode 1 =
 * the occurrence pair (code fetches stalled by DMA 0x80, data cache fills 0x9A).
 * Read beside CYCLES around each effect render; interrupts in between are counted, as they are in CYCLES. Nothing is
 * read while the monitor is off (g_coop_pf_live 0: op1emu images, or a boot that did not program it). */
#define COOP_PFCNTR0 (*(volatile uint32_t *)0xFFE08100u)
#define COOP_PFCNTR1 (*(volatile uint32_t *)0xFFE08104u)
extern volatile uint32_t g_coop_pf_live;
static inline void coop_pf_now(uint32_t p[2])
{
    if (g_coop_pf_live) { p[0] = COOP_PFCNTR0; p[1] = COOP_PFCNTR1; } else { p[0] = 0; p[1] = 0; }
}
static inline void coop_cost_add_pf(int i, uint32_t dt, const uint32_t p0[2])
{
    struct coop_cost *c = &g_coop_cost[i];
    c->sum += dt; c->n++;
    if (dt > c->max) c->max = dt;
    if (g_coop_pf_live) { uint32_t p[2]; coop_pf_now(p); c->p0 += p[0] - p0[0]; c->p1 += p[1] - p0[1]; }
}

/* The slow-block profiler (p5e): every coop hook inside the audio callback leaves (id, CYCLES); CoopAudioCb resets
 * the list each block and keeps it when the block ran over g_coop_slow_th cycles (coop_stats.cc prints the kept
 * ones as "slow" lines). The largest gap between two marks is where a long block spent its time. Ids:
 * 0x01 callback start, 0x02 block start done, 0x03 the stock callback returned; 0x10/0x11 a synth / drum voice render
 * start, 0x18/0x19 its end; 0x20 synth note-on, 0x21 synth voice hook, 0x22 drum voice hook; 0x30/0x31 the S3 stage;
 * 0x34/0x35 the mix entry; 0x38/0x39 the tape record tap; 0x40/0x41 the stock mono FX init (coop_fx_init_mono);
 * 0x42/0x43 st_make; 0x44/0x45 the mono FX render; 0x46/0x47 the stereo FX render; 0x48/0x49 FX 2 instrument,
 * 0x4a/0x4b FX 2 master; 0x50/0x51 the master stand-in. */
#define COOP_MK_N 128
extern volatile uint32_t g_mk_n, g_mk_lost;
extern uint32_t g_mk[COOP_MK_N][2];
static inline void coop_mark_at(uint32_t id, uint32_t t)
{
    uint32_t n = g_mk_n;
    if (n < COOP_MK_N) { g_mk[n][0] = id; g_mk[n][1] = t; g_mk_n = n + 1; } else g_mk_lost++;
}
static inline void coop_mark(uint32_t id) { coop_mark_at(id, coop_cost_now()); }

/* The gap profile (p5g diagnostic, g_coop_gap_prof = 1): every block, the time between each pair of consecutive marks
 * (in time order) is added up by pair, key = prev id << 8 | id. The time between two coop marks is stock code (or an
 * interrupt), so the table says where an ordinary block spends what coop does not measure. The runner snapshots it
 * per section. */
struct coop_gap { uint32_t key, n, max; uint64_t sum; };
#define COOP_GAP_N 48
extern struct coop_gap g_gap[COOP_GAP_N];
extern uint32_t g_gap_blocks, g_gap_full;
#endif
