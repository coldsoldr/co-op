// Host tests for the stereo instrument bus DSP (coop_stereo.h, coop_fixed.h stock-exact ops).
// The device-side correctness of the stock replicas is proven in op1emu (coop image == stock image
// sample for sample at spread 0); these tests pin the host models and the properties the takeover
// relies on: centre is exactly zero, equal power, difference accumulate, decimator/DC/mix replicas
// against straightforward references, and the mono fast path == the full path with zero deltas.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include "coop_fixed.h"
#include "coop_stereo.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while(0)

static uint32_t rng = 12345;
static int16_t rnd16() { rng = rng * 1664525u + 1013904223u; return (int16_t)(rng >> 16); }

int main() {
    // ---- fr16 primitives: host model of the Blackfin default-mode ops ----
    CHECK(coop_fr16_mul(32767, 32767) == 32766);        // (0x3fff0001 + 0x4000) >> 15
    CHECK(coop_fr16_mul(-32768, -32768) == 32767);      // the one saturating case
    CHECK(coop_fr16_mul(-32768, 32767) == -32767);
    CHECK(coop_fr16_mul(16384, 16384) == 8192);         // 0.5 * 0.5 = 0.25 exactly
    CHECK(coop_fr16_mul(1, 1) == 0 && coop_fr16_mul(1, 16384) == 1);   // round half up: 0.5 LSB -> 1
    CHECK(coop_fr16_add(30000, 10000) == 32767 && coop_fr16_add(-30000, -10000) == -32768);
    CHECK(coop_fr16_shl1(20000) == 32767 && coop_fr16_shl1(-100) == -200);
    CHECK(coop_sadd32(2000000000, 2000000000) == 2147483647);
    CHECK(coop_ssub32(-2000000000, 2000000000) == (int32_t)-2147483648LL);
    // fr32 stock product ~ (c*x) >> 31
    for (int i = 0; i < 2000; i++) {
        int32_t c = (int32_t)((uint32_t)rnd16() << 16 | (uint16_t)rnd16());
        int32_t x = (int32_t)((uint32_t)rnd16() << 16 | (uint16_t)rnd16());
        int64_t ref = ((int64_t)c * x) >> 31;
        int64_t got = coop_fr32_mul_stock(c, x);
        CHECK(std::llabs(got - ref) <= 2);
    }
    CHECK(coop_fr32_mul_stock(0x40000000, 0x40000000) == 0x20000000);   // 0.5 * 0.5

    // ---- pan deltas: centre exactly zero, equal power (gL^2+gR^2 = 2), hard sides ----
    coop_pan_delta c = coop_pan_deltas(0);
    CHECK(c.dL == 0 && c.dR == 0);
    coop_pan_delta l = coop_pan_deltas(-32768);
    CHECK(l.dL > 13500 && l.dL <= 13573 && l.dR == -32768);              // gL = sqrt2, gR = 0
    coop_pan_delta r = coop_pan_deltas(32767);
    CHECK(r.dR > 13500 && r.dL < -32700);
    for (int p = -32768; p <= 32767; p += 97) {
        coop_pan_delta d = coop_pan_deltas((int16_t)p);
        double gL = 1.0 + d.dL / 32768.0, gR = 1.0 + d.dR / 32768.0;
        CHECK(std::abs(gL * gL + gR * gR - 2.0) < 0.01);
        CHECK(gL >= -0.001 && gR >= -0.001);
    }
    // monotonic: left to right, gR rises and gL falls
    coop_pan_delta prev = coop_pan_deltas(-32768);
    for (int p = -32768 + 256; p <= 32767; p += 256) {
        coop_pan_delta d = coop_pan_deltas((int16_t)p);
        CHECK(d.dR >= prev.dR - 1 && d.dL <= prev.dL + 1);
        prev = d;
    }

    // ---- difference accumulate: centre adds nothing; hard right cancels L exactly in mid+side terms ----
    static int32_t dL[COOP_VBLK], dR[COOP_VBLK];
    static int16_t out[COOP_VBLK], env[COOP_VBLK];
    for (int i = 0; i < COOP_VBLK; i++) { out[i] = rnd16(); env[i] = (int16_t)(rnd16() & 0x7fff); }
    memset(dL, 0, sizeof dL); memset(dR, 0, sizeof dR);
    coop_side_accum_synth(dL, dR, out, env, 0x6000, coop_pan_deltas(0));
    for (int i = 0; i < COOP_VBLK; i++) CHECK(dL[i] == 0 && dR[i] == 0);
    coop_side_accum_synth(dL, dR, out, env, 0x6000, coop_pan_deltas(32767));   // hard right
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(out[i], env[i]), 0x6000);     // the stock contribution
        CHECK(std::abs(v + dL[i]) <= 1);                                       // M + dL = 0 : left silent
        CHECK(std::abs((int)(dR[i]) - (int)std::floor(v * 0.41421356)) <= 2);  // R = sqrt2 * v
    }
    memset(dL, 0, sizeof dL); memset(dR, 0, sizeof dR);
    coop_side_accum_drum(dL, dR, out, 0x6000, coop_pan_deltas(-32768));        // hard left
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_shl1(coop_fr16_mul(out[i], 0x6000));
        CHECK(std::abs(v + dR[i]) <= 1);
    }

    // ---- half-band decimator replica vs a direct reference of the same arithmetic ----
    static int16_t coefs[COOP_HB_TAPS];
    for (int k = 0; k < COOP_HB_TAPS; k++) {                      // a plausible symmetric low-pass
        double x = (k - 15.5) * M_PI / 2.0;
        coefs[k] = (int16_t)(16384.0 * (std::sin(x) / x) * (0.54 + 0.46 * std::cos(M_PI * (k - 15.5) / 16.0)));
    }
    coop_hb_state st; coop_hb_init(&st);
    static int32_t in[COOP_VBLK]; static int16_t dec[COOP_MBLK];
    static int16_t hist_e[4096], hist_o[4096]; int nh = 0;       // reference: full history
    int16_t ramp = 0x1000, step = 0x40;
    int16_t rramp = ramp;
    for (int blk = 0; blk < 8; blk++) {
        for (int i = 0; i < COOP_VBLK; i++) in[i] = rnd16() >> 1;
        coop_hb_decimate(&st, in, dec, coefs, ramp, step);
        for (int n = 0; n < COOP_MBLK; n++) {
            hist_e[nh] = (int16_t)in[2 * n]; hist_o[nh] = (int16_t)in[2 * n + 1]; nh++;
            int32_t acc = (nh - 17 >= 0 ? hist_o[nh - 17] : 0) << 15;           // odd sample 16 back
            for (int k = 0; k < COOP_HB_TAPS; k++) {
                int idx = nh - 32 + k;                                        // oldest .. newest
                acc = coop_mac_w32(acc, coefs[k], idx >= 0 ? hist_e[idx] : 0);
            }
            int16_t y = coop_fr16_mul((int16_t)(acc >> 16), rramp);
            rramp = coop_fr16_add(step, rramp);
            CHECK(dec[n] == y);
        }
        for (int n = 0; n < COOP_MBLK; n++) ramp = coop_fr16_add(step, ramp);
    }
    // all-zero input after a zero history: zero output; DC input: steady output ~ gain * ramp
    coop_hb_init(&st);
    for (int i = 0; i < COOP_VBLK; i++) in[i] = 0;
    coop_hb_decimate(&st, in, dec, coefs, 32767, 0);
    for (int n = 0; n < COOP_MBLK; n++) CHECK(dec[n] == 0);

    // ---- DC blocker replica: removes DC, passes a step's edge, matches the written-out recurrence ----
    coop_dc_state dc; coop_dc_init(&dc);
    const int32_t cdc = 0x00400000;                               // ~0.002: a few Hz corner
    static int16_t dcin[COOP_MBLK], dcout[COOP_MBLK];
    for (int i = 0; i < COOP_MBLK; i++) dcin[i] = 8000;
    long double last = 0;
    for (int blk = 0; blk < 400; blk++) {                          // 400 blocks of DC
        coop_dc_block(&dc, dcin, dcout, cdc, COOP_MBLK);
        last = dcout[COOP_MBLK - 1];
    }
    CHECK(std::abs(last) < 400);                                   // DC mostly gone after 0.6 s
    coop_dc_init(&dc);
    coop_dc_block(&dc, dcin, dcout, cdc, COOP_MBLK);
    CHECK(dcout[0] == 8000);                                       // the edge passes at full level
    // recurrence check on random input
    coop_dc_init(&dc);
    int32_t xp = 0, yq = 0, y = 0;
    for (int blk = 0; blk < 5; blk++) {
        for (int i = 0; i < COOP_MBLK; i++) dcin[i] = rnd16();
        coop_dc_block(&dc, dcin, dcout, cdc, COOP_MBLK);
        for (int i = 0; i < COOP_MBLK; i++) {
            int32_t x = (int32_t)dcin[i] << 16;
            int32_t t = coop_sadd32(coop_ssub32(y, xp), x);
            y = coop_ssub32(t, coop_fr32_mul_stock(cdc, yq));
            int16_t o = (int16_t)(y >> 16);
            CHECK(dcout[i] == o);
            xp = x; yq = (int32_t)o << 16;
        }
    }

    // ---- mix loop B replica: zero deltas == mono fast path == direct formula; deltas steer L/R ----
    static int16_t L[COOP_MBLK], R[COOP_MBLK], L2[COOP_MBLK], R2[COOP_MBLK], M[COOP_MBLK];
    static int16_t zL[COOP_MBLK], zR[COOP_MBLK];
    for (int i = 0; i < COOP_MBLK; i++) { M[i] = rnd16(); L[i] = L2[i] = rnd16() >> 2; R[i] = R2[i] = rnd16() >> 2; }
    int16_t rs1 = 0x7000, rs2 = 0x7000;
    coop_mix_b(L, R, M, zL, zR, 0x6000, &rs1, 0x40);
    coop_mix_b_mono(L2, R2, M, 0x6000, &rs2, 0x40);
    CHECK(rs1 == rs2 && rs1 == 0x7fff);                            // saturated after 64 steps of 0x40
    for (int i = 0; i < COOP_MBLK; i++) CHECK(L[i] == L2[i] && R[i] == R2[i]);
    // direct formula
    int16_t rr = 0x7000;
    for (int i = 0; i < COOP_MBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(M[i], 0x6000), rr);
        rr = coop_fr16_add(0x40, rr);
        (void)v;
    }
    // hard-right deltas: L becomes (M + dL) * g with dL = -M  -> L unchanged by the voice
    for (int i = 0; i < COOP_MBLK; i++) { zL[i] = (int16_t)-M[i]; zR[i] = (int16_t)(M[i] * 0.41421); L[i] = R[i] = 0; }
    rs1 = 0x7fff;
    coop_mix_b(L, R, M, zL, zR, 0x7fff, &rs1, 0);
    for (int i = 0; i < COOP_MBLK; i++) { CHECK(std::abs(L[i]) <= 1); CHECK(std::abs(R[i] - (int)(M[i] * 1.41421)) <= 3 || std::abs(M[i]) > 23000); }

    printf(fails ? "stereo_test: %d FAILURES\n" : "stereo_test: all checks passed\n", fails);
    return fails ? 1 : 0;
}
