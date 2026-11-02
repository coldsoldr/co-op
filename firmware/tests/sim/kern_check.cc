/* Blackfin-simulator check of the T4 assembly kernels (coop_stereo.h: the per-voice difference accumulate and
 * the four mix loops) against their portable C references (the _c versions, the code that defined the sound up
 * to T3g). Random vectors plus the edge values (0, +-1, 32767, -32768) in every operand. Run with
 * tests/sim/run_kern_check.sh (bfin-elf-g++ -mcpu=bf524 + bfin-elf-run). Not a device artifact. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "coop_stereo.h"

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const int16_t EDGE[] = { 0, 1, -1, 32767, -32768, 16384, -16384, 23170, -23170 };
static int16_t r16(void)
{
    uint32_t r = rnd();
    if ((r & 7) == 0) return EDGE[(r >> 3) % (sizeof EDGE / sizeof EDGE[0])];
    return (int16_t)(r >> 8);
}
static int32_t r32(void) { return (int32_t)rnd(); }

int main(void)
{
    long bad_synth = 0, bad_drum = 0, bad_a = 0, bad_am = 0, bad_b = 0, bad_bm = 0;
    static int16_t out[COOP_VBLK], env[COOP_VBLK], M[COOP_MBLK], dL16[COOP_MBLK], dR16[COOP_MBLK];
    static int32_t dL1[COOP_VBLK], dR1[COOP_VBLK], dL2[COOP_VBLK], dR2[COOP_VBLK];
    static int16_t L1[COOP_MBLK], R1[COOP_MBLK], L2[COOP_MBLK], R2[COOP_MBLK];
    for (int it = 0; it < 3000; it++) {
        for (int i = 0; i < COOP_VBLK; i++) { out[i] = r16(); env[i] = r16(); dL1[i] = dL2[i] = r32() >> 4; dR1[i] = dR2[i] = r32() >> 4; }
        int16_t gain = r16();
        coop_pan_delta d; d.dL = r16(); d.dR = r16();
        coop_side_accum_synth_c(dL1, dR1, out, env, gain, d);
        coop_side_accum_synth(dL2, dR2, out, env, gain, d);
        if (memcmp(dL1, dL2, sizeof dL1) || memcmp(dR1, dR2, sizeof dR1)) bad_synth++;
        for (int i = 0; i < COOP_VBLK; i++) { dL1[i] = dL2[i] = r32() >> 4; dR1[i] = dR2[i] = r32() >> 4; }
        coop_side_accum_drum_c(dL1, dR1, out, gain, d);
        coop_side_accum_drum(dL2, dR2, out, gain, d);
        if (memcmp(dL1, dL2, sizeof dL1) || memcmp(dR1, dR2, sizeof dR1)) bad_drum++;

        for (int i = 0; i < COOP_MBLK; i++) { M[i] = r16(); dL16[i] = r16(); dR16[i] = r16(); L1[i] = L2[i] = r16(); R1[i] = R2[i] = r16(); }
        int16_t gL = r16(), gR = r16(), step = r16(), level = r16();
        volatile int16_t lvl = level;
        volatile int16_t rec1[2], rec2[2];
        rec1[0] = rec2[0] = r16(); rec1[1] = rec2[1] = (int16_t)(r16() & 0x7fff);
        int16_t ra = coop_mix_a_c(L1, R1, M, dL16, dR16, gL, gR, step, rec1, &lvl);
        int16_t rb = coop_mix_a(L2, R2, M, dL16, dR16, gL, gR, step, rec2, &lvl);
        if (ra != rb || rec1[0] != rec2[0] || rec1[1] != rec2[1] || memcmp(L1, L2, sizeof L1) || memcmp(R1, R2, sizeof R1)) bad_a++;

        for (int i = 0; i < COOP_MBLK; i++) { L1[i] = L2[i] = r16(); R1[i] = R2[i] = r16(); }
        rec1[0] = rec2[0] = r16(); rec1[1] = rec2[1] = (int16_t)(r16() & 0x7fff);
        ra = coop_mix_a_mono_c(L1, R1, M, gL, gR, step, rec1, &lvl);
        rb = coop_mix_a_mono(L2, R2, M, gL, gR, step, rec2, &lvl);
        if (ra != rb || rec1[0] != rec2[0] || rec1[1] != rec2[1] || memcmp(L1, L2, sizeof L1) || memcmp(R1, R2, sizeof R1)) bad_am++;

        for (int i = 0; i < COOP_MBLK; i++) { L1[i] = L2[i] = r16(); R1[i] = R2[i] = r16(); }
        int16_t ramp1 = r16(), ramp2 = ramp1;
        coop_mix_b_c(L1, R1, M, dL16, dR16, gain, &ramp1, step);
        coop_mix_b(L2, R2, M, dL16, dR16, gain, &ramp2, step);
        if (ramp1 != ramp2 || memcmp(L1, L2, sizeof L1) || memcmp(R1, R2, sizeof R1)) bad_b++;

        for (int i = 0; i < COOP_MBLK; i++) { L1[i] = L2[i] = r16(); R1[i] = R2[i] = r16(); }
        ramp1 = ramp2 = r16();
        coop_mix_b_mono_c(L1, R1, M, gain, &ramp1, step);
        coop_mix_b_mono(L2, R2, M, gain, &ramp2, step);
        if (ramp1 != ramp2 || memcmp(L1, L2, sizeof L1) || memcmp(R1, R2, sizeof R1)) bad_bm++;
    }
    printf("kernels vs C over 3000 blocks: accum_synth %ld accum_drum %ld mix_a %ld mix_a_mono %ld mix_b %ld mix_b_mono %ld mismatching blocks\n",
           bad_synth, bad_drum, bad_a, bad_am, bad_b, bad_bm);
    return (bad_synth | bad_drum | bad_a | bad_am | bad_b | bad_bm) ? 1 : 0;
}
