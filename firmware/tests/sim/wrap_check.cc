/* Blackfin-simulator check of the e15 effect-wrap kernels (core/sdram/wrap_fast.S) against the C loops they replace
 * in plugin_host.cc (copied here verbatim in form), bit for bit, including the guard words around every array (no
 * store outside it). Random vectors, random Q31 words (the plugin may leave any low half), and the edge values in
 * every operand. Run with tests/sim/run_wrap_check.sh (bfin-elf-g++ -mcpu=bf524 + bfin-elf-run). Not a device artifact. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "coop_fixed.h"

extern "C" void coop_wrap_ms_in(const int16_t *const src[3], int32_t *lr);
extern "C" void coop_wrap_ms_out(const int32_t *lr, int16_t *const dst[3]);
extern "C" void coop_wrap_ms_out_sat(const int32_t *lr, int16_t *const dst[3]);
extern "C" void coop_wrap_lr_in(const int16_t *l, const int16_t *r, int32_t *lr);
extern "C" void coop_wrap_lr_out(const int32_t *lr, int16_t *l, int16_t *r);

#define N 64
#define G 8                                               /* guard words / halfwords on each side */

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const int16_t EDGE16[] = { 0, 1, -1, 2, -2, 32767, -32768, 32766, -32767, 16384, -16384 };
static const int32_t EDGE32[] = { 0, 1, -1, 0x7fffffff, (int32_t)0x80000000, 0x7fff0000, (int32_t)0x80000000 | 0xffff,
                                  0x0000ffff, (int32_t)0xffff0000, 0x00010000, (int32_t)0xfffeffff, 0x3fffffff };
static int16_t r16(int mode)
{
    uint32_t r = rnd();
    if (mode == 1 || (r & 7) == 0) return EDGE16[(r >> 3) % (sizeof EDGE16 / sizeof EDGE16[0])];
    return (int16_t)(r >> 8);
}
static int32_t r32(int mode)
{
    uint32_t r = rnd();
    if (mode == 1 || (r & 7) == 0) return EDGE32[(r >> 3) % (sizeof EDGE32 / sizeof EDGE32[0])];
    return (int32_t)r;
}

/* the C of plugin_host.cc (e14) */
static void c_ms_in(const int16_t *buf, const int16_t *dL, const int16_t *dR, int32_t *L, int32_t *R)
{
    for (int i = 0; i < N; i++) {
        L[i] = (int32_t)coop_sat16((int32_t)buf[i] + dL[i]) << 16;
        R[i] = (int32_t)coop_sat16((int32_t)buf[i] + dR[i]) << 16;
    }
}
static void c_ms_out(const int32_t *L, const int32_t *R, int16_t *buf, int16_t *dL, int16_t *dR)   /* FX 1 */
{
    for (int i = 0; i < N; i++) {
        int16_t l = coop_sat16(L[i] >> 16), r = coop_sat16(R[i] >> 16);
        int16_t m = (int16_t)(((int32_t)l + r) >> 1);
        buf[i] = m; dL[i] = (int16_t)(l - m); dR[i] = (int16_t)(r - m);
    }
}
static void c_s2i(int16_t *mid, int16_t *sl, int16_t *sr, int32_t *L, int32_t *R, const int32_t *L2, const int32_t *R2)
{   /* slot 2 at the instrument: carrier -> int16 l / r -> Q31; the effect's Q31 -> l / r -> carrier */
    int16_t l[N], r[N];
    for (int i = 0; i < N; i++) { l[i] = coop_sat16((int32_t)mid[i] + sl[i]); r[i] = coop_sat16((int32_t)mid[i] + sr[i]); }
    for (int i = 0; i < N; i++) { L[i] = (int32_t)l[i] << 16; R[i] = (int32_t)r[i] << 16; }
    for (int i = 0; i < N; i++) { l[i] = coop_sat16(L2[i] >> 16); r[i] = coop_sat16(R2[i] >> 16); }
    for (int i = 0; i < N; i++) {
        int32_t m = ((int32_t)l[i] + r[i]) >> 1;
        mid[i] = (int16_t)m; sl[i] = coop_sat16(l[i] - m); sr[i] = coop_sat16(r[i] - m);
    }
}

struct a16 { int16_t g0[G]; int16_t v[N]; int16_t g1[G]; };
struct a32 { int32_t g0[G]; int32_t v[2 * N]; int32_t g1[G]; };
static void guard16(a16 *a) { for (int i = 0; i < G; i++) a->g0[i] = a->g1[i] = (int16_t)0x5A5A; }
static void guard32(a32 *a) { for (int i = 0; i < G; i++) a->g0[i] = a->g1[i] = 0x5A5A5A5A; }
static int ok16(const a16 *a) { for (int i = 0; i < G; i++) if (a->g0[i] != (int16_t)0x5A5A || a->g1[i] != (int16_t)0x5A5A) return 0; return 1; }
static int ok32(const a32 *a) { for (int i = 0; i < G; i++) if (a->g0[i] != 0x5A5A5A5A || a->g1[i] != 0x5A5A5A5A) return 0; return 1; }

int main(void)
{
    static a16 m, sl, sr, m2, sl2, sr2, l16, r16b, l16b, r16c;
    static a32 lr, lr2;
    static int32_t Lc[N], Rc[N];
    long bad[5] = { 0, 0, 0, 0, 0 }, guards = 0;
    const long IT = 20000;
    for (long it = 0; it < IT; it++) {
        int mode = (it % 5 == 0) ? 1 : 0;                  /* every fifth block all edge values */
        guard16(&m); guard16(&sl); guard16(&sr); guard16(&m2); guard16(&sl2); guard16(&sr2);
        guard16(&l16); guard16(&r16b); guard16(&l16b); guard16(&r16c); guard32(&lr); guard32(&lr2);
        for (int i = 0; i < N; i++) { m.v[i] = r16(mode); sl.v[i] = r16(mode); sr.v[i] = r16(mode); }
        /* 1. ms_in */
        c_ms_in(m.v, sl.v, sr.v, Lc, Rc);
        for (int i = 0; i < 2 * N; i++) lr.v[i] = (int32_t)rnd();
        const int16_t *src[3] = { m.v, sl.v, sr.v };
        coop_wrap_ms_in(src, lr.v);
        if (memcmp(Lc, lr.v, sizeof Lc) || memcmp(Rc, lr.v + N, sizeof Rc)) bad[0]++;
        if (!ok32(&lr)) guards++;
        /* 2. ms_out from arbitrary Q31 (a plugin may leave any low half) */
        for (int i = 0; i < N; i++) { Lc[i] = r32(mode); Rc[i] = r32(mode); lr.v[i] = Lc[i]; lr.v[N + i] = Rc[i]; }
        c_ms_out(Lc, Rc, m2.v, sl2.v, sr2.v);
        int16_t *dst[3] = { m.v, sl.v, sr.v };
        coop_wrap_ms_out(lr.v, dst);
        if (memcmp(m.v, m2.v, sizeof m.v) || memcmp(sl.v, sl2.v, sizeof sl.v) || memcmp(sr.v, sr2.v, sizeof sr.v)) bad[1]++;
        if (!ok16(&m) || !ok16(&sl) || !ok16(&sr) || !ok32(&lr)) guards++;
        /* 3. lr_in */
        for (int i = 0; i < N; i++) { l16.v[i] = r16(mode); r16b.v[i] = r16(mode); }
        for (int i = 0; i < N; i++) { Lc[i] = (int32_t)l16.v[i] << 16; Rc[i] = (int32_t)r16b.v[i] << 16; }
        for (int i = 0; i < 2 * N; i++) lr.v[i] = (int32_t)rnd();
        coop_wrap_lr_in(l16.v, r16b.v, lr.v);
        if (memcmp(Lc, lr.v, sizeof Lc) || memcmp(Rc, lr.v + N, sizeof Rc)) bad[2]++;
        if (!ok32(&lr) || !ok16(&l16) || !ok16(&r16b)) guards++;
        /* 4. lr_out */
        for (int i = 0; i < N; i++) { lr.v[i] = r32(mode); lr.v[N + i] = r32(mode); }
        for (int i = 0; i < N; i++) { l16b.v[i] = coop_sat16(lr.v[i] >> 16); r16c.v[i] = coop_sat16(lr.v[N + i] >> 16); }
        coop_wrap_lr_out(lr.v, l16.v, r16b.v);
        if (memcmp(l16.v, l16b.v, sizeof l16.v) || memcmp(r16c.v, r16b.v, sizeof r16c.v)) bad[3]++;
        if (!ok16(&l16) || !ok16(&r16b) || !ok32(&lr)) guards++;
        /* 5. slot 2 at the instrument: e14's int16 detour against ms_in / ms_out direct */
        for (int i = 0; i < N; i++) { m.v[i] = m2.v[i] = r16(mode); sl.v[i] = sl2.v[i] = r16(mode); sr.v[i] = sr2.v[i] = r16(mode); }
        for (int i = 0; i < 2 * N; i++) lr2.v[i] = r32(mode);
        static int32_t L1c[N], R1c[N];
        c_s2i(m2.v, sl2.v, sr2.v, L1c, R1c, lr2.v, lr2.v + N);
        const int16_t *src2[3] = { m.v, sl.v, sr.v };
        coop_wrap_ms_in(src2, lr.v);
        int ok_in = !memcmp(L1c, lr.v, sizeof L1c) && !memcmp(R1c, lr.v + N, sizeof R1c);
        int16_t *dst2[3] = { m.v, sl.v, sr.v };
        coop_wrap_ms_out_sat(lr2.v, dst2);
        if (!ok_in || memcmp(m.v, m2.v, sizeof m.v) || memcmp(sl.v, sl2.v, sizeof sl.v) || memcmp(sr.v, sr2.v, sizeof sr.v)) bad[4]++;
    }
    printf("wrap_check: %ld blocks: ms_in bad %ld, ms_out bad %ld, lr_in bad %ld, lr_out bad %ld, slot2_instr bad %ld, guard hits %ld\n",
           IT, bad[0], bad[1], bad[2], bad[3], bad[4], guards);
    int fail = bad[0] || bad[1] || bad[2] || bad[3] || bad[4] || guards;
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
