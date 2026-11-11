/* Blackfin-simulator check of the T4 assembly kernels (coop_stereo.h: the per-voice difference accumulate and
 * the four mix loops) against their portable C references (the _c versions, the code that defined the sound up
 * to T3g). Random vectors plus the edge values (0, +-1, 32767, -32768) in every operand. Run with
 * tests/sim/run_kern_check.sh (bfin-elf-g++ -mcpu=bf524 + bfin-elf-run). Not a device artifact. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "coop_stereo.h"

/* performance step 2: the S3 stage in assembly (core/sdram/coop_stage.S) against the C path of coop_instr_stage */
struct coop_stage_io {
    int32_t *vL, *vR; coop_hb2_state *hb; const int16_t *coefs; uint32_t ramp_step; coop_dc_state *dcL, *dcR; int32_t c;
    int16_t *mid, *sl, *sr, *pL, *pR;
};
extern "C" int coop_stage_asm(struct coop_stage_io *io);
static int16_t stage_c(int32_t *vL, int32_t *vR, coop_hb2_state *hb, const int16_t *coefs, uint32_t rw, coop_dc_state *dl,
                       coop_dc_state *dr, int32_t c, int16_t *mid, int16_t *sl, int16_t *sr, int16_t *pL, int16_t *pR)
{
    int16_t ramp = coop_hb_decimate2(hb, vL, vR, pL, pR, coefs, (int16_t)rw, (int16_t)(rw >> 16));
    coop_dc_block(dl, pL, pL, c, COOP_MBLK);
    coop_dc_block(dr, pR, pR, c, COOP_MBLK);
    for (int i = 0; i < COOP_MBLK; i++) {
        int32_t l = pL[i], r = pR[i], m = (l + r) >> 1;
        mid[i] = (int16_t)m; sl[i] = coop_sat16(l - m); sr[i] = coop_sat16(r - m);
    }
    for (int i = 0; i < COOP_VBLK; i++) vL[i] = vR[i] = 0;
    return ramp;
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
static int32_t r32(void) { return (int32_t)rnd(); }

int main(void)
{
    long bad_stage = 0;
    static coop_hb2_state sh1, sh2; static coop_dc_state sdl1, sdr1, sdl2, sdr2;
    coop_hb2_init(&sh1); coop_hb2_init(&sh2); coop_dc_init(&sdl1); coop_dc_init(&sdr1); coop_dc_init(&sdl2); coop_dc_init(&sdr2);
    static int32_t svL1[COOP_VBLK], svR1[COOP_VBLK], svL2[COOP_VBLK], svR2[COOP_VBLK];
    static int16_t smid1[COOP_MBLK], ssl1[COOP_MBLK], ssr1[COOP_MBLK], spL1[COOP_MBLK], spR1[COOP_MBLK];
    static int16_t smid2[COOP_MBLK], ssl2[COOP_MBLK], ssr2[COOP_MBLK], spL2[COOP_MBLK], spR2[COOP_MBLK];
    int32_t sc = 0x7f000000 + (int32_t)(rnd() & 0xffffff);
    long bad_psynth = 0, bad_pdrum = 0, bad_synth = 0, bad_drum = 0, bad_a = 0, bad_am = 0, bad_b = 0, bad_bm = 0, bad_hb = 0;
    static int32_t sL[COOP_VBLK], sR[COOP_VBLK];
    static int16_t oL1[COOP_MBLK], oR1[COOP_MBLK], oL2[COOP_MBLK], oR2[COOP_MBLK], coefs[COOP_HB_TAPS];
    static coop_hb_state hL, hR; static coop_hb2_state h2;
    coop_hb_init(&hL); coop_hb_init(&hR); coop_hb2_init(&h2);
    for (int k = 0; k < COOP_HB_TAPS; k++) coefs[k] = r16();
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
        for (int i = 0; i < COOP_VBLK; i++) { dL1[i] = dL2[i] = r32() >> 4; dR1[i] = dR2[i] = r32() >> 4; }
        coop_pair_accum_synth_c(dL1, dR1, out, env, gain, d);                   /* S3: the pair kernels */
        coop_pair_accum_synth(dL2, dR2, out, env, gain, d);
        if (memcmp(dL1, dL2, sizeof dL1) || memcmp(dR1, dR2, sizeof dR1)) bad_psynth++;
        for (int i = 0; i < COOP_VBLK; i++) { dL1[i] = dL2[i] = r32() >> 4; dR1[i] = dR2[i] = r32() >> 4; }
        coop_pair_accum_drum_c(dL1, dR1, out, gain, d);
        coop_pair_accum_drum(dL2, dR2, out, gain, d);
        if (memcmp(dL1, dL2, sizeof dL1) || memcmp(dR1, dR2, sizeof dR1)) bad_pdrum++;

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

        for (int i = 0; i < COOP_VBLK; i++) { sL[i] = (r32() >> 3) * ((i & 15) ? 1 : 3); sR[i] = (r32() >> 3) * ((i & 7) ? 1 : 3); }
        int16_t hr = r16(), hs = r16();
        coop_hb_decimate(&hL, sL, oL1, coefs, hr, hs);
        coop_hb_decimate(&hR, sR, oR1, coefs, hr, hs);
        coop_hb_decimate2(&h2, sL, sR, oL2, oR2, coefs, hr, hs);
        if (memcmp(oL1, oL2, sizeof oL1) || memcmp(oR1, oR2, sizeof oR1)) bad_hb++;

        for (int i = 0; i < COOP_VBLK; i++) {                                      /* the stage, state carried on */
            int32_t v = (r32() >> ((rnd() & 3) == 0 ? 0 : 3)), w = (r32() >> ((rnd() & 3) == 0 ? 0 : 3));
            svL1[i] = svL2[i] = v; svR1[i] = svR2[i] = w;
        }
        uint32_t rw = (uint32_t)(uint16_t)r16() | ((uint32_t)(uint16_t)(r16() >> 6) << 16);
        if ((it & 63) == 0) sc = (int32_t)rnd();
        int16_t ra1 = stage_c(svL1, svR1, &sh1, coefs, rw, &sdl1, &sdr1, sc, smid1, ssl1, ssr1, spL1, spR1);
        struct coop_stage_io io = { svL2, svR2, &sh2, coefs, rw, &sdl2, &sdr2, sc, smid2, ssl2, ssr2, spL2, spR2 };
        int16_t ra2 = (int16_t)coop_stage_asm(&io);
        if (ra1 != ra2 || memcmp(smid1, smid2, sizeof smid1) || memcmp(ssl1, ssl2, sizeof ssl1) || memcmp(ssr1, ssr2, sizeof ssr1)
            || memcmp(spL1, spL2, sizeof spL1) || memcmp(spR1, spR2, sizeof spR1) || memcmp(svL1, svL2, sizeof svL1)
            || memcmp(svR1, svR2, sizeof svR1) || memcmp(&sh1, &sh2, sizeof sh1) || memcmp(&sdl1, &sdl2, sizeof sdl1)
            || memcmp(&sdr1, &sdr2, sizeof sdr1)) {
            if (!bad_stage) printf("stage first mismatch at block %d: ramp %d/%d mid0 %d/%d pL0 %d/%d dcL.y %ld/%ld\n", it, ra1, ra2,
                                   smid1[0], smid2[0], spL1[0], spL2[0], (long)sdl1.y, (long)sdl2.y);
            bad_stage++;
            sh2 = sh1; sdl2 = sdl1; sdr2 = sdr1;
        }
    }
    printf("kernels vs C over 3000 blocks: pair_synth %ld pair_drum %ld accum_synth %ld accum_drum %ld mix_a %ld mix_a_mono %ld mix_b %ld mix_b_mono %ld hb_pair %ld stage %ld mismatching blocks\n",
           bad_psynth, bad_pdrum, bad_synth, bad_drum, bad_a, bad_am, bad_b, bad_bm, bad_hb, bad_stage);
    return (bad_stage | bad_psynth | bad_pdrum | bad_synth | bad_drum | bad_a | bad_am | bad_b | bad_bm | bad_hb) ? 1 : 0;
}
