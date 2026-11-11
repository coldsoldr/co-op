/* Simulator check of core/sdram/voltage_fast.S (run with tests/sim/run_voltage_check.sh): the stock Voltage render,
 * transcribed from the stock disassembly by voltage_transcribe.py, with g_coop_fast_voltage = 0 (the stock loop) against
 * the same render with 1 (the rewrite): random voices (both Kf cases), sine / K38 tables, parameters and pitch words
 * (full-range, so phase wraps and every saturation happen), the state carried over 60 calls per run. Not a device
 * artifact; no stock bytes in the repo (the transcription is made in a temp directory). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern "C" {
volatile uint32_t g_coop_fast_voltage;
volatile uint32_t g_voltage_hit_calls;
int16_t t_sine[8192 + 16];
int16_t t_tab[256];
int32_t t_dbuf[128 + 64];
int16_t t_scope[32];
void t_voltage_render(void *self, int16_t *out, const int16_t *params);
}
static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const int16_t EDGE[] = { 0, 1, -1, 32767, -32768, 16384, -16384, 23170, -23170 };
static int16_t r16(void)
{
    uint32_t r = rnd();
    if ((r & 7) == 0) return EDGE[(r >> 3) % (sizeof EDGE / sizeof EDGE[0])];
    return (int16_t)(r >> 8);
}

int main(void)
{
    static uint8_t objA[0x40], objB[0x40];
    static int16_t outA[128 + 8], outB[128 + 8], scA[32], scB[32];
    long bad = 0, calls = 0, wraps = 0;
    for (int run = 0; run < 40; run++) {
        for (int k = 0; k < 8192 + 16; k++) t_sine[k] = r16();
        for (int k = 0; k < 256; k++) t_tab[k] = r16();
        for (int k = 0; k < 0x40; k++) objA[k] = (uint8_t)rnd();
        if (run & 1) objA[0x30] = 0;
        memcpy(objB, objA, sizeof objA);
        int16_t params[4];
        for (int k = 0; k < 4; k++) params[k] = r16();
        for (int it = 0; it < 60; it++) {
            int mode = (run + it) % 4;
            for (int k = 0; k < 128 + 64; k++) {
                uint32_t r = rnd();
                t_dbuf[k] = mode == 0 ? (int32_t)r : mode == 1 ? (int32_t)(r >> 6) : mode == 2 ? (int32_t)(r >> 3) - (1 << 28) : (int32_t)(r >> 10);
            }
            if ((it % 7) == 0) for (int k = 0; k < 4; k++) params[k] = r16();
            for (int k = 0; k < 128 + 8; k++) outA[k] = outB[k] = (int16_t)rnd();
            g_coop_fast_voltage = 0;
            t_voltage_render(objA, outA + 4, params);
            memcpy(scA, t_scope, sizeof scA);
            uint32_t h0 = g_voltage_hit_calls;
            g_coop_fast_voltage = 1;
            t_voltage_render(objB, outB + 4, params);
            memcpy(scB, t_scope, sizeof scB);
            int b = 0;
            if (g_voltage_hit_calls != h0 + 1) b |= 8;
            if (memcmp(objA, objB, sizeof objA)) b |= 1;
            if (memcmp(outA, outB, sizeof outA)) b |= 2;
            if (memcmp(scA, scB, sizeof scA)) b |= 4;
            calls++;
            if (b) {
                if (bad < 5) {
                    printf("run %d it %d: bad %d\n", run, it, b);
                    for (int k = 0; k < 0x34; k += 2)
                        if (*(int16_t *)(objA + k) != *(int16_t *)(objB + k)) printf("  obj+%#x: %04x %04x\n", k, (uint16_t)*(int16_t *)(objA + k), (uint16_t)*(int16_t *)(objB + k));
                    int n = 0;
                    for (int k = 0; k < 136 && n < 6; k++) if (outA[k] != outB[k]) { printf("  out[%d]: %d %d\n", k - 4, outA[k], outB[k]); n++; }
                }
                bad++;
                memcpy(objB, objA, sizeof objA);
            }
        }
    }
    printf("voltage: %ld calls, %ld bad, hits %u\n", calls, bad, (unsigned)g_voltage_hit_calls);
    return bad != 0;
}
