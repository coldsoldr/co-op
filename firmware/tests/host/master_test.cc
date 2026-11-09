// Property tests for the master bus suite (coop_master.h): compressor, drive, EQ.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include "coop_fixed.h"
#include "coop_master.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while(0)

static const int32_t ONE = 2147483647;        // ~ +1.0 Q31
static const int32_t HALF = 1 << 30;           // 0.5

int main() {
    // ---- Q31 helpers ----
    CHECK(coop_mul_q31(ONE, ONE) >= ONE - 2);
    CHECK(coop_mul_q31(HALF, HALF) == (1 << 29));          // 0.25 in Q31
    CHECK(coop_sat32((int64_t)ONE + ONE) == ONE);
    CHECK(coop_abs32(-12345) == 12345);

    // ---- drive ----
    // clean (drive=0): small signal passes ~linearly (cubic 1.5x-0.5x^3 ~ x for small x)
    int32_t small = 1 << 20;
    int32_t d0 = coop_drive_sample(small, 0);
    CHECK(std::llabs((long long)d0 - small) < (small / 20));   // within 5%
    // drive saturates: a near-full input with drive stays bounded to +-1 and is monotonic
    int32_t hi = coop_drive_sample(ONE, ONE / 2);
    int32_t lo = coop_drive_sample(-ONE, ONE / 2);
    CHECK(hi <= ONE && hi > 0);
    CHECK(lo >= -ONE && lo < 0);
    CHECK(hi == -lo || std::llabs((long long)hi + lo) < 4);     // odd symmetry

    // ---- compressor ----
    coop_comp_state c; c.env = 0;
    int32_t thresh = HALF;            // 0.5
    int32_t ratio_inv = HALF;         // 2:1
    // quiet signal below threshold: unchanged (unity makeup)
    for (int i = 0; i < 200; i++) coop_comp_sample(&c, 1 << 26, thresh, ratio_inv, 1 << 20, 20, 2, 7);
    int32_t q = coop_comp_sample(&c, 1 << 26, thresh, ratio_inv, 1 << 20, 20, 2, 7);
    CHECK(std::llabs((long long)q - (1 << 26)) < (1 << 22));    // ~unchanged
    // loud signal above threshold: gain reduced (output < input)
    coop_comp_state c2; c2.env = 0;
    for (int i = 0; i < 400; i++) coop_comp_sample(&c2, ONE, thresh, ratio_inv, 1 << 20, 20, 2, 7);
    int32_t loud = coop_comp_sample(&c2, ONE, thresh, ratio_inv, 1 << 20, 20, 2, 7);
    CHECK(loud < ONE - (ONE / 8));                              // clearly compressed

    // ---- biquad ----
    // bypass (b0=1, rest 0) passes through
    coop_biquad_state b; b.s1 = b.s2 = 0;
    int32_t x = 123456789;
    CHECK(coop_biquad_sample(&b, x, 1 << COOP_EQ_FRAC, 0, 0, 0, 0) == x);
    // a DC-blocking highpass (b0=1, b1=-1, a1=-0.99) drives a constant input toward 0 over time
    coop_biquad_state hp; hp.s1 = hp.s2 = 0;
    int32_t a1 = -(int32_t)(0.99 * (1 << COOP_EQ_FRAC));
    int32_t out = 0;
    for (int i = 0; i < 2000; i++) out = coop_biquad_sample(&hp, HALF, 1 << COOP_EQ_FRAC, -(1 << COOP_EQ_FRAC), 0, a1, 0);
    CHECK(std::llabs((long long)out) < (HALF / 50));           // DC removed

    // ---- the suite: init is transparent (no comp, no drive, bypass EQ) ----
    coop_master m; coop_master_init(&m);
    int32_t L[8], R[8], Lref[8], Rref[8];
    for (int i = 0; i < 8; i++) { L[i] = Lref[i] = (i - 4) * (1 << 24); R[i] = Rref[i] = (4 - i) * (1 << 24); }
    coop_master_process(&m, L, R, 8);
    for (int i = 0; i < 8; i++) {
        CHECK(std::llabs((long long)L[i] - Lref[i]) < (1 << 20));   // ~identity
        CHECK(std::llabs((long long)R[i] - Rref[i]) < (1 << 20));
    }

    printf(fails ? "master_test: %d FAIL\n" : "master_test: OK\n", fails);
    return fails ? 1 : 0;
}
