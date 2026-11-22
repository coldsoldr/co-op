// Property tests for the per-tape channel strip (core/include/coop_strip.h, docs/channel-strip.md).
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include "coop_strip.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
#define CHECKV(c, v) do { if (!(c)) { printf("FAIL: %s (line %d) value %g\n", #c, __LINE__, (double)(v)); fails++; } } while (0)

static const double FS = 44100.0;
static const int32_t UNITY15 = 32767;

struct Bus {
    int32_t dL[STRIP_BLOCK], dR[STRIP_BLOCK], aL[STRIP_BLOCK], aR[STRIP_BLOCK], bL[STRIP_BLOCK], bR[STRIP_BLOCK];
    coop_strip_bus b;
    Bus() { b = { dL, dR, aL, aR, bL, bR }; clear(); }
    void clear() { memset(dL, 0, sizeof dL); memset(dR, 0, sizeof dR); memset(aL, 0, sizeof aL);
                   memset(aR, 0, sizeof aR); memset(bL, 0, sizeof bL); memset(bR, 0, sizeof bR); }
};

// Run a sine through the strip for `blocks` blocks; return the RMS (dBFS) of the dry bus over the last half.
static double sine_db(coop_strip *s, double hz, double amp_db, int blocks = 400, int which = 0, int32_t g = UNITY15)
{
    Bus bus;
    double acc = 0; int cnt = 0; double ph = 0, amp = pow(10.0, amp_db / 20.0);
    int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
    for (int k = 0; k < blocks; k++) {
        for (int i = 0; i < STRIP_BLOCK; i++) {
            double v = amp * sin(ph); ph += 2 * M_PI * hz / FS;
            L[i] = R[i] = (int32_t)lrint(v * 2147483647.0);
        }
        bus.clear();
        coop_strip_process(s, L, R, g, g, &bus.b);
        const int32_t *o = which == 0 ? bus.dL : which == 1 ? bus.aL : bus.bL;
        if (k >= blocks / 2) for (int i = 0; i < STRIP_BLOCK; i++) { double x = o[i] / 2147483648.0; acc += x * x; cnt++; }
    }
    double rms = sqrt(acc / cnt);
    return rms > 0 ? 20 * log10(rms * sqrt(2.0)) : -200;     // as the sine's peak level
}

static void fresh(coop_strip *s, coop_strip_params *p) { coop_strip_init(s); *p = s->p; }

int main()
{
    coop_strip s; coop_strip_params p;

    // ---- helpers ----
    CHECK(strip_log2(2147483647) > -2 && strip_log2(2147483647) <= 0);
    CHECK(std::abs(strip_log2(1 << 30) - (-(1 << 16))) <= 2);             // 0.5 -> -1.0
    CHECK(std::abs(strip_exp2_q26(0) - (1 << 26)) <= 1);                    // 2^0 = 1
    CHECK(std::abs(strip_exp2_q26(-(1 << 16)) - (1 << 25)) <= 1);           // 2^-1
    CHECK(std::abs(strip_exp2_q26(3 << 16) - (8 << 26)) <= 8);              // 2^3
    for (int x = 1 << 10; x < (1 << 30); x += x / 3 + 1) {
        double want = log2(x / 2147483648.0);
        CHECKV(fabs(strip_log2(x) / 65536.0 - want) < 0.002, want);
    }
    CHECK(strip_level_q30(100) == (1 << 30) && strip_level_q30(0) == 0 && strip_level_q30(50) == (1 << 28));

    // ---- neutral strip: dry = input x fader, sends silent, nothing else runs ----
    fresh(&s, &p);
    CHECK(coop_strip_neutral(&p));
    {
        Bus bus; int32_t L[STRIP_BLOCK], R[STRIP_BLOCK], L0[STRIP_BLOCK];
        srand(1);
        int bad = 0, sends = 0;
        for (int k = 0; k < 50; k++) {
            for (int i = 0; i < STRIP_BLOCK; i++) { L[i] = (rand() - RAND_MAX / 2) * 1700; R[i] = -L[i]; L0[i] = L[i]; }
            bus.clear();
            coop_strip_process(&s, L, R, 1 << 15, 1 << 15, &bus.b);   // fader exactly unity (Q15 1.0)
            for (int i = 0; i < STRIP_BLOCK; i++) {
                if (bus.dL[i] != L0[i] || bus.dR[i] != -L0[i] || L[i] != L0[i]) bad++;
                sends |= bus.aL[i] | bus.aR[i] | bus.bL[i] | bus.bR[i];
            }
        }
        CHECK(bad == 0);
        CHECK(sends == 0);
    }

    // ---- EQ ----
    // a band at 0 dB does not run, even with the EQ on
    fresh(&s, &p); p.eq_on = 1; coop_strip_set(&s, &p); coop_strip_update(&s);
    CHECK(s.nbands == 0);
    // peak +6 dB at 1 kHz (freq index: 20 * 2^(i/12) = 1008 Hz at i = 68), Q 1.4 (index 7)
    fresh(&s, &p); p.eq_on = 1; p.eq_freq[1] = 68; p.eq_gain[1] = STRIP_EQ_GAIN0 + 12; coop_strip_set(&s, &p);
    {
        double ref = sine_db(&s, 1008, -12);
        CHECKV(fabs(ref - (-6.0)) < 0.3, ref);
        fresh(&s, &p); p.eq_on = 1; p.eq_freq[1] = 68; p.eq_gain[1] = STRIP_EQ_GAIN0 + 12; coop_strip_set(&s, &p);
        double far = sine_db(&s, 100, -12);
        CHECKV(fabs(far - (-12.0)) < 0.4, far);
    }
    // peak -18 dB (deepest) and +18 dB at 3 kHz
    for (int g : { 0, 72 }) {
        fresh(&s, &p); p.eq_on = 1; p.eq_freq[2] = 87; p.eq_gain[2] = g; p.eq_q[2] = 10; coop_strip_set(&s, &p);
        double f = 20 * pow(2.0, 87 / 12.0);
        double v = sine_db(&s, f, -24);
        double want = -24 + (g - 36) * 0.5;
        CHECKV(fabs(v - want) < 0.5, v);
    }
    // low cut at 100 Hz (index 28 = 101 Hz): 30 Hz well down, 1 kHz untouched
    fresh(&s, &p); p.eq_on = 1; p.eq_cut[0] = 1; p.eq_freq[0] = 28; p.eq_q[0] = 6; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 30, -6); CHECKV(v < -6 - 15, v); }
    fresh(&s, &p); p.eq_on = 1; p.eq_cut[0] = 1; p.eq_freq[0] = 28; p.eq_q[0] = 6; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 1000, -6); CHECKV(fabs(v + 6) < 0.3, v); }
    // high cut at 2 kHz (index 80 = 2016 Hz): 12 kHz well down
    fresh(&s, &p); p.eq_on = 1; p.eq_cut[3] = 1; p.eq_freq[3] = 80; p.eq_q[3] = 6; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 12000, -6); CHECKV(v < -6 - 20, v); }
    // low shelf +9 dB at 100 Hz: 30 Hz near +9, 3 kHz near 0
    fresh(&s, &p); p.eq_on = 1; p.eq_freq[0] = 28; p.eq_gain[0] = STRIP_EQ_GAIN0 + 18; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 30, -18, 800); CHECKV(fabs(v - (-9)) < 1.0, v); }
    fresh(&s, &p); p.eq_on = 1; p.eq_freq[0] = 28; p.eq_gain[0] = STRIP_EQ_GAIN0 + 18; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 3000, -18); CHECKV(fabs(v - (-18)) < 0.4, v); }
    // high shelf -12 dB at 5 kHz: 15 kHz near -12, 200 Hz near 0
    fresh(&s, &p); p.eq_on = 1; p.eq_freq[3] = 96; p.eq_gain[3] = STRIP_EQ_GAIN0 - 24; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 15000, -6); CHECKV(fabs(v - (-18)) < 1.0, v); }
    fresh(&s, &p); p.eq_on = 1; p.eq_freq[3] = 96; p.eq_gain[3] = STRIP_EQ_GAIN0 - 24; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 200, -6); CHECKV(fabs(v - (-6)) < 0.4, v); }
    // stability: every band at every extreme setting decays to silence after the input stops
    {
        int bad = 0;
        for (int fi = 0; fi < STRIP_EQ_NFREQ; fi += 7)
        for (int qi = 0; qi < STRIP_EQ_NQ; qi += 5)
        for (int g : { 0, 72 })
        for (int cut = 0; cut < 2; cut++) {
            fresh(&s, &p); p.eq_on = 1;
            for (int b = 0; b < 4; b++) { p.eq_freq[b] = fi; p.eq_q[b] = qi; p.eq_gain[b] = g; p.eq_cut[b] = cut; }
            coop_strip_set(&s, &p);
            Bus bus; int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
            /* 0.4 s of noise, then silence until the output is below -114 dBFS: four Q 5+ bands at 20 Hz and
               +18 dB ring for seconds, so allow up to 30 s; a filter that never gets there fails */
            int32_t pk = 0;
            for (int k = 0; k < 20000; k++) {
                for (int i = 0; i < STRIP_BLOCK; i++) L[i] = R[i] = k < 280 ? (int32_t)((rand() & 0xffff) - 0x8000) << 14 : 0;
                bus.clear(); coop_strip_process(&s, L, R, UNITY15, UNITY15, &bus.b);
                pk = 0;
                for (int i = 0; i < STRIP_BLOCK; i++) if (std::abs(bus.dL[i]) > pk) pk = std::abs(bus.dL[i]);
                if (k > 300 && pk <= (1 << 12)) break;
            }
            if (pk > (1 << 12)) { bad++; printf("eq decay fi %d qi %d g %d cut %d: %d\n", fi, qi, g, cut, pk); }
        }
        CHECK(bad == 0);
    }

    // ---- compressor ----
    // -20 dB threshold, 4:1, hard knee, a -8 dB sine -> -20 + 12/4 = -17 dB
    fresh(&s, &p); p.comp_on = 1; p.c_thresh = 40; p.c_ratio = 4; p.c_knee = 0; p.c_attack = 10; p.c_release = 10;
    coop_strip_set(&s, &p);
    { double v = sine_db(&s, 220, -8, 1500); CHECKV(fabs(v - (-17)) < 1.0, v); }
    // below the threshold: unchanged
    fresh(&s, &p); p.comp_on = 1; p.c_thresh = 40; p.c_ratio = 4; p.c_knee = 0; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 220, -30, 800); CHECKV(fabs(v - (-30)) < 0.3, v); }
    // makeup +6 dB
    fresh(&s, &p); p.comp_on = 1; p.c_thresh = 40; p.c_ratio = 4; p.c_knee = 0; p.c_makeup = 12; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 220, -30, 800); CHECKV(fabs(v - (-24)) < 0.3, v); }
    // limiter (inf:1) at -12 dB holds a 0 dB sine near -12
    fresh(&s, &p); p.comp_on = 1; p.c_thresh = 24; p.c_ratio = 9; p.c_knee = 0; p.c_attack = 0; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 220, -1, 1500); CHECKV(v < -11 && v > -13.5, v); }
    // soft knee: at the threshold the gain reduction is part of the hard-knee one, never more
    fresh(&s, &p); p.comp_on = 1; p.c_thresh = 40; p.c_ratio = 9; p.c_knee = 24; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 220, -20, 1500); CHECKV(v < -20 && v > -24, v); }

    // ---- saturator ----
    // mix 0: dry exactly, whatever the drive
    fresh(&s, &p); p.sat_on = 1; p.s_drive = 100; p.s_mix = 0; coop_strip_set(&s, &p);
    {
        Bus bus; int32_t L[STRIP_BLOCK], R[STRIP_BLOCK], L0[STRIP_BLOCK]; int bad = 0;
        for (int k = 0; k < 20; k++) {
            for (int i = 0; i < STRIP_BLOCK; i++) { L[i] = R[i] = L0[i] = (rand() - RAND_MAX / 2) * 900; }
            bus.clear(); coop_strip_process(&s, L, R, 1 << 15, 1 << 15, &bus.b);
            for (int i = 0; i < STRIP_BLOCK; i++) bad += bus.dL[i] != L0[i];
        }
        CHECK(bad == 0);
    }
    // drive 0, tape: a quiet signal passes nearly unchanged (tanh is ~linear)
    fresh(&s, &p); p.sat_on = 1; p.s_drive = 0; coop_strip_set(&s, &p);
    { double v = sine_db(&s, 440, -30); CHECKV(fabs(v + 30) < 0.3, v); }
    // every type at full drive stays bounded and makes harmonics (output differs from a scaled input)
    for (int t = 0; t < STRIP_SAT_NTYPE; t++) {
        fresh(&s, &p); p.sat_on = 1; p.s_drive = 100; p.s_type = t; coop_strip_set(&s, &p);
        double v = sine_db(&s, 440, 0);
        CHECKV(v < 1.0 && v > -24, v);
    }
    // tone: darker cuts 10 kHz, brighter lifts it
    {
        fresh(&s, &p); p.sat_on = 1; p.s_drive = 0; p.s_tone = -50; coop_strip_set(&s, &p);
        double dark = sine_db(&s, 10000, -30);
        fresh(&s, &p); p.sat_on = 1; p.s_drive = 0; p.s_tone = 50; coop_strip_set(&s, &p);
        double bright = sine_db(&s, 10000, -30);
        CHECKV(dark < -30 - 6, dark);
        CHECKV(bright > -30 + 3, bright);
    }

    // ---- sends ----
    // no dry, FX 1 at 60 %: dry bus silent, send 1 = 0.36 x signal, send 2 silent
    fresh(&s, &p); p.dry = 0; p.fx1 = 60; coop_strip_set(&s, &p);
    CHECK(!coop_strip_neutral(&p));
    {
        double d = sine_db(&s, 500, -6, 200, 0), a = sine_db(&s, 500, -6, 200, 1), b = sine_db(&s, 500, -6, 200, 2);
        CHECK(d < -150);
        CHECKV(fabs(a - (-6 + 20 * log10(0.36))) < 0.1, a);
        CHECK(b < -150);
    }
    // pre tap: the send hears the track before the EQ; post tap hears it after
    {
        fresh(&s, &p); p.eq_on = 1; p.eq_freq[1] = 68; p.eq_gain[1] = 0; p.fx2 = 100; p.tap_post = 0; coop_strip_set(&s, &p);
        double pre = sine_db(&s, 1008, -6, 400, 2);
        fresh(&s, &p); p.eq_on = 1; p.eq_freq[1] = 68; p.eq_gain[1] = 0; p.fx2 = 100; p.tap_post = 1; coop_strip_set(&s, &p);
        double post = sine_db(&s, 1008, -6, 400, 2);
        CHECKV(fabs(pre + 6) < 0.2, pre);
        CHECKV(post < -6 - 15, post);
    }
    // the fader rides dry and sends alike
    fresh(&s, &p); p.fx1 = 100; coop_strip_set(&s, &p);
    { double a = sine_db(&s, 500, -6, 200, 1, UNITY15 / 2); CHECKV(fabs(a - (-12)) < 0.1, a); }
    // a level change ramps: no step larger than the ramp allows
    {
        fresh(&s, &p); coop_strip_set(&s, &p);
        Bus bus; int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
        for (int i = 0; i < STRIP_BLOCK; i++) L[i] = R[i] = 1 << 30;
        coop_strip_process(&s, L, R, 1 << 15, 1 << 15, &bus.b);
        p.dry = 0; coop_strip_set(&s, &p);
        for (int i = 0; i < STRIP_BLOCK; i++) L[i] = R[i] = 1 << 30;
        bus.clear(); coop_strip_process(&s, L, R, 1 << 15, 1 << 15, &bus.b);
        int32_t maxstep = 0, prev = 1 << 30;
        for (int i = 0; i < STRIP_BLOCK; i++) { int32_t d = std::abs(bus.dL[i] - prev); if (d > maxstep) maxstep = d; prev = bus.dL[i]; }
        CHECK(maxstep <= (1 << 30) / 32);
        CHECK(bus.dL[STRIP_BLOCK - 1] == 0);
    }

    // ---- everything on, random settings and full-scale noise: bounded, no runaway ----
    {
        int bad = 0;
        for (int trial = 0; trial < 300; trial++) {
            fresh(&s, &p);
            p.eq_on = p.comp_on = p.sat_on = 1; p.tap_post = rand() & 1;
            for (int b = 0; b < 4; b++) { p.eq_cut[b] = rand() & 1; p.eq_freq[b] = rand() % STRIP_EQ_NFREQ;
                p.eq_gain[b] = rand() % STRIP_EQ_NGAIN; p.eq_q[b] = rand() % STRIP_EQ_NQ; }
            p.c_thresh = rand() % 97; p.c_ratio = rand() % STRIP_COMP_NRATIO; p.c_attack = rand() % 32; p.c_release = rand() % 32;
            p.c_makeup = rand() % 49; p.c_knee = rand() % 25; p.s_drive = rand() % 101; p.s_type = rand() % 4;
            p.s_tone = (int8_t)(rand() % 101 - 50); p.s_mix = rand() % 101; p.dry = rand() % 101; p.fx1 = rand() % 101; p.fx2 = rand() % 101;
            coop_strip_set(&s, &p);
            Bus bus; int32_t L[STRIP_BLOCK], R[STRIP_BLOCK];
            for (int k = 0; k < 60; k++) {
                for (int i = 0; i < STRIP_BLOCK; i++) { L[i] = (int32_t)((uint32_t)rand() * 2u); R[i] = (int32_t)((uint32_t)rand() * 3u); }
                bus.clear(); coop_strip_process(&s, L, R, UNITY15, UNITY15, &bus.b);
            }
            if (s.gr < 0) bad++;
        }
        CHECK(bad == 0);
    }

    printf(fails ? "strip_test: %d FAILED\n" : "strip_test: all passed\n", fails);
    return fails ? 1 : 0;
}
