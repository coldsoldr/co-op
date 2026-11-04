/* coop-fw: per-stage cycle accounting for the stats file's "cost" line (coop_stats.cc). Each stage adds its cycles
 * per call; the file prints the average, the peak and the count since boot. CYCLES counts core clocks, interrupts
 * included, so a stage's figure is what the block spends in it. */
#ifndef COOP_COST_H
#define COOP_COST_H
#include <stdint.h>

struct coop_cost { uint64_t sum; uint32_t n, max; };

#define COOP_COST_STAGE   0          /* S3 decimator + DC + mid / side (coop_instr_stage) */
#define COOP_COST_S2I     1          /* instrument slot 2 */
#define COOP_COST_S2M     2          /* master slot 2 */
#define COOP_COST_MASTER  3          /* the master stand-in, slot 2 included */
#define COOP_COST_PLUG    4          /* a plugin at slot 1 (instrument) */
#define COOP_COST_ST1     5          /* + type 0..7: the stock effect at slot 1 (its stereo class), by type */
#define COOP_COST_N       13

extern struct coop_cost g_coop_cost[COOP_COST_N];

static inline uint32_t coop_cost_now(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static inline void coop_cost_add(int i, uint32_t dt)
{
    struct coop_cost *c = &g_coop_cost[i];
    c->sum += dt; c->n++;
    if (dt > c->max) c->max = dt;
}
#endif
