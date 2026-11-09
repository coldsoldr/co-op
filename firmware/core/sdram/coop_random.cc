/*
 * coop-fw u4: randomise the current page's parameters (docs/ui-features.md §10).
 *
 * Hold the synth (or drum) key and press the right arrow: the page shown gets new values drawn inside the stock
 * Ranges and written the way the stock encoder handler writes them, the live words then the patch JSON setter, so the
 * sound changes at once, saves as edited, and always passes the boot check (which tests each word against the same
 * tables: errors 15 / 19 / 21 / 24 / 27). The left arrow with the key held steps back through the last five
 * randomisations (the values before each); manual edits are not recorded. The arrows are swallowed while the key is
 * held (stock: octave down / up). The mode key itself is deferred by coop_key_event (lander_off.cc): stock resets
 * the page to T1 when the key is pressed on its own mode, so a tap is sent to the stock on the release and a hold
 * sends nothing.
 *
 * Pages (research 2026-11-07, OP1_vdk.dis; ../../te-op1-cfw/docs/screen-interface.md §4c, lfo-interface.md §7):
 *   synth T1  knobs    0xFF8013C4 + inst*16   ranges 0x01DE5890 + type*64 + k*8 {min,max,scale,mode}   Patch::SetKnobs     0x01A00D14
 *   synth T2  adsr     0x01B60EF0 + inst*16   ranges 0x01DE5710 + k*6 {min,max,scale}, words 0..3 (A D S R) Patch::SetAdsr      0x01A00C98
 *   synth T3  fx       0xFF801444 + inst*16   ranges 0x01DE5C50 + fxtype*48 + k*6, words 0..3 only      Patch::SetFxParams  0x019FF664
 *   synth T4  lfo      0xFF8014C4 + inst*16   ranges 0x01DE5740 + lfotype*48 + k*6                    Patch::SetLfoParams 0x019FF734
 *   drum  T2  dyna_env 0xFF801544 + kit*16    ranges 0x01DE56C8 + k*6, words 0..4                      0x01A0005C
 *   drum  T3  fx       0xFF8015C4 + kit*16    as the synth's (type 0xFF801754 + kit*4)                 0x019FF664
 *   drum  T4  lfo      0xFF801644 + kit*16    as the synth's (type 0xFF8013A4 + kit*4)                 0x019FF734
 *   drum  T1: the last played voice's sample record (pitch, start, end ...), not knobs: left alone.
 * The effect and LFO pages are left alone while the stock has them off (as the stock encoder handler does); FX words
 * 4..7 are never written (coop's plugins keep their selector in word 4; stock never writes them either); the
 * sampler engine (range mode 2, special) and the shifted knobs that are fine adjustments keep their values.
 *
 * KeyHandler thread, as the stock handler. Fixed point only, no libc. The draw is a 32-bit LCG seeded from CYCLES.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define CUR_MODE        (*(volatile int *)0xFF907D58u)                        /* 0 synth, 1 drum, 2 tape, 3 mixer */
#define SYNTH_PAGE      (*(volatile int *)0x01B4EE88u)                        /* 0 engine, 1 envelope, 2 fx, 3 lfo */
#define DRUM_PAGE       (*(volatile int *)0x01AAB9F4u)
#define SYNTH_INST      (*(volatile int *)0xFF8016D4u)                        /* the sound slot 0..7 */
#define DRUM_KIT        (*(volatile int *)0xFF8016D8u)
#define PATCH_S(i)      ((void *)(0x01DDD7E8u + (uint32_t)(i) * 0x38))
#define PATCH_D(i)      ((void *)(0x01DDD9A8u + (uint32_t)(i) * 0x38))
#define ENG_KNOBS(i)    ((volatile int16_t *)(0xFF8013C4u + (uint32_t)(i) * 16))
#define ENG_TYPE(i)     (*(volatile int *)(0xFF8016E4u + (uint32_t)(i) * 4))
#define ENG_RANGE(t, k) ((const volatile int16_t *)(0x01DE5890u + (uint32_t)(t) * 64 + (uint32_t)(k) * 8))
#define ADSR(i)         ((volatile int16_t *)(0x01B60EF0u + (uint32_t)(i) * 16))
#define ADSR_RANGE(k)   ((const volatile int16_t *)(0x01DE5710u + (uint32_t)(k) * 6))
#define DYNA(i)         ((volatile int16_t *)(0xFF801544u + (uint32_t)(i) * 16))
#define DYNA_RANGE(k)   ((const volatile int16_t *)(0x01DE56C8u + (uint32_t)(k) * 6))
#define FX_S(i)         ((volatile int16_t *)(0xFF801444u + (uint32_t)(i) * 16))
#define FX_D(i)         ((volatile int16_t *)(0xFF8015C4u + (uint32_t)(i) * 16))
#define FX_TYPE_S(i)    (*(volatile int *)(0xFF801734u + (uint32_t)(i) * 4))
#define FX_TYPE_D(i)    (*(volatile int *)(0xFF801754u + (uint32_t)(i) * 4))
#define FX_RANGE(t, k)  ((const volatile int16_t *)(0x01DE5C50u + (uint32_t)(t) * 48 + (uint32_t)(k) * 6))
#define FX_ACTIVE_S     ((int (*)(int))0x0196727Cu)
#define FX_ACTIVE_D     ((int (*)(int))0x0196725Cu)
#define LFO_S(i)        ((volatile int16_t *)(0xFF8014C4u + (uint32_t)(i) * 16))
#define LFO_D(i)        ((volatile int16_t *)(0xFF801644u + (uint32_t)(i) * 16))
#define LFO_TYPE_S(i)   (*(volatile int *)(0xFF801384u + (uint32_t)(i) * 4))
#define LFO_TYPE_D(i)   (*(volatile int *)(0xFF8013A4u + (uint32_t)(i) * 4))
#define LFO_RANGE(t, k) ((const volatile int16_t *)(0x01DE5740u + (uint32_t)(t) * 48 + (uint32_t)(k) * 6))
#define LFO_ACTIVE_S    ((int (*)(int))0x0196726Cu)
#define LFO_ACTIVE_D    ((int (*)(int))0x0196724Cu)
#define SET_KNOBS       ((void (*)(void *, const int16_t *))0x01A00D14u)
#define SET_ADSR        ((void (*)(void *, const int16_t *))0x01A00C98u)
#define SET_FX          ((void (*)(void *, const int16_t *))0x019FF664u)
#define SET_LFO         ((void (*)(void *, const int16_t *))0x019FF734u)
#define SET_DYNA        ((void (*)(void *, const int16_t *))0x01A0005Cu)
#define KEY_LEFT  0x24
#define KEY_RIGHT 0x25
#define HIST 5

extern "C" uint8_t coop_mode_keys_held(void);                                 /* lander_off.cc: bit 0 synth, 1 drum */
extern "C" void coop_mode_key_used(void);                                     /* lander_off.cc: the held key was a modifier */

volatile uint32_t g_rnd_count, g_rnd_backs, g_rnd_refused, g_rnd_slot2;      /* stats / tests; slot2: FX 2 / LFO 2 draws */
/* u10: the focused slot. On the effect page with FX 2 in focus (coop_ui.cc) the draw goes to slot 2's knobs (q15,
 * uniform over the type's turnable ranges, plugin_host.cc knob_map); on the LFO page with LFO 2 in focus to the
 * shown sound's LFO 2 parameters (lfo2.cc ranges, the stock's "scale 0 = not turnable" rule). Both refuse while the
 * slot is off, as the stock page does; a plugin in FX 2 is refused (no range table). The history keeps the slot. */
extern "C" int coop_fx2_focused(int pos2);                                     /* coop_ui.cc: 0 instrument, 1 master */
extern "C" int coop_lfo2_focused(int pos);                                     /* coop_ui.cc: 0 synth, 1 drum */
extern "C" int coop_slot2_req_type(int pos2);                                  /* plugin_host.cc: -1 none / plugin */
extern "C" int coop_slot2_bypassed(int pos2);
extern "C" void coop_slot2_get(int pos2, uint8_t *out10);
extern "C" int coop_slot2_set(int pos2, int kind, int id, const uint16_t *knob);
extern "C" void coop_ui_slot2_touched(void);                                   /* coop_ui.cc: reload the page's copy, save */
extern "C" int coop_lfo2_kind_type(int *on);                                   /* lfo2.cc: the shown sound's LFO 2 type, 7 none */
extern "C" void coop_lfo2_page_params(int16_t *p8);                            /* coop_ui.cc: its parameters */
extern "C" void coop_lfo2_set_sound(int type, int on, const int16_t *p);
extern "C" const int16_t *coop_lfo2_ranges(int type);                          /* {min, max, scale} x 8 */
enum { H_STOCK, H_FX2, H_LFO2 };
struct hist { int kind, mode, page, inst; int t, on; int16_t v[8]; };        /* H_FX2: v[0..3] q15 knobs of type t; H_LFO2: the 8 params of type t */
static struct hist s_hist[HIST];
static int s_hist_n;
static uint32_t s_seed;
static uint64_t s_swallow;

static uint32_t rnd(void)
{
    if (!s_seed) { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); s_seed = c | 1; }
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed >> 8;
}
static int16_t draw(int16_t lo, int16_t hi) { return (int16_t)(lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1))); }
static int filled(const volatile int16_t *rg) { return rg[1] > rg[0]; }     /* boot-built tables: zero = unknown */

/* the page's live words and the setter that follows them */
static volatile int16_t *words(int mode, int page, int inst)
{
    if (mode == 0) return page == 0 ? ENG_KNOBS(inst) : page == 1 ? ADSR(inst) : page == 2 ? FX_S(inst) : LFO_S(inst);
    return page == 1 ? DYNA(inst) : page == 2 ? FX_D(inst) : page == 3 ? LFO_D(inst) : 0;
}
static void commit(int mode, int page, int inst, const int16_t *v)
{
    volatile int16_t *w = words(mode, page, inst);
    if (!w) return;
    for (int k = 0; k < 8; k++) w[k] = v[k];
    void *patch = mode == 0 ? PATCH_S(inst) : PATCH_D(inst);
    if (mode == 0) { if (page == 0) SET_KNOBS(patch, v); else if (page == 1) SET_ADSR(patch, v); else if (page == 2) SET_FX(patch, v); else SET_LFO(patch, v); }
    else { if (page == 1) SET_DYNA(patch, v); else if (page == 2) SET_FX(patch, v); else if (page == 3) SET_LFO(patch, v); }
}
static struct hist *push_hist(int kind, int mode, int page, int inst, const volatile int16_t *v)
{
    if (s_hist_n == HIST) { for (int i = 1; i < HIST; i++) s_hist[i - 1] = s_hist[i]; s_hist_n--; }
    struct hist *h = &s_hist[s_hist_n++];
    h->kind = kind; h->mode = mode; h->page = page; h->inst = inst; h->t = 0; h->on = 0;
    for (int k = 0; k < 8; k++) h->v[k] = v[k];
    return h;
}
static void commit_fx2(int t, const int16_t *q)                               /* slot 2's four knobs, q15 */
{
    uint16_t k[4];
    for (int i = 0; i < 4; i++) k[i] = (uint16_t)q[i];
    if (coop_slot2_set(0, 1, t, k)) coop_ui_slot2_touched();
}
static int randomise_fx2(int mode)                                            /* FX 2 of the instrument shown */
{
    int t = coop_slot2_req_type(0);
    if (t < 0 || t > 7 || coop_slot2_bypassed(0)) return 0;                  /* a plugin, nothing, or off */
    uint8_t b[10]; coop_slot2_get(0, b);
    int16_t cur[8], v[8];
    for (int i = 0; i < 8; i++) cur[i] = 0;
    for (int i = 0; i < 4; i++) cur[i] = (int16_t)(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
    for (int i = 0; i < 8; i++) v[i] = cur[i];
    int changed = 0;
    for (int k = 0; k < 4; k++) { const volatile int16_t *rg = FX_RANGE(t, k); if (rg[2] == 0 || !filled(rg)) continue; v[k] = draw(0, 32767); changed = 1; }
    if (!changed) return 0;
    struct hist *h = push_hist(H_FX2, mode, 2, 0, cur); h->t = t;
    commit_fx2(t, v);
    g_rnd_count++; g_rnd_slot2++;
    return 1;
}
static int randomise_lfo2(int mode)                                           /* LFO 2 of the sound shown */
{
    int on, t = coop_lfo2_kind_type(&on);
    if (t < 0 || t > 2 || !on) return 0;                                      /* none, or off */
    int16_t cur[8], v[8];
    coop_lfo2_page_params(cur);
    for (int i = 0; i < 8; i++) v[i] = cur[i];
    const int16_t *rg = coop_lfo2_ranges(t);
    int changed = 0;
    for (int k = 0; k < 8; k++) { int lo = rg[3 * k], hi = rg[3 * k + 1]; if (rg[3 * k + 2] == 0 || hi <= lo) continue; v[k] = draw((int16_t)lo, (int16_t)hi); changed = 1; }
    if (!changed) return 0;
    struct hist *h = push_hist(H_LFO2, mode, 3, 0, cur); h->t = t; h->on = on;
    coop_lfo2_set_sound(t, on, v);
    g_rnd_count++; g_rnd_slot2++;
    return 1;
}

static int randomise(void)
{
    int mode = CUR_MODE;
    if (mode != 0 && mode != 1) return 0;
    int page = mode == 0 ? SYNTH_PAGE : DRUM_PAGE, inst = mode == 0 ? SYNTH_INST : DRUM_KIT;
    if (inst < 0 || inst > 7 || page < 0 || page > 3) return 0;
    if (page == 2 && coop_fx2_focused(0)) return randomise_fx2(mode);        /* u10: the focused slot */
    if (page == 3 && coop_lfo2_focused(mode)) return randomise_lfo2(mode);
    volatile int16_t *w = words(mode, page, inst);
    if (!w) return 0;                                                         /* drum T1 */
    int16_t v[8];
    for (int k = 0; k < 8; k++) v[k] = w[k];
    int changed = 0;
    if (mode == 0 && page == 0) {
        int t = ENG_TYPE(inst);
        if (t < 0 || t > 14 || t == 6 || t == 8 || t == 9) return 0;         /* sampler (special), the drum ids */
        for (int k = 0; k < 8; k++) {
            const volatile int16_t *rg = ENG_RANGE(t, k);
            if (rg[2] == 0 || !filled(rg) || rg[3] == 2) continue;            /* not turnable, unfilled, special */
            if (k >= 4 && rg[3] != 3) continue;                               /* a shifted knob that is fine adjustment */
            v[k] = draw(rg[0], rg[1]); changed = 1;
        }
    } else if (mode == 0 && page == 1) {
        for (int k = 0; k < 4; k++) { const volatile int16_t *rg = ADSR_RANGE(k); if (rg[2] == 0 || !filled(rg)) continue; v[k] = draw(rg[0], rg[1]); changed = 1; }
    } else if (mode == 1 && page == 1) {
        for (int k = 0; k < 5; k++) { const volatile int16_t *rg = DYNA_RANGE(k); if (rg[2] == 0 || !filled(rg)) continue; v[k] = draw(rg[0], rg[1]); changed = 1; }
    } else if (page == 2) {
        if (!(mode == 0 ? FX_ACTIVE_S(inst) : FX_ACTIVE_D(inst))) return 0;
        int t = mode == 0 ? FX_TYPE_S(inst) : FX_TYPE_D(inst);
        if (t < 0 || t > 7) return 0;
        for (int k = 0; k < 4; k++) { const volatile int16_t *rg = FX_RANGE(t, k); if (rg[2] == 0 || !filled(rg)) continue; v[k] = draw(rg[0], rg[1]); changed = 1; }
    } else {
        if (!(mode == 0 ? LFO_ACTIVE_S(inst) : LFO_ACTIVE_D(inst))) return 0;
        int t = mode == 0 ? LFO_TYPE_S(inst) : LFO_TYPE_D(inst);
        if (t < 0 || t > 6) return 0;
        for (int k = 0; k < 8; k++) { const volatile int16_t *rg = LFO_RANGE(t, k); if (rg[2] == 0 || !filled(rg)) continue; v[k] = draw(rg[0], rg[1]); changed = 1; }
    }
    if (!changed) return 0;
    push_hist(H_STOCK, mode, page, inst, w);
    commit(mode, page, inst, v);
    g_rnd_count++;
    return 1;
}
static int back(void)
{
    if (!s_hist_n) return 0;
    struct hist *h = &s_hist[--s_hist_n];
    if (h->kind == H_FX2) { if (coop_slot2_req_type(0) == h->t) commit_fx2(h->t, h->v); }   /* the slot still holds that type */
    else if (h->kind == H_LFO2) { int on, t = coop_lfo2_kind_type(&on); if (t == h->t) coop_lfo2_set_sound(t, on, h->v); }
    else commit(h->mode, h->page, h->inst, h->v);
    g_rnd_backs++;
    return 1;
}

/* from coop_ui_key (KeyHandler thread): 1 = consumed (both edges of a taken arrow) */
extern "C" int coop_random_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) { if (s_swallow & bit) { s_swallow &= ~bit; return 1; } return 0; }
    if (code != KEY_LEFT && code != KEY_RIGHT) return 0;
    int mode = CUR_MODE;
    uint8_t held = coop_mode_keys_held();
    if (!((mode == 0 && (held & 1)) || (mode == 1 && (held & 2)))) return 0;   /* the mode's own key held */
    coop_mode_key_used();
    if (code == KEY_RIGHT) { if (!randomise()) g_rnd_refused++; }
    else back();
    s_swallow |= bit;
    return 1;
}
