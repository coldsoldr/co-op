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
#define F32767      0x46FFFE00u                                           /* 32767.0f */
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

volatile uint32_t g_fx2view_params, g_fx2view_disp, g_fx2view_pops;      /* stats / tests */

static int focused_stock(int pos)                                          /* the stock position 0 / 1 / 3 */
{
    int p2 = pos == 3 ? 1 : 0;
    return coop_fx2_focused(p2) && coop_slot2_req_type(p2) >= 0 ? p2 + 1 : 0;   /* 1 + pos2, or 0 */
}
static uint32_t *vec_begin(void *v) { return *(uint32_t **)((uint8_t *)v + 4); }

extern "C" void *coop_fx_param_read(int slot, int pos)
{
    void *v = STOCK_PARAM(slot, pos);
    int f = focused_stock(pos);
    if (!f || !v) return v;
    uint32_t *b = vec_begin(v);
    if (!b) return v;
    for (int k = 0; k < 4; k++) b[k] = FDIV(I2F(coop_slot2_knob_q15(f - 1, k)), F32767);
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
    return r;
}

/* KeyHandler thread: show FX 2's page (a stock type) or FX 1's again, for the position shown (0 synth, 1 drum, 3 master) */
extern "C" void coop_fx2_post_screen(int pos, int fx2)
{
    int slot = pos == 1 ? DRUM_KIT : SYNTH_INST;
    int type;
    if (fx2) { type = coop_slot2_req_type(pos == 3 ? 1 : 0); if (type < 0) return; }
    else type = pos == 3 ? FX_TYPE_M : (pos == 1 ? FX_TYPE_D(slot) : FX_TYPE_S(slot));
    if (type < 0 || type > 7) return;
    POST_SCREEN(FOR_FX(type), slot, pos);
}
