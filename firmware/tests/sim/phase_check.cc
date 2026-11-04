/* Simulator check of core/sdram/phase_fast.S (run with tests/sim/run_phase_check.sh): the stock Phase render,
 * transcribed from the stock disassembly by phase_transcribe.py, with g_coop_fast_phase = 0 (the stock loop) against
 * the same render with 1 (the rewrite): random voices (flag 0 and 1, counts around p3 >>> 10), sine tables, parameters
 * (p3 >>> 10 zero, positive and negative) and pitch words (full-range and small, so both phases wrap, part B runs and
 * stops, and every saturation happens), the state carried over 60 calls per run. Not a device artifact; no stock bytes
 * in the repo (the transcription is made in a temp directory). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern "C" {
volatile uint32_t g_coop_fast_phase;
volatile uint32_t g_phase_hit_calls, g_phase_hit_wrap1, g_phase_hit_wrap2;
int16_t t_sine[8192 + 16];
int32_t t_dbuf[128 + 64];
uint32_t t_acc;
void t_phase_render(void *self, int16_t *out, const int16_t *params);
}
static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const int16_t EDGE[] = { 0, 1, -1, 32767, -32768, 16384, -16384, 23170, -23170, 1023, 1024, 2048, -1024 };
static int16_t r16(void)
{
    uint32_t r = rnd();
    if ((r & 7) == 0) return EDGE[(r >> 3) % (sizeof EDGE / sizeof EDGE[0])];
    if ((r & 7) == 1) return (int16_t)((r >> 8) & 0x1fff);   /* small p3: p3 >>> 10 in 0..7 */
    return (int16_t)(r >> 8);
}

int main(void)
{
    static uint8_t objA[0x20], objB[0x20];
    static int16_t outA[128 + 8], outB[128 + 8];
    long bad = 0, calls = 0, flag0 = 0;
    for (int run = 0; run < 60; run++) {
        for (int k = 0; k < 8192 + 16; k++) t_sine[k] = r16();
        for (int k = 0; k < 0x20; k++) objA[k] = (uint8_t)rnd();
        objA[0x14] = (run % 3) == 0 ? 0 : (run % 3) == 1 ? 1 : objA[0x14];
        *(int32_t *)(objA + 0x18) = (int32_t)(rnd() % 12) - 2;
        memcpy(objB, objA, sizeof objA);
        int16_t params[4];
        for (int k = 0; k < 4; k++) params[k] = r16();
        uint32_t acc = rnd();
        for (int it = 0; it < 60; it++) {
            int mode = (run + it) % 5;
            for (int k = 0; k < 128 + 64; k++) {
                uint32_t r = rnd();
                t_dbuf[k] = mode == 0 ? (int32_t)r : mode == 1 ? (int32_t)(r >> 6) : mode == 2 ? (int32_t)(r >> 3) - (1 << 28)
                          : mode == 3 ? (int32_t)(r >> 10) : (int32_t)(0x00800000 + (r >> 16));
            }
            if ((it % 7) == 0) for (int k = 0; k < 4; k++) params[k] = r16();
            for (int k = 0; k < 128 + 8; k++) outA[k] = outB[k] = (int16_t)rnd();
            if (!objA[0x14]) flag0++;
            g_coop_fast_phase = 0;
            t_acc = acc;
            t_phase_render(objA, outA + 4, params);
            uint32_t accA = t_acc;
            uint32_t h0 = g_phase_hit_calls;
            g_coop_fast_phase = 1;
            t_acc = acc;
            t_phase_render(objB, outB + 4, params);
            uint32_t accB = t_acc;
            acc = accA;
            int b = 0;
            if (g_phase_hit_calls != h0 + 1) b |= 8;
            if (memcmp(objA, objB, 0x1c)) b |= 1;
            if (memcmp(outA, outB, sizeof outA)) b |= 2;
            if (accA != accB) b |= 4;
            calls++;
            if (b) {
                if (bad < 5) {
                    printf("run %d it %d: bad %d\n", run, it, b);
                    for (int k = 0; k < 0x1c; k += 4)
                        if (*(uint32_t *)(objA + k) != *(uint32_t *)(objB + k)) printf("  obj+%#x: %08x %08x\n", k, (unsigned)*(uint32_t *)(objA + k), (unsigned)*(uint32_t *)(objB + k));
                    int n = 0;
                    for (int k = 0; k < 136 && n < 6; k++) if (outA[k] != outB[k]) { printf("  out[%d]: %d %d\n", k - 4, outA[k], outB[k]); n++; }
                }
                bad++;
                memcpy(objB, objA, sizeof objA);
            }
        }
    }
    printf("phase: %ld calls, %ld bad, hits %u, wrap1 %u, wrap2 %u, renders starting with flag 0 %ld\n", calls, bad,
           (unsigned)g_phase_hit_calls, (unsigned)g_phase_hit_wrap1, (unsigned)g_phase_hit_wrap2, flag0);
    return bad != 0;
}
