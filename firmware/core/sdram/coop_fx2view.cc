/*
 * coop-fw u7: FX 2 shown on the STOCK effect page (the owner, 2026-11-07: "it should just look like the stock fx").
 *
 * Every stock effect screen's Update reads its dials through read_fx_param (0x01A28DA0(slot, pos): the position's
 * knob words normalised with the range table into a static std::vector<float>, begin at +4) and its animation
 * through the display getter (0x01A28B34(pos, n, a): the display slots 0xFF801774 + 16 * s, n words, as floats), one
 * CALL of each per Update at sixteen known sites (research 2026-11-07, OP1_vdk.dis). coop retargets those sixteen
 * CALLs at the two wrappers below: they call the stock first (a built vector of the right size) and, while FX 2 of
 * that position is in focus and holds a stock type, overwrite the vector's floats with slot 2's: the knobs as
 * q15 / 32767 (the stock's own soft-float int-to-float 0x01AA7050 and divide 0x01AA54C8: coop has no float), the
 * animation from the slot-2 twin's own display words (plugin_host.cc coop_slot2_disp: the master sums L + R as the
 * stock does). Dials, numbers and animation then follow with no change to the page.
 *
 * The page itself: the screen id is posted like a key event (0x019D7C94(id, slot, pos), a 64-entry ring the gfx
 * thread pops in screens_update at 0x01A3BB40). On focus coop posts for_fx(type 2); and because every handled key
 * reposts FX 1's id, the pop CALL is retargeted at coop_screen_pop, which rewrites an effect-page id (8..15) to FX
 * 2's while it is in focus. A plugin in FX 2 has no stock page: coop's overlay stays for it (coop_ui.cc).
 *
 * Gfx thread (the wrappers, the pop), KeyHandler thread (the posts). No float, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define STOCK_PARAM ((void *(*)(int, int))0x01A28DA0u)                    /* read_fx_param(slot, pos) -> std::vector<float> * */
#define STOCK_DISP  ((void *(*)(int, int, int))0x01A28B34u)               /* display getter(pos, n, a) -> std::vector<float> * */
#define STOCK_POP   ((int (*)(int *, int *, int *))0x019D7D70u)           /* the screen-event pop(&id, &slot, &pos) */
#define POST_SCREEN ((void (*)(int, int, int))0x019D7C94u)                /* post(id, slot, pos) */
#define I2F         ((uint32_t (*)(int))0x01AA7050u)                      /* the stock's int -> float (bits in R0) */
#define FDIV        ((uint32_t (*)(uint32_t, uint32_t))0x01AA54C8u)       /* the stock's float divide */
#define FOR_FX      ((int (*)(int))0x01A3BEC8u)                           /* type 0..7 -> screen 8..15 */
#define FSUB        ((uint32_t (*)(uint32_t, uint32_t))0x01AA53C8u)       /* the stock's float subtract */
#define SYNTH_INST  (*(volatile int *)0xFF8016D4u)
#define DRUM_KIT    (*(volatile int *)0xFF8016D8u)
#define FX_TYPE_S(i) (*(volatile int *)(0xFF801734u + (uint32_t)(i) * 4))
#define FX_TYPE_D(i) (*(volatile int *)(0xFF801754u + (uint32_t)(i) * 4))
#define FX_TYPE_M   (*(volatile int *)0xFF801090u)

extern "C" int coop_fx2_focused(int pos2);                                /* coop_ui.cc: 0 instrument, 1 master */
extern "C" int coop_slot2_req_type(int pos2);                             /* plugin_host.cc: the stock type ASKED FOR (-1: none / a plugin);
                                                                             the audio thread builds it later, the page must not wait */
extern "C" int coop_slot2_knob_q15(int pos2, int k);
extern "C" int coop_slot2_disp(int pos2, int i);

volatile uint32_t g_fx2view_params, g_fx2view_disp, g_fx2view_pops, g_fx2view_empty, g_fx2view_size;   /* stats / tests; empty: the stock vector had no entries while FX 2 was in focus */
volatile uint32_t g_fx2view_ids, g_fx2view_last_post, g_fx2view_cur;    /* debug: FOR_FX(0..7) as nibbles, the last id coop posted, the screen shown at the last read */
#define CUR_SCREEN  (*(volatile int *)0x018B65C8u)

static int focused_stock(int pos)                                          /* the stock position 0 / 1 / 3 */
{
    int p2 = pos == 3 ? 1 : 0;
    return coop_fx2_focused(p2) && coop_slot2_req_type(p2) >= 0 ? p2 + 1 : 0;   /* 1 + pos2, or 0 */
}
static uint32_t *vec_begin(void *v) { return *(uint32_t **)((uint8_t *)v + 4); }
static uint32_t s_vec_buf[8];                                              /* coop's own vector storage (8 floats) */
static uint32_t s_vec_on[4];                                               /* {0, begin, end, cap}: the stock vector's shape */
extern "C" int coop_slot2_bypassed(int pos2);                              /* plugin_host.cc */
extern "C" int coop_lfo2_focused(int pos);                                 /* coop_ui.cc: LFO 2 in focus for the instrument (0 synth, 1 drum) */
extern "C" int coop_lfo2_kind_type(int *on);                               /* lfo2.cc: the shown sound's LFO 2 type (7 none) */
volatile uint32_t g_lfo2view_params, g_lfo2view_pops;                      /* u8: the LFO page fed LFO 2 */

extern "C" void *coop_fx_param_read(int slot, int pos)
{
    void *v = STOCK_PARAM(slot, pos);
    int f = focused_stock(pos);
    if (!f || !v) return v;
    g_fx2view_cur = (uint32_t)CUR_SCREEN;
    if (!g_fx2view_ids) { uint32_t m = 0; for (int t = 0; t < 8; t++) m |= (uint32_t)(FOR_FX(t) & 15) << (4 * t); g_fx2view_ids = m; }
    uint32_t *b = vec_begin(v), *e = *(uint32_t **)((uint8_t *)v + 8);
    g_fx2view_size = (uint32_t)(e - b);
    /* Never an empty vector: every effect screen's Update asserts size >= 4 (the cwo screen 0x019427DC and the spring
     * screen 0x01A33FCE jump to the L1 abort 0xFFA0A1E0 otherwise; op1emu froze whole on it, 2026-11-08). The stock's
     * big "FX OFF" is not this vector, it is FX 1's on / off flag (its reader: coop_fx_on_read below). When the
     * stock's own vector has no entries (FX 1 none) coop's 8-float one stands in. */
    if (!b || e - b < 4) { b = s_vec_buf; e = s_vec_buf + 8; s_vec_on[1] = (uint32_t)(uintptr_t)b; s_vec_on[2] = (uint32_t)(uintptr_t)e; v = s_vec_on; g_fx2view_empty++; }
    /* As the stock fills it (0x01A28E74): entry k = (raw_k - min_k) / (max_k - min_k) with the type's Ranges row
     * (0x01DE5C50 + 48 * type, int16 {min, max, default} x 8), entries with max == min left as they are. raw 0..3 are
     * the knob words in range units (slot 2's q15 knobs mapped as the render maps them, plugin_host.cc knob_map),
     * raw 4..7 the row's defaults (what slot 2's twin is given). A q15 / 32767 here showed every dial at 0 (u7). */
    int t = coop_slot2_req_type(f - 1);
    const int16_t *rg = (const int16_t *)(0x01DE5C50u + (uint32_t)(t & 7) * 0x30u);
    int n = (int)(e - b); if (n > 8) n = 8;
    for (int k = 0; k < n; k++) {
        int lo = rg[3 * k], hi = rg[3 * k + 1];
        if (hi == lo) continue;
        int raw;
        if (k < 4) { int a = lo, c = hi; if (c < a) { int s = a; a = c; c = s; } raw = a + (int)(((uint32_t)(c - a) * (uint32_t)coop_slot2_knob_q15(f - 1, k)) >> 15); }
        else raw = rg[3 * k + 2];
        b[k] = FDIV(FSUB(I2F(raw), I2F(lo)), FSUB(I2F(hi), I2F(lo)));
    }
    g_fx2view_params++;
    return v;
}
extern "C" void *coop_fx_disp_read(int pos, int n, int a)
{
    void *v = STOCK_DISP(pos, n, a);
    int f = focused_stock(pos);
    if (!f || !v) return v;
    uint32_t *b = vec_begin(v);
    if (!b) return v;
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) b[i] = I2F(coop_slot2_disp(f - 1, i));
    g_fx2view_disp++;
    return v;
}
extern "C" int coop_screen_pop(int *id, int *slot, int *pos)
{
    int r = STOCK_POP(id, slot, pos);
    if (r && *id >= 8 && *id <= 15) {
        int f = focused_stock(*pos);
        if (f) { *id = FOR_FX(coop_slot2_req_type(f - 1)); g_fx2view_pops++; }
    }
    if (r && *id >= 24 && *id <= 30 && *pos >= 0 && *pos <= 1 && coop_lfo2_focused(*pos)) {   /* u8: the LFO page shows LFO 2's type */
        int on, t = coop_lfo2_kind_type(&on);
        if (t >= 0 && t <= 2) { *id = 24 + t; g_lfo2view_pops++; }
    }
    return r;
}

/* ---- u8: LFO 2 on the stock LFO page. The seven LFO screens read their eight dials through read_lfo_param
 * (0x01A293BC(slot, pos) -> a static std::vector<float>, (v - min) / (max - min) with the type's Ranges row), one
 * CALL each; those seven CALLs are retargeted here. While LFO 2 is in focus for the instrument the vector gets the
 * sound's LFO 2 parameters normalised with LFO 2's own type ranges. Gfx thread. ---- */
#define STOCK_LFO_PARAM ((void *(*)(int, int))0x01A293BCu)
#define LFO_TYPE_S(i)   (*(volatile int *)(0xFF801384u + (uint32_t)(i) * 4))  /* LFO 1's type per synth slot / drum kit */
#define LFO_TYPE_D(i)   (*(volatile int *)(0xFF8013A4u + (uint32_t)(i) * 4))
extern "C" int coop_sound_lfo2(int kind, int slot, int *type, int *on, int16_t *p8);
extern "C" const int16_t *coop_lfo2_ranges(int type);
extern "C" void *coop_lfo_param_read(int slot, int pos)
{
    void *v = STOCK_LFO_PARAM(slot, pos);
    if (pos < 0 || pos > 1 || !v || !coop_lfo2_focused(pos)) return v;
    int type, on; int16_t p[8];
    int kslot = (pos ? DRUM_KIT : SYNTH_INST) & 7;
    if (!coop_sound_lfo2(pos, kslot, &type, &on, p) || type > 2) return v;
    uint32_t *b = vec_begin(v), *e = *(uint32_t **)((uint8_t *)v + 8);
    if (!b) return v;
    int n = (int)(e - b); if (n > 8) n = 8;
    const int16_t *rg = coop_lfo2_ranges(type);
    for (int k = 0; k < n; k++) {
        int lo = rg[k * 3], hi = rg[k * 3 + 1];
        if (hi <= lo) { b[k] = 0; continue; }
        b[k] = FDIV(FSUB(I2F(p[k]), I2F(lo)), FSUB(I2F(hi), I2F(lo)));
    }
    g_lfo2view_params++;
    return v;
}
/* ---- u8: the stock's big "FX OFF" / "LFO OFF" overlays. Each is drawn from a predicate the screen framework calls
 * for the shown page (FX 0x019AAAEE, LFO 0x019C9782; neither is CALLed directly, they sit behind pointers): the page's
 * position word picks the on-flag getter of the stock state block at 0x1AAAC00 (FX: master byte 0x1AAAC40,
 * 0x1AAAC30 + kit, 0x1AAAC20 + slot; LFO: 0x1AAAC38 + kit, 0x1AAAC28 + slot; the T3 / T4 toggles write the same
 * bytes, 0x01978188 and 0x019781E0), and "off" is the getter returning 0. Those five getter CALLs are retargeted
 * here: in FX 2 / LFO 2 focus the overlay follows slot 2's own state. Gfx thread. ---- */
#define FX_PAGE_POS   (*(volatile int *)0x01DD7F7Cu)                        /* the effect page's position: 0 synth, 1 drum, 2 master */
#define LFO_PAGE_POS  (*(volatile int *)0x01DD7F80u)                        /* the LFO page's: 0 synth, 1 drum */
#define STOCK_FX_ON_M ((int (*)(void))0x01967240u)
#define STOCK_FX_ON_D ((int (*)(int))0x0196725Cu)
#define STOCK_FX_ON_S ((int (*)(int))0x0196727Cu)
#define STOCK_LFO_ON_D ((int (*)(int))0x0196724Cu)
#define STOCK_LFO_ON_S ((int (*)(int))0x0196726Cu)
volatile uint32_t g_fx2view_on, g_lfo2view_on;                              /* stats / tests: overlay reads answered for slot 2 */
extern "C" int coop_fx_on_read(int i)
{
    int p = FX_PAGE_POS;
    int f = p >= 0 && p <= 2 ? focused_stock(p == 2 ? 3 : p) : 0;
    if (f) { g_fx2view_on++; return !coop_slot2_bypassed(f - 1); }
    return p == 2 ? STOCK_FX_ON_M() : p == 1 ? STOCK_FX_ON_D(i) : STOCK_FX_ON_S(i);
}
extern "C" int coop_lfo_on_read(int i)
{
    int p = LFO_PAGE_POS;
    if (p >= 0 && p <= 1 && coop_lfo2_focused(p)) {
        int on, t = coop_lfo2_kind_type(&on);
        if (t >= 0 && t <= 2) { g_lfo2view_on++; return on ? 1 : 0; }
    }
    return p == 1 ? STOCK_LFO_ON_D(i) : STOCK_LFO_ON_S(i);
}

/* KeyHandler thread: show LFO 2's page (its type) or LFO 1's again, for the instrument (0 synth, 1 drum) */
extern "C" void coop_lfo2_post_screen(int pos, int lfo2)
{
    int slot = (pos ? DRUM_KIT : SYNTH_INST) & 7, type;
    if (lfo2) { int on; type = coop_lfo2_kind_type(&on); if (type < 0 || type > 2) return; }
    else type = pos ? LFO_TYPE_D(slot) : LFO_TYPE_S(slot);
    if (type < 0 || type > 6) return;
    POST_SCREEN(24 + type, slot, pos);
}

/* KeyHandler thread: show FX 2's page (a stock type) or FX 1's again, for the position shown (0 synth, 1 drum, 3 master) */
extern "C" void coop_fx2_post_screen(int pos, int fx2)
{
    int slot = pos == 1 ? DRUM_KIT : SYNTH_INST;
    int type;
    if (fx2) { type = coop_slot2_req_type(pos == 3 ? 1 : 0); if (type < 0) return; }
    else type = pos == 3 ? FX_TYPE_M : (pos == 1 ? FX_TYPE_D(slot) : FX_TYPE_S(slot));
    if (type < 0 || type > 7) return;
    g_fx2view_last_post = (uint32_t)FOR_FX(type);
    POST_SCREEN(FOR_FX(type), slot, pos);
}
