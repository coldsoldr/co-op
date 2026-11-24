/* Blackfin-simulator check of the channel strip kernels (core/sdram/strip_fast.S) against their C model
 * (core/include/coop_strip_bf.h), bit for bit, with guard words around every array (no store outside it):
 * each kernel alone over random and edge inputs and states, then the whole coop_strip_bf_process with the
 * assembly against the C kernels over random parameter settings and blocks (outputs, buses and the whole state
 * compared). Run with tests/sim/run_strip_check.sh (bfin-elf-g++ -mcpu=bf524 + bfin-elf-run). Not a device artifact. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "coop_strip_bf.h"

#define N STRIP_BLOCK
#define G 8
#define GUARD 0x5A5A5A5Au

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int32_t rr(int32_t lo, int32_t hi) { return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)); }
static const int16_t EDGE16[] = { 0, 1, -1, 2, -2, 32767, -32768, 32766, -32767, 16384, -16384, 8191, -8192 };
static int16_t r16(int mode)
{
    uint32_t r = rnd();
    if (mode == 1 || (mode == 0 && (r & 7) == 0)) return EDGE16[(r >> 3) % (sizeof EDGE16 / sizeof EDGE16[0])];
    if (mode == 2) return (int16_t)((int32_t)(r >> 16) >> 6);            /* quiet */
    return (int16_t)(r >> 16);
}
static uint32_t rframe(int mode) { return strip_bf_pk(r16(mode), r16(mode)); }

/* an array of n words with guards on both sides */
struct W { uint32_t g0[G]; uint32_t v[2 * (N + 2)]; uint32_t g1[G]; };
static void wguard(W *a) { for (int i = 0; i < G; i++) a->g0[i] = a->g1[i] = GUARD; }
static int wok(const W *a) { for (int i = 0; i < G; i++) if (a->g0[i] != GUARD || a->g1[i] != GUARD) return 0; return 1; }

static long fails = 0;
static void report(const char *name, long n, long bad, long guards)
{
    printf("strip_check %-12s %6ld cases: bad %ld, guard hits %ld\n", name, n, bad, guards);
    if (bad || guards) fails++;
}

/* two copies of the arrays a kernel touches, C and asm */
static W ca, cb, cc, aa, ab, ac;
static void prep(int words_a, int words_b, int words_c, int mode)
{
    wguard(&ca); wguard(&cb); wguard(&cc);
    for (int i = 0; i < 2 * (N + 2); i++) { ca.v[i] = i < words_a ? rframe(mode) : rnd(); cb.v[i] = i < words_b ? rframe(mode) : rnd(); cc.v[i] = i < words_c ? rframe(mode) : rnd(); }
    aa = ca; ab = cb; ac = cc;
}
static int same(void) { return !memcmp(&ca, &aa, sizeof ca) && !memcmp(&cb, &ab, sizeof cb) && !memcmp(&cc, &ac, sizeof cc); }
static int guards(void) { return !wok(&aa) || !wok(&ab) || !wok(&ac); }

static void rand_satk(strip_bf_satk *k)
{
    static const int16_t *tabs[3] = { strip_sat_tape, strip_sat_tube, strip_sat_diode };
    int d = rr(0, 100);
    k->tab = tabs[rr(0, 2)] + 256;
    k->drive = rnd() & 1 ? strip_drive_gain[d] >> 13 : rr(0, 32767);
    k->dcomp = rnd() & 1 ? strip_drive_comp[d] : rr(0, 32767);
    int32_t t = rr(-32767, 32767);
    k->tone = (int32_t)strip_bf_pk(t, t);
    int32_t m = rr(1, 32767);
    k->mix = (int32_t)strip_bf_pk(m, 32768 - m);
    k->lp = (int32_t)rframe(0);
    k->dc[0] = (int32_t)rnd(); k->dc[1] = (int32_t)rnd();
    if (rnd() & 1) { k->dc[0] >>= 8; k->dc[1] >>= 8; }
}

static void check_kernels(void)
{
    const long IT = 3000;
    long bad, gh;
    /* pack / unpack / peak */
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        prep(N / 2, N / 2, N, (int)(it % 4));
        int32_t pc = strip_bfc_pack((const int16_t *)ca.v, (const int16_t *)cb.v, cc.v);
        int32_t pa = strip_bfa_pack((const int16_t *)aa.v, (const int16_t *)ab.v, ac.v);
        bad += pc != pa || !same(); gh += guards();
    }
    report("pack", IT, bad, gh);
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        prep(N, N / 2, N / 2, (int)(it % 4));
        int32_t pc = strip_bfc_unpack(ca.v, (int16_t *)cb.v, (int16_t *)cc.v);
        int32_t pa = strip_bfa_unpack(aa.v, (int16_t *)ab.v, (int16_t *)ac.v);
        bad += pc != pa || !same(); gh += guards();
    }
    report("unpack", IT, bad, gh);
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        prep(N / 2, N / 2, 0, (int)(it % 4));
        int32_t pc = strip_bfc_peak((const int16_t *)ca.v, (const int16_t *)cb.v);
        int32_t pa = strip_bfa_peak((const int16_t *)aa.v, (const int16_t *)ab.v);
        bad += pc != pa || !same(); gh += guards();
    }
    report("peak", IT, bad, gh);
    /* the compressor: constants from random settings, random states, random and edge blocks (quiet ones too) */
    bad = gh = 0;
    {
        static coop_strip_bf s;
        for (long it = 0; it < IT; it++) {
            coop_strip_bf_init(&s);
            coop_strip_params p = s.p;
            p.comp_on = 1; p.c_thresh = rr(0, 96); p.c_ratio = rr(0, STRIP_COMP_NRATIO - 1); p.c_attack = rr(0, 31);
            p.c_release = rr(0, 31); p.c_makeup = rr(0, 48); p.c_knee = rr(0, 3) ? rr(0, 24) : 0;
            coop_strip_bf_set(&s, &p); coop_strip_bf_update(&s);
            strip_bf_comp kc = s.comp, ka;
            if (it & 1) { kc.gr = rr(0, 1 << 20); kc.cg = rr(0, 0x7fffffff); }
            if (it % 5 == 0) kc.cg = rr(0, 1 << 26);
            ka = kc;
            for (int rep = 0; rep < 4; rep++) {
                prep(N, 0, 0, (int)((it + rep) % 4));
                if ((it + rep) % 7 == 0) for (int i = 0; i < N; i++) ca.v[i] = aa.v[i] = 0;
                strip_bfc_comp(ca.v, &kc);
                strip_bfa_comp(aa.v, &ka);
                bad += !same() || memcmp(&kc, &ka, sizeof kc); gh += guards();
            }
        }
    }
    report("comp", IT * 4, bad, gh);
    /* sat: shape, fold (y separate and in place), tube, tone, mix */
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        strip_bf_satk kc, ka;
        rand_satk(&kc); ka = kc;
        prep(N, N, 0, (int)(it % 4));
        int inplace = it & 1;
        int fold = (it >> 1) & 1;
        if (fold) {
            strip_bfc_fold(ca.v, inplace ? ca.v : cb.v, &kc);
            strip_bfa_fold(aa.v, inplace ? aa.v : ab.v, &ka);
        } else {
            strip_bfc_shape(ca.v, inplace ? ca.v : cb.v, &kc);
            strip_bfa_shape(aa.v, inplace ? aa.v : ab.v, &ka);
        }
        bad += !same(); gh += guards();
    }
    report("shape/fold", IT, bad, gh);
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        strip_bf_satk kc, ka;
        rand_satk(&kc); ka = kc;
        prep(N, N, 0, (int)(it % 4));
        strip_bfc_tube(ca.v, &kc); strip_bfa_tube(aa.v, &ka);
        bad += !same() || memcmp(&kc, &ka, sizeof kc); gh += guards();
        strip_bfc_tone(cb.v, &kc); strip_bfa_tone(ab.v, &ka);
        bad += !same() || memcmp(&kc, &ka, sizeof kc); gh += guards();
        strip_bfc_mix(ca.v, cb.v, &kc); strip_bfa_mix(aa.v, ab.v, &ka);
        bad += !same() || memcmp(&kc, &ka, sizeof kc); gh += guards();
    }
    report("tube/tone/mix", IT, bad, gh);
    /* bus jobs: 1 .. 6 jobs over four buses (ca) and four sources (cb), ramps, constants and unity */
    bad = gh = 0;
    for (long it = 0; it < IT; it++) {
        prep(2 * (N + 2), 2 * (N + 2), 0, (int)(it % 4));
        strip_bf_job jc[6], ja[6];
        int n = rr(1, 6);
        for (int i = 0; i < n; i++) {
            int b = rr(0, 3), x = rr(0, 3);
            int32_t g0 = rr(-32767, 32768), g1 = rr(-32767, 32768);
            int kind = rr(0, 6);
            if (kind == 0) g0 = g1 = 32768;                 /* unity */
            if (kind == 1) g0 = g1 = 0;                     /* skipped */
            if (kind == 2) g1 = g0;                         /* constant */
            if (kind == 3) g0 = 0;                          /* fade in */
            if (kind == 4) g1 = rr(0, 1) ? 32768 : 0;       /* to unity / out */
            jc[i].bus = (int16_t *)ca.v + 64 * b; jc[i].x = (const int16_t *)cb.v + 64 * x; jc[i].g0 = g0; jc[i].g1 = g1;
            ja[i].bus = (int16_t *)aa.v + 64 * b; ja[i].x = (const int16_t *)ab.v + 64 * x; ja[i].g0 = g0; ja[i].g1 = g1;
        }
        strip_bfc_bus_n(jc, n);
        strip_bfa_bus_n(ja, n);
        bad += !same(); gh += guards();
    }
    report("bus_n", IT, bad, gh);
    /* EQ band: coefficients from random settings, random states (Y, E, r any 16-bit values), random inputs */
    bad = gh = 0;
    static coop_strip_bf s;
    static W cy, ay;
    long nk = 0;
    for (long it = 0; it < IT; it++) {
        coop_strip_bf_init(&s);
        coop_strip_params p = s.p;
        int b = rr(0, 3);
        p.eq_on = 1; p.eq_cut[b] = rr(0, 1); p.eq_freq[b] = rr(0, STRIP_EQ_NFREQ - 1);
        p.eq_gain[b] = rr(0, STRIP_EQ_NGAIN - 1); p.eq_q[b] = rr(0, STRIP_EQ_NQ - 1);
        if (p.eq_gain[b] == STRIP_EQ_GAIN0) p.eq_gain[b]++;
        coop_strip_bf_set(&s, &p); coop_strip_bf_update(&s);
        nk += s.eqc[b][5] != 0;
        uint32_t stc[7], sta[7];
        int mode = (int)(it % 4);
        for (int i = 0; i < 7; i++) stc[i] = it % 3 ? rframe(mode) : 0;
        if (it % 3 == 1) { stc[6] &= 0x7fff7fffu; stc[4] &= 0x7fff7fffu; stc[5] &= 0x7fff7fffu; }
        memcpy(sta, stc, sizeof stc);
        prep(N + 2, N + 2, 0, mode);
        wguard(&cy);
        for (int i = 0; i < 2 * (N + 2); i++) cy.v[i] = rnd();
        ay = cy;
        for (int rep = 0; rep < 3; rep++) {
            strip_bf_eqjob jc = { ca.v, cb.v, s.eqc[b], stc }, ja = { aa.v, ab.v, s.eqc[b], sta };
            strip_bfc_eq(&jc, 1, cy.v);
            strip_bfa_eq(&ja, 1, ay.v);
            bad += !same() || memcmp(stc, sta, sizeof stc) || memcmp(&cy, &ay, sizeof cy);
            gh += guards() || !wok(&ay);
            for (int i = 2; i < N + 2; i++) { ca.v[i] = aa.v[i] = rframe(mode); }
        }
    }
    report("eq_band", IT * 3, bad, gh);
    printf("strip_check eq_band: %ld of %ld settings with the b's scaled (k > 0)\n", nk, IT);
}

/* the whole strip: asm kernels against C kernels */
static void check_process(void)
{
    static coop_strip_bf sc, sa;
    struct Bus { uint32_t g0[G]; int16_t v[6][N]; uint32_t g1[G]; };
    static Bus bc, ba;
    struct LR { uint32_t g0[G]; int16_t l[N]; uint32_t gm[G]; int16_t r[N]; uint32_t g1[G]; };
    static LR lc, la;
    long bad = 0, gh = 0, blocks = 0;
    const long TRIALS = 600;
    for (long t = 0; t < TRIALS; t++) {
        coop_strip_bf_init(&sc);
        coop_strip_params p = sc.p;
        p.eq_on = rr(0, 3) != 0; p.comp_on = rr(0, 2) != 0; p.sat_on = rr(0, 2) != 0; p.tap_post = rr(0, 1);
        for (int b = 0; b < 4; b++) {
            p.eq_cut[b] = rr(0, 1); p.eq_freq[b] = rr(0, STRIP_EQ_NFREQ - 1);
            p.eq_gain[b] = rr(0, 3) ? rr(0, STRIP_EQ_NGAIN - 1) : STRIP_EQ_GAIN0; p.eq_q[b] = rr(0, STRIP_EQ_NQ - 1);
        }
        p.c_thresh = rr(0, 96); p.c_ratio = rr(0, STRIP_COMP_NRATIO - 1); p.c_attack = rr(0, 31); p.c_release = rr(0, 31);
        p.c_makeup = rr(0, 48); p.c_knee = rr(0, 24); p.s_drive = rr(0, 100); p.s_type = rr(0, 3);
        p.s_tone = (int8_t)(rr(0, 2) ? rr(-50, 50) : 0); p.s_mix = rr(0, 3) ? rr(0, 100) : 100;
        p.dry = rr(0, 3) ? rr(0, 100) : 100; p.fx1 = rr(0, 1) ? rr(0, 100) : 0; p.fx2 = rr(0, 1) ? rr(0, 100) : 0;
        coop_strip_bf_set(&sc, &p);
        sa = sc;
        int32_t gL = rr(0, 1) ? 32768 : rr(-32767, 32768), gR = rr(0, 1) ? gL : rr(0, 32768);
        for (int k = 0; k < 24; k++) {
            if (k == 12 && rr(0, 1)) {          /* a parameter change mid-run */
                p.dry = rr(0, 100); p.fx1 = rr(0, 100); p.s_mix = rr(0, 100); p.eq_gain[rr(0, 3)] = rr(0, 72);
                coop_strip_bf_set(&sc, &p); coop_strip_bf_set(&sa, &p);
            }
            if (k == 18) { gL = rr(0, 32768); gR = rr(0, 32768); }
            int mode = (int)((t + k) % 5);
            for (int i = 0; i < G; i++) lc.g0[i] = lc.gm[i] = lc.g1[i] = bc.g0[i] = bc.g1[i] = GUARD;
            for (int i = 0; i < N; i++) {
                lc.l[i] = mode == 4 ? 0 : r16(mode & 3); lc.r[i] = mode == 4 ? 0 : r16(mode & 3);
                for (int j = 0; j < 6; j++) bc.v[j][i] = r16(2);
            }
            la = lc; ba = bc;
            coop_strip_bf_bus bus_c = { bc.v[0], bc.v[1], bc.v[2], bc.v[3], bc.v[4], bc.v[5] };
            coop_strip_bf_bus bus_a = { ba.v[0], ba.v[1], ba.v[2], ba.v[3], ba.v[4], ba.v[5] };
            coop_strip_bf_process_k(&sc, lc.l, lc.r, gL, gR, &bus_c, &strip_bf_c_kernels);
            coop_strip_bf_process_k(&sa, la.l, la.r, gL, gR, &bus_a, &strip_bf_asm_kernels);
            blocks++;
            /* pointers to each strip's own arrays: equal by construction (outputs are compared); the rest bit for bit */
            static coop_strip_bf ta; ta = sa;
            for (int q = 0; q < 6; q++) { ta.jobs[q].bus = sc.jobs[q].bus; ta.jobs[q].x = sc.jobs[q].x; }
            for (int q = 0; q < 4; q++) { ta.ej[q].in = sc.ej[q].in; ta.ej[q].out = sc.ej[q].out; ta.ej[q].coef = sc.ej[q].coef; ta.ej[q].st = sc.ej[q].st; }
            ta.cur = sc.cur; ta.y = sc.y; ta.comp.log2t = sc.comp.log2t; ta.comp.exp2t = sc.comp.exp2t; ta.sat.tab = sc.sat.tab;
            bad += memcmp(&lc, &la, sizeof lc) || memcmp(&bc, &ba, sizeof bc) || memcmp(&sc, &ta, sizeof sc);
            for (int i = 0; i < G; i++)
                gh += la.g0[i] != GUARD || la.gm[i] != GUARD || la.g1[i] != GUARD || ba.g0[i] != GUARD || ba.g1[i] != GUARD;
        }
    }
    report("process", blocks, bad, gh);
}

int main(void)
{
    check_kernels();
    check_process();
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails ? 1 : 0;
}
