// Cycle cost of the channel strip per track and block, on the bfin simulator: tests/sim/run_strip_bench.sh.
// Two columns: the golden reference (core/include/coop_strip.h, int64 C) and the Blackfin implementation
// (core/include/coop_strip_bf.h driving the core/sdram/strip_fast.S kernels). The simulator has no cache or SDRAM
// model, so these are the core's instruction cycles only (a floor for the device; docs/performance.md has the
// sim-to-device factors).
#include <stdint.h>
#include <stdio.h>
#include "coop_strip.h"
#include "coop_strip_bf.h"

static inline uint32_t cyc(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

static int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
static int32_t b[6][STRIP_BLOCK];
static coop_strip s;
static int16_t l16[STRIP_BLOCK] __attribute__((aligned(4))), r16[STRIP_BLOCK] __attribute__((aligned(4)));
static int16_t b16[6][STRIP_BLOCK] __attribute__((aligned(4)));
static coop_strip_bf sb;

struct res { uint32_t avg, worst, upd; };

static res run_ref(const coop_strip_params *p)
{
    coop_strip_bus bus = { b[0], b[1], b[2], b[3], b[4], b[5] };
    coop_strip_init(&s);
    coop_strip_set(&s, p);
    uint32_t seed = 1, worst = 0, sum = 0, upd = 0;
    for (int k = 0; k < 40; k++) {
        for (int i = 0; i < STRIP_BLOCK; i++) {
            seed = seed * 1664525u + 1013904223u;
            L[i] = (int32_t)seed >> 2; R[i] = (int32_t)(seed * 7u) >> 2;
        }
        uint32_t t0 = cyc();
        coop_strip_process(&s, L, R, 30000, 28000, &bus);
        uint32_t dt = cyc() - t0;
        if (k >= 2) { sum += dt; if (dt > worst) worst = dt; }   /* block 0 holds the coefficient update */
        if (k == 0) upd = dt;
    }
    return { sum / 38, worst, upd };
}

/* the same signal as int16; the fader moves every 8th block so the level ramps are timed too */
static res run_bf(const coop_strip_params *p)
{
    coop_strip_bf_bus bus = { b16[0], b16[1], b16[2], b16[3], b16[4], b16[5] };
    coop_strip_bf_init(&sb);
    coop_strip_bf_set(&sb, p);
    uint32_t seed = 1, worst = 0, sum = 0, upd = 0;
    for (int k = 0; k < 40; k++) {
        for (int i = 0; i < STRIP_BLOCK; i++) {
            seed = seed * 1664525u + 1013904223u;
            l16[i] = (int16_t)((int32_t)seed >> 18); r16[i] = (int16_t)((int32_t)(seed * 7u) >> 18);
        }
        int32_t g = (k & 8) ? 26000 : 30000;
        uint32_t t0 = cyc();
        coop_strip_bf_process(&sb, l16, r16, g, g - 2000, &bus);
        uint32_t dt = cyc() - t0;
        if (k >= 2) { sum += dt; if (dt > worst) worst = dt; }
        if (k == 0) upd = dt;
    }
    return { sum / 38, worst, upd };
}

static void row(const char *name, const coop_strip_params *p)
{
    res a = run_ref(p), c = run_bf(p);
    printf("%-36s ref avg %6u worst %6u | bf avg %6u worst %6u (update block %6u)\n", name,
           (unsigned)a.avg, (unsigned)a.worst, (unsigned)c.avg, (unsigned)c.worst, (unsigned)c.upd);
}

/* the kernels alone (asm), one call each, for the breakdown in docs/channel-strip.md */
static void kernels(void)
{
    static uint32_t w[2][STRIP_BLOCK + 2], ye[STRIP_BLOCK + 2], st[7];
    coop_strip_bf_init(&sb);
    coop_strip_params p = sb.p;
    p.eq_on = 1; p.eq_gain[1] = 48; p.eq_gain[3] = 72;
    coop_strip_bf_set(&sb, &p); coop_strip_bf_update(&sb);
    for (int i = 0; i < STRIP_BLOCK + 2; i++) w[0][i] = strip_bf_pk(i * 300 - 9000, 7000 - i * 200);
    uint32_t t;
    t = cyc(); strip_bfa_pack(l16, r16, w[0] + 2); t = cyc() - t; printf("kernel pack            %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_unpack(w[0] + 2, l16, r16); t = cyc() - t; printf("kernel unpack          %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_peak(l16, r16); t = cyc() - t; printf("kernel peak            %5u\n", (unsigned)t);
    t = cyc(); strip_bf_eqjob e1 = { w[0], w[1], sb.eqc[1], st }; strip_bfa_eq(&e1, 1, ye); t = cyc() - t; printf("kernel eq_band (k = %u) %5u\n", (unsigned)sb.eqc[1][5], (unsigned)t);
    t = cyc(); strip_bf_eqjob e3 = { w[0], w[1], sb.eqc[3], st }; strip_bfa_eq(&e3, 1, ye); t = cyc() - t; printf("kernel eq_band (k = %u) %5u\n", (unsigned)sb.eqc[3][5], (unsigned)t);
    { coop_strip_params q = sb.p; q.comp_on = 1; coop_strip_bf_set(&sb, &q); coop_strip_bf_update(&sb); }
    t = cyc(); strip_bfa_comp(w[0] + 2, &sb.comp); t = cyc() - t; printf("kernel comp (4 x 16)   %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_shape(w[0] + 2, w[1] + 2, &sb.sat); t = cyc() - t; printf("kernel shape           %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_fold(w[0] + 2, w[1] + 2, &sb.sat); t = cyc() - t; printf("kernel fold            %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_tube(w[1] + 2, &sb.sat); t = cyc() - t; printf("kernel tube            %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_tone(w[1] + 2, &sb.sat); t = cyc() - t; printf("kernel tone            %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_mix(w[0] + 2, w[1] + 2, &sb.sat); t = cyc() - t; printf("kernel mix             %5u\n", (unsigned)t);
    strip_bf_job jb[6] = { { b16[0], l16, 1000, 3000 }, { b16[1], r16, 1000, 3000 }, { b16[2], l16, 9000, 9000 },
                           { b16[3], r16, 9000, 9000 }, { b16[4], l16, 32768, 32768 }, { b16[5], r16, 0, 0 } };
    t = cyc(); strip_bfa_bus_n(jb, 2); t = cyc() - t; printf("kernel bus_n (2 ramps) %5u\n", (unsigned)t);
    t = cyc(); strip_bfa_bus_n(jb, 6); t = cyc() - t; printf("kernel bus_n (6 mixed) %5u\n", (unsigned)t);
}

int main(void)
{
    uint32_t sc;    /* the simulator starts with the cycle counter off (SYSCFG CCEN, bit 1) */
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(sc));
    sc |= 2u;
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(sc));
    coop_strip_params p, d;
    coop_strip_params_default(&d);
    p = d; row("neutral (dry only)", &p);
    p = d; p.dry = 0; row("all off, dry 0 (silent track)", &p);
    p = d; p.eq_on = 1; p.eq_gain[1] = 48; row("EQ, 1 band", &p);
    p = d; p.eq_on = 1; for (int i = 0; i < 4; i++) p.eq_gain[i] = 48; row("EQ, 4 bands", &p);
    p = d; p.comp_on = 1; row("comp", &p);
    p = d; p.sat_on = 1; row("sat tape", &p);
    p = d; p.sat_on = 1; p.s_tone = 20; p.s_mix = 60; row("sat tape + tone + mix", &p);
    p = d; p.fx1 = 50; p.fx2 = 30; row("dry + 2 sends", &p);
    p = d; p.eq_on = p.comp_on = p.sat_on = 1; for (int i = 0; i < 4; i++) p.eq_gain[i] = 48;
    p.s_tone = 20; p.s_mix = 60; p.fx1 = 50; p.fx2 = 30; row("everything on", &p);
    /* the dearest setting: four +18 dB bands (b's scaled, k > 0), tube with tone and mix, pre tap */
    p = d; p.eq_on = p.comp_on = p.sat_on = 1; p.tap_post = 0;
    for (int i = 0; i < 4; i++) p.eq_gain[i] = 72;
    p.s_type = STRIP_SAT_TUBE; p.s_tone = -30; p.s_mix = 50; p.dry = 70; p.fx1 = 50; p.fx2 = 30;
    row("everything on, worst (4 x +18 dB, tube)", &p);
    kernels();
    return 0;
}
