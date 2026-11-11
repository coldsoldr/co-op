// Host tests for coop_fixed.h / coop_dsp.h, checked against a float/int64 reference.
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "coop_fixed.h"
#include "coop_dsp.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while(0)

int main() {
    // mul_q15 vs reference (rounded), with saturation at the extremes
    for (int a = -32768; a <= 32767; a += 257) {
        for (int b = -32768; b <= 32767; b += 257) {
            int32_t ref = (int32_t)std::lround((double)a * b / 32768.0);
            if (ref > 32767) ref = 32767;
            if (ref < -32768) ref = -32768;
            int16_t got = coop_mul_q15((int16_t)a, (int16_t)b);
            CHECK(std::abs(got - ref) <= 1);  // +-1 LSB for rounding convention
        }
    }
    CHECK(coop_mul_q15(32767, 32767) == 32766);     // ~1.0 * 1.0
    CHECK(coop_mul_q15(-32768, 32767) == -32767);   // saturates toward -1
    CHECK(coop_sat16(70000) == 32767);
    CHECK(coop_sat16(-70000) == -32768);
    CHECK(coop_add16(30000, 30000) == 32767);       // saturating add

    // pan: hard left, centre, hard right
    const int N = 8;
    int16_t in[N], L[N], R[N];
    for (int i = 0; i < N; i++) in[i] = 10000;
    coop_pan_mono_to_stereo(in, L, R, 0, N);         // hard left
    CHECK(L[0] == 10000 - 1 || L[0] == 10000);       // gL ~= 1.0
    CHECK(R[0] == 0);
    coop_pan_mono_to_stereo(in, L, R, 32767, N);     // hard right
    CHECK(R[0] >= 9999);
    CHECK(L[0] == 0);
    coop_pan_mono_to_stereo(in, L, R, 16384, N);     // centre ~0.5 each
    CHECK(std::abs(L[0] - 5000) <= 2);
    CHECK(std::abs(R[0] - 5000) <= 2);

    printf(fails ? "fixed_test: %d FAIL\n" : "fixed_test: OK\n", fails);
    return fails ? 1 : 0;
}
