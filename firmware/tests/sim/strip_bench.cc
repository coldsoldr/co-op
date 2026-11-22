// Cycle cost of the channel strip (core/include/coop_strip.h) per track and block, on the bfin simulator:
// tests/sim/run_strip_bench.sh. The simulator has no cache or SDRAM model, so these are the core's
// instruction cycles only (a floor for the device; docs/performance.md has the sim-to-device factors).
#include <stdint.h>
#include <stdio.h>
#include "coop_strip.h"

static inline uint32_t cyc(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

static int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
static int32_t b[6][STRIP_BLOCK];
static coop_strip s;

static uint32_t run(const char *name, const coop_strip_params *p)
{
    coop_strip_bus bus = { b[0], b[1], b[2], b[3], b[4], b[5] };
    coop_strip_init(&s);
    coop_strip_set(&s, p);
    uint32_t seed = 1, worst = 0, sum = 0;
    for (int k = 0; k < 40; k++) {
        for (int i = 0; i < STRIP_BLOCK; i++) {
            seed = seed * 1664525u + 1013904223u;
            L[i] = (int32_t)seed >> 2; R[i] = (int32_t)(seed * 7u) >> 2;
        }
        uint32_t t0 = cyc();
        coop_strip_process(&s, L, R, 30000, 28000, &bus);
        uint32_t dt = cyc() - t0;
        if (k >= 2) { sum += dt; if (dt > worst) worst = dt; }   /* block 0 holds the coefficient update */
        if (k == 0) printf("%-34s update block %6u\n", name, (unsigned)dt);
    }
    printf("%-34s avg %6u  worst %6u cycles/track/block\n", name, (unsigned)(sum / 38), (unsigned)worst);
    return worst;
}

int main(void)
{
    uint32_t sc;    /* the simulator starts with the cycle counter off (SYSCFG CCEN, bit 1) */
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(sc));
    sc |= 2u;
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(sc));
    coop_strip_params p, d;
    coop_strip_params_default(&d);
    p = d; run("neutral (dry only)", &p);
    p = d; p.dry = 0; run("all off, dry 0 (silent track)", &p);
    p = d; p.eq_on = 1; p.eq_gain[1] = 48; run("EQ, 1 band", &p);
    p = d; p.eq_on = 1; for (int i = 0; i < 4; i++) p.eq_gain[i] = 48; run("EQ, 4 bands", &p);
    p = d; p.comp_on = 1; run("comp", &p);
    p = d; p.sat_on = 1; run("sat tape", &p);
    p = d; p.sat_on = 1; p.s_tone = 20; p.s_mix = 60; run("sat tape + tone + mix", &p);
    p = d; p.fx1 = 50; p.fx2 = 30; run("dry + 2 sends", &p);
    p = d; p.eq_on = p.comp_on = p.sat_on = 1; for (int i = 0; i < 4; i++) p.eq_gain[i] = 48;
    p.s_tone = 20; p.s_mix = 60; p.fx1 = 50; p.fx2 = 30; run("everything on", &p);
    return 0;
}
