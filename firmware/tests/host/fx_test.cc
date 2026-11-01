// Property tests for the coop FX slot (coop_fx.h): the stereo feedback delay.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "coop_fixed.h"
#include "coop_fx.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while(0)

static const int32_t ONE = 2147483647;
static const int64_t MINV = -2147483648LL;   // saturation floor (coop_sat32 min)
#include <cmath>

static int32_t lineL[64], lineR[64];

int main() {
    const int LEN = 64;

    // ---- bypass: mix == 0 leaves the signal untouched and never touches the line ----
    {
        coop_delay d; coop_delay_init(&d, lineL, lineR, LEN);  // mix defaults to 0
        int32_t L[8], R[8];
        for (int i = 0; i < 8; i++) { L[i] = (i + 1) << 24; R[i] = -(i + 1) << 24; }
        for (int blk = 0; blk < 4; blk++) {
            int32_t Lin[8], Rin[8];
            for (int i = 0; i < 8; i++) { Lin[i] = L[i]; Rin[i] = R[i]; }
            coop_delay_process(&d, L, R, 8);
            for (int i = 0; i < 8; i++) { CHECK(L[i] == Lin[i]); CHECK(R[i] == Rin[i]); }
        }
        CHECK(d.cleared == 0);   // bypass did not start the clear
    }

    // ---- an impulse reappears after `delay` samples (mix = 1, fb = 0) ----
    {
        coop_delay d; coop_delay_init(&d, lineL, lineR, LEN);
        coop_delay_set(&d, 10, 0, ONE);           // delay 10, no feedback, full wet
        // feed one block of silence to let the spread-clear finish (LEN/CLEAR_CHUNK <= 1 block here)
        int32_t z[LEN]; for (int i = 0; i < LEN; i++) z[i] = 0;
        coop_delay_process(&d, z, z, LEN);         // clears the line (LEN <= CLEAR_CHUNK)
        CHECK(d.cleared == LEN);
        // impulse at t=0, then silence; expect a copy at t=10
        int32_t L[40], R[40];
        for (int i = 0; i < 40; i++) { L[i] = R[i] = 0; }
        L[0] = 1 << 28; R[0] = 1 << 27;
        coop_delay_process(&d, L, R, 40);
        // dry(0) + wet*delayed; mix = ONE is ~0.99999 (Q31), so within a couple of LSB
        CHECK(std::llabs((long long)L[10] - (1 << 28)) < 4);
        CHECK(std::llabs((long long)R[10] - (1 << 27)) < 4);
        CHECK(L[9] == 0 && L[11] == 0);            // only at the delay tap
    }

    // ---- feedback produces decaying repeats ----
    {
        coop_delay d; coop_delay_init(&d, lineL, lineR, LEN);
        coop_delay_set(&d, 5, ONE / 2, ONE);       // delay 5, fb 0.5, full wet
        int32_t z[LEN]; for (int i = 0; i < LEN; i++) z[i] = 0;
        coop_delay_process(&d, z, z, LEN);         // clear
        int32_t L[32], R[32];
        for (int i = 0; i < 32; i++) { L[i] = R[i] = 0; }
        L[0] = 1 << 28; R[0] = 1 << 28;
        coop_delay_process(&d, L, R, 32);
        // echoes at 5, 10, 15, ... each ~half the previous
        CHECK(std::llabs((long long)L[5] - (1 << 28)) < 4);
        CHECK(std::llabs((long long)L[10] - (1 << 27)) < (1 << 20));   // ~0.5
        CHECK(std::llabs((long long)L[15] - (1 << 26)) < (1 << 20));   // ~0.25
        CHECK(L[10] < L[5] && L[15] < L[10]);                          // decaying
    }

    // ---- no overflow: full-scale input with feedback stays saturated, not wrapped ----
    {
        coop_delay d; coop_delay_init(&d, lineL, lineR, LEN);
        coop_delay_set(&d, 3, (int32_t)(0.9 * ONE), ONE);
        int32_t z[LEN]; for (int i = 0; i < LEN; i++) z[i] = 0;
        coop_delay_process(&d, z, z, LEN);
        int32_t L[64], R[64];
        for (int i = 0; i < 64; i++) { L[i] = ONE; R[i] = -ONE; }
        coop_delay_process(&d, L, R, 64);
        for (int i = 0; i < 64; i++) {
            CHECK((int64_t)L[i] <= ONE && (int64_t)L[i] >= MINV);
            CHECK((int64_t)R[i] <= ONE && (int64_t)R[i] >= MINV);
        }
    }

    printf(fails ? "fx_test: %d FAIL\n" : "fx_test: OK\n", fails);
    return fails ? 1 : 0;
}
