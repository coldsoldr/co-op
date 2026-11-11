// Property tests for the stereo instrument bus DSP (coop_voice.h): pan law, spread, stereo mix.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include "coop_fixed.h"
#include "coop_voice.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while(0)

int main() {
    // ---- pan law endpoints ----
    coop_pan c = coop_pan_gains(0);
    CHECK(std::abs(c.gL - 23170) < 40 && std::abs(c.gR - 23170) < 40);   // centre ~ -3 dB
    CHECK(std::abs(c.gL - c.gR) < 4);                                     // centre is symmetric
    coop_pan l = coop_pan_gains(-32768);
    CHECK(l.gL > 32700 && l.gR < 40);                                     // hard left
    coop_pan r = coop_pan_gains(32767);
    CHECK(r.gR > 32700 && r.gL < 40);                                     // hard right

    // ---- equal power across the field: gL^2 + gR^2 ~ 1 ----
    const double unit = 32767.0 * 32767.0;
    for (int p = -32768; p <= 32767; p += 257) {
        coop_pan g = coop_pan_gains((int16_t)p);
        double power = (double)g.gL * g.gL + (double)g.gR * g.gR;
        CHECK(std::abs(power - unit) / unit < 0.03);                      // within 3%
    }
    // monotonic: as pan goes left->right, gR rises and gL falls
    coop_pan prev = coop_pan_gains(-32768);
    for (int p = -32768 + 512; p <= 32767; p += 512) {
        coop_pan g = coop_pan_gains((int16_t)p);
        CHECK(g.gR >= prev.gR - 1 && g.gL <= prev.gL + 1);
        prev = g;
    }

    // ---- spread: 0 = all centred ----
    coop_spread s; coop_spread_init(&s, 0);
    for (int i = 0; i < COOP_MAX_VOICES; i++) coop_spread_note_on(&s, i);
    for (int i = 0; i < COOP_MAX_VOICES; i++) CHECK(s.pan[i] == 0);

    // ---- spread: full = positions vary and stay in range ----
    coop_spread_set(&s, 32767);
    int nonzero = 0, spread_seen = 0;
    for (int i = 0; i < COOP_MAX_VOICES; i++) {
        coop_spread_note_on(&s, i);
        CHECK(s.pan[i] >= -32768 && s.pan[i] <= 32767);
        if (s.pan[i] != 0) nonzero++;
        if (std::abs((int)s.pan[i]) > 16000) spread_seen++;   // some land well off centre
    }
    CHECK(nonzero >= COOP_MAX_VOICES - 1);    // essentially all get a random position
    CHECK(spread_seen >= 1);

    // ---- spread half: positions bounded by ~half the field ----
    coop_spread s2; coop_spread_init(&s2, 16384);   // 0.5
    for (int i = 0; i < COOP_MAX_VOICES; i++) coop_spread_note_on(&s2, i);
    for (int i = 0; i < COOP_MAX_VOICES; i++) CHECK(std::abs((int)s2.pan[i]) <= 16500);

    // ---- stereo mix: centred voice -> L == R (mono equivalence) ----
    int32_t L[16] = {0}, R[16] = {0};
    int16_t v[16]; for (int i = 0; i < 16; i++) v[i] = (int16_t)((i - 8) * 1000);
    coop_voice_mix(L, R, v, 16, coop_pan_gains(0), 32767);
    for (int i = 0; i < 16; i++) CHECK(L[i] == R[i]);

    // ---- hard-left voice -> R stays ~silent, L carries it ----
    int32_t L2[16] = {0}, R2[16] = {0};
    coop_voice_mix(L2, R2, v, 16, coop_pan_gains(-32768), 32767);
    for (int i = 0; i < 16; i++) { CHECK(std::abs(R2[i]) < 64); }
    CHECK(std::abs(L2[8 + 4]) > 1000);    // a non-zero sample is clearly present on the left

    // ---- accumulate: two centred voices sum ----
    int32_t L3[16] = {0}, R3[16] = {0};
    coop_voice_mix(L3, R3, v, 16, coop_pan_gains(0), 32767);
    coop_voice_mix(L3, R3, v, 16, coop_pan_gains(0), 32767);
    for (int i = 0; i < 16; i++) CHECK(std::abs(L3[i] - 2 * L[i]) <= 2);

    printf(fails ? "voice_test: %d FAIL\n" : "voice_test: OK\n", fails);
    return fails ? 1 : 0;
}
