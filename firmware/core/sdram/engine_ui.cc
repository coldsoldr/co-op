/*
 * coop-fw e4 / e6: the declared controls of plugin synth engines and (e6, below) plugin effects (docs/engines.md
 * "Controls", docs/plugins.md "Effect controls"). The e4 notes, for engines (guide/plugin-guidelines.md
 * §4). On a plugin engine's page (synth mode, page T1, screen 0x14) the core dispatches the layers the stock leaves
 * free: click, shift + click, fn + turn, fn + click, shift + fn + turn, shift + fn + click (fn = Help held,
 * coop_tape_ui.cc). Turn and shift + turn stay the stock's (knob words 0..3, fine adjustment on shift: iter's range
 * table gives words 4..7 scale 0) [C op1emu e4 probe: ENG_RANGE(14, k) = {0, 32767, 10485, 0} for k 0..3,
 * {0, 32767, 0, 0} for k 4..7].
 *
 * A control lives in one of the slot's free knob words 5, 6, 7 (word 4 is the core's selector). The words are saved
 * with the sound (op1.db slot knobs carry all eight), reach the engine as params[5..7], and pass the stock boot check
 * (iter's words 4..7 accept 0..32767). `steps` store the step index, `continuous` Q15. A word is written as the
 * randomiser writes knob words (coop_random.cc): the live words, then Patch::SetKnobs, so it saves and reloads like
 * any knob.
 *
 * The declaration comes from the plugin file (extension tag 6, packed from plugin.json "controls" by
 * tools/plugin/pack.py): one line per control, tab-separated:
 *   action  encoder  word  kind  count  default  random  detents  name  step,step,...
 * action 1 click, 3 shift + click, 4 fn + turn, 5 fn + click, 6 shift + fn + turn, 7 shift + fn + click (0 turn and
 * 2 shift + turn are the stock's: refused); encoder 0..3 blue green white orange; word 5..7; kind s (steps) or c
 * (continuous); count 2..16 (steps); default; random 0/1; detents a step on a turn (1..8); name and step names up to 8
 * characters, lower case (the stock glyphs). A malformed declaration refuses the file: the inventory says why.
 * e5: a steps line may end in one more field, wrap 0/1 (plugin.json "wrap": true): a turn past the last step goes to
 * the first and back. A core before e5 skips the rest of the line, so the same file clamps there.
 * e6: that 11th field is a flags word: 1 wrap (steps), 2 fine (a continuous fn + turn: shift + fn + turn on its encoder
 * moves it by 1/16 of the step). Kind l is a latch (effects only): its word field is the bit of params[8], a click
 * flips it, it is never saved.
 *
 * Rules (guidelines §4): a click acts on release, and only if shift and fn are as they were on the press, so a click
 * that becomes part of a combo never fires the plain one; an undeclared layer on this page does nothing and is
 * consumed; a turn or click on a steps control moves one step (a click wraps); every change shows the control's name
 * and value for a moment in the encoder's colour (the core draws it, for every engine, with or without a screen).
 *
 * Threads: parse on the main thread at load; keys and turns on the KeyHandler thread; the popup on the Gfx thread.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_text.h"

#define CUR_MODE     (*(volatile int *)0xFF907D58u)
#define CUR_SCREEN   (*(volatile int *)0x018B65C8u)
#define SYNTH_PAGE   (*(volatile int *)0x01B4EE88u)                        /* 0 engine, 1 envelope, 2 fx, 3 lfo */
#define SYNTH_INST   (*(volatile int *)0xFF8016D4u)
#define SHIFT_HELD   (*(volatile uint8_t *)0x01DD7F78u)
#define ENG_KNOBS(i) ((volatile int16_t *)(0xFF8013C4u + (uint32_t)(i) * 16))
#define PATCH_S(i)   ((void *)(0x01DDD7E8u + (uint32_t)(i) * 0x38))
#define SET_KNOBS    ((void (*)(void *, const int16_t *))0x01A00D14u)      /* Patch::SetKnobs (coop_random.cc) */
#define MALLOC       ((void *(*)(uint32_t))0x01A9F348u)
#define ITER_SCREEN  0x14
#define KEY_ENC0     0x3B
#define MAGIC        0x5300
#define SLOTS        64
#define CTL_MAX      8
#define STEP_MAX     16
#define NAME_LEN     9
#define COARSE_Q15 328                                                     /* 1 % of 32767 a detent (guidelines §4.2) */
#define POPUP_CYCLES 480000000u                                            /* ~1.2 s at 400 MHz */

extern "C" const coop_engine_t *coop_engine_at(int n);                    /* engine_host.cc */
extern "C" int coop_fn_held(void);                                        /* coop_tape_ui.cc */
extern "C" void coop_fn_use(void);

enum { A_TURN, A_CLICK, A_SHIFT_TURN, A_SHIFT_CLICK, A_FN_TURN, A_FN_CLICK, A_SHIFT_FN_TURN, A_SHIFT_FN_CLICK };
struct ctl {
    uint8_t action, enc, word, kind, count, random, detents, wrap;     /* e5: wrap (was pad); e6: word = the bit for a latch */
    uint8_t fine, pad[3];                                              /* e6: a fn_turn continuous control's shift + fn + turn is its fine */
    uint16_t step, fine_step;                                          /* e6: Q15 a detent (0 = 328), fine Q15 a detent (0 = step / 16) */
    int16_t def;
    char name[NAME_LEN];
    char steps[STEP_MAX][NAME_LEN];
};
struct ctls { int n, fx; struct ctl c[CTL_MAX]; };                  /* e6: fx = an effect's (latches allowed) */
static struct ctls *g_ctl[SLOTS];
volatile uint32_t g_ctl_turns, g_ctl_clicks, g_ctl_ignored, g_ctl_cancelled, g_ctl_engines, g_ctl_popup_frames;
volatile uint32_t g_ctl_fx, g_fxc_turns, g_fxc_fine, g_fxc_clicks, g_fxc_latches, g_fxc_ignored, g_fxc_cancelled, g_fxc_popup_frames, g_fxc_random;   /* e6 */

/* ---- parse (main thread, at load) ---- */
static int num(const char *&p, int *out)
{
    int v = 0, neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = 1; if (v > 99999) return 0; }
    *out = neg ? -v : v;
    return any;
}
static int field_end(const char *&p) { if (*p != '\t') return 0; p++; return 1; }
static int word_into(const char *&p, char *out, char stop)                 /* up to 8 characters until stop / tab / newline */
{
    int i = 0;
    while (*p && *p != '\t' && *p != '\n' && *p != stop) {
        char c = *p++;
        if (i >= NAME_LEN - 1 || c < 0x20 || (c >= 'A' && c <= 'Z')) return 0;
        out[i++] = c;
    }
    out[i] = 0;
    return i > 0;
}

/* 0 = ok (or nothing declared), else why the file is refused. fx: an effect's declaration (e6: latches) */
static const char *set_controls(int n, const char *text, int fx)
{
    if (!text || !*text) return 0;
    if (n <= 0 || n >= SLOTS) return "bad engine number";
    struct ctls *t = (struct ctls *)MALLOC(sizeof(struct ctls));
    if (!t) return "plugin memory full";
    volatile uint8_t *z = (volatile uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(struct ctls); i++) z[i] = 0;
    t->fx = fx;
    const char *p = text;
    while (*p) {
        if (t->n >= CTL_MAX) return "more than 8 controls";
        struct ctl *c = &t->c[t->n];
        int a, e, w, cnt, d, r, det;
        if (!num(p, &a) || !field_end(p) || !num(p, &e) || !field_end(p) || !num(p, &w) || !field_end(p)) return "controls: bad line";
        char kind = *p++;
        if (!field_end(p) || !num(p, &cnt) || !field_end(p) || !num(p, &d) || !field_end(p) || !num(p, &r) || !field_end(p)
            || !num(p, &det) || !field_end(p) || !word_into(p, c->name, 0)) return "controls: bad line";
        if (a == A_TURN || a == A_SHIFT_TURN || a < 0 || a > 7) return "controls: turn and shift + turn are the stock's";
        if (e < 0 || e > 3) return "controls: encoder must be 0..3";
        int click = a == A_CLICK || a == A_SHIFT_CLICK || a == A_FN_CLICK || a == A_SHIFT_FN_CLICK;
        if (kind == 'l') {                                                 /* e6: a latch (effects): the word field is its bit */
            if (!fx) return "controls: a latch is for effects only";
            if (!click) return "controls: a latch needs a click";
            if (w < 0 || w > 15) return "controls: a latch's bit must be 0..15";
        } else if (w < 5 || w > 7) return "controls: word must be 5, 6 or 7";
        if (kind != 's' && kind != 'c' && kind != 'l') return "controls: kind must be steps, continuous or latch";
        if (kind == 'c' && click) return "controls: a click needs a steps control";
        if (det < 1 || det > 8) return "controls: detents must be 1..8";
        c->action = (uint8_t)a; c->enc = (uint8_t)e; c->word = (uint8_t)w; c->kind = (uint8_t)kind;
        c->random = r ? 1 : 0; c->detents = (uint8_t)det;
        if (kind == 's') {
            if (cnt < 2 || cnt > STEP_MAX) return "controls: steps need 2..16 positions";
            if (!field_end(p)) return "controls: step names missing";
            for (int i = 0; i < cnt; i++) {
                if (!word_into(p, c->steps[i], ',')) return "controls: a step name is empty or too long";
                if (i < cnt - 1) { if (*p != ',') return "controls: fewer step names than steps"; p++; }
            }
            if (d < 0 || d >= cnt) return "controls: default outside the steps";
            if (*p == '\t') {                                              /* e5: the wrap field */
                int wr;
                p++;
                if (!num(p, &wr) || wr < 0 || wr > 1) return "controls: wrap must be 0 or 1";
                c->wrap = (uint8_t)wr;
            }
        } else if (kind == 'c') {
            cnt = 0;
            if (d < 0 || d > 32767) return "controls: default outside 0..32767";
            if (p[0] == '\t' && p[1] == '\t') {                            /* e6: an empty steps field, then flags: 2 = fine */
                int fl;
                p += 2;
                if (!num(p, &fl) || (fl & ~2)) return "controls: a continuous control's flags must be 0 or 2 (fine)";
                if (fl && a != A_FN_TURN) return "controls: fine is for a fn_turn control";
                c->fine = fl ? 1 : 0;
                if (*p == '\t') {                                          /* e6: step, then fine_step (Q15 a detent; 0 = default) */
                    int st, fs = 0;
                    p++;
                    if (!num(p, &st) || st < 0 || st > 32767) return "controls: step must be 1..32767";
                    if (*p == '\t') { p++; if (!num(p, &fs) || fs < 0 || fs > 32767) return "controls: fine_step must be 1..32767"; }
                    if (fs && !c->fine) return "controls: fine_step needs fine";
                    if (fs > (st ? st : COARSE_Q15)) return "controls: fine_step must not exceed step";
                    c->step = (uint16_t)st; c->fine_step = (uint16_t)fs;
                }
            }
        } else {
            cnt = 0; d = 0;                                                /* a latch: off at every selection, never saved */
        }
        c->count = (uint8_t)cnt; c->def = (int16_t)d;
        for (int k = 0; k < t->n; k++) {
            const struct ctl *o = &t->c[k];
            if ((o->kind == 'l') == (c->kind == 'l') && o->word == c->word)
                return c->kind == 'l' ? "controls: two latches share a bit" : "controls: two controls share a word";
            if (o->action == c->action && o->enc == c->enc) return "controls: two controls on one action";
            if (o->enc == c->enc && ((o->fine && c->action == A_SHIFT_FN_TURN) || (c->fine && o->action == A_SHIFT_FN_TURN)))
                return "controls: shift + fn + turn is the fine of that encoder's fn + turn";
        }
        t->n++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    __asm__ __volatile__("ssync;" ::: "memory");
    g_ctl[n] = t;
    if (fx) g_ctl_fx++; else g_ctl_engines++;
    return 0;
}
extern "C" const char *coop_engine_set_controls(int n, const char *text) { return set_controls(n, text, 0); }
extern "C" const char *coop_fx_set_controls(int n, const char *text) { return set_controls(n, text, 1); }   /* e6 */

/* e6: what the render may see in word 5..7 of plugin n: the highest value (step count - 1, or 32767); -1 = no control */
extern "C" int coop_ctl_word_max(int n, int word)
{
    const struct ctls *t = (n > 0 && n < SLOTS) ? g_ctl[n] : 0;
    if (t) for (int k = 0; k < t->n; k++)
        if (t->c[k].kind != 'l' && t->c[k].word == word) return t->c[k].kind == 's' ? t->c[k].count - 1 : 32767;
    return -1;
}

/* the default of the control in `word` (5..7), else 0: for the engine's synth_types row (plugin_host.cc) */
extern "C" int coop_engine_control_default(int n, int word)
{
    const struct ctls *t = (n > 0 && n < SLOTS) ? g_ctl[n] : 0;
    if (t) for (int k = 0; k < t->n; k++) if (t->c[k].kind != 'l' && t->c[k].word == word) return t->c[k].def;
    return 0;
}

/* ---- where we are ---- */
static int shown_engine(void)                                              /* the plugin engine on the shown page, else 0 */
{
    if (CUR_MODE != 0 || SYNTH_PAGE != 0 || CUR_SCREEN != ITER_SCREEN) return 0;
    int inst = SYNTH_INST;
    if (inst < 0 || inst > 7) return 0;
    int n = ENG_KNOBS(inst)[4] - MAGIC;
    return (n > 0 && n < SLOTS && coop_engine_at(n)) ? n : 0;
}
static struct ctl *find(int n, int action, int enc)
{
    struct ctls *t = g_ctl[n];
    if (t) for (int k = 0; k < t->n; k++) if (t->c[k].action == action && t->c[k].enc == enc) return &t->c[k];
    if (t && action == A_SHIFT_FN_TURN)                                     /* e6: the fine of a "fine" fn + turn */
        for (int k = 0; k < t->n; k++) if (t->c[k].action == A_FN_TURN && t->c[k].enc == enc && t->c[k].fine) return &t->c[k];
    return 0;
}
static int clamp_step(const struct ctl *c, int v) { return v < 0 ? 0 : (v >= c->count ? c->count - 1 : v); }

/* ---- the popup (written on the KeyHandler thread, read on the Gfx thread: a sequence word, then the fields) ---- */
static volatile int s_pop_n, s_pop_k;
static volatile uint32_t s_pop_at;
static inline uint32_t now(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

static void set_word(int inst, int word, int value)
{
    volatile int16_t *w = ENG_KNOBS(inst);
    int16_t v[8];
    for (int k = 0; k < 8; k++) v[k] = w[k];
    v[word] = (int16_t)value;
    w[word] = (int16_t)value;
    SET_KNOBS(PATCH_S(inst), v);
}
static volatile int s_pop_pos = -1;                                        /* e6: -1 an engine's popup, else the effect position */
static void touched_at(int n, const struct ctl *c, int pos)
{
    s_pop_k = (int)(c - g_ctl[n]->c);
    s_pop_n = n;
    s_pop_pos = pos;
    s_pop_at = now() | 1u;
}
static void touched(int n, const struct ctl *c) { touched_at(n, c, -1); }

/* ---- turns (coop_ui_knob, KeyHandler thread): 1 = consumed ---- */
#define FINE_DIV   16                                                      /* e6: the fine step, 1/16 of that (stock's shift ratio):
                                                                              328/16 = 20.5 a detent, as 20 and 21 (the remainder is kept) */
static int s_acc[4], s_fine[4];
/* the value a turn of `steps` detents makes of `cur`, or -1 when it doesn't move (a steps control between detents).
 * fine: the shift + fn + turn of a "fine" fn + turn control */
static int turn_value(const struct ctl *c, int enc, int cur, int steps, int fine)
{
    if (c->kind == 's') {
        s_acc[enc] += steps;
        int moved = 0;
        while (s_acc[enc] >= c->detents) { s_acc[enc] -= c->detents; moved++; }
        while (s_acc[enc] <= -c->detents) { s_acc[enc] += c->detents; moved--; }
        if (!moved) return -1;
        int v;
        if (c->wrap) {                                                     /* e5: past the last step to the first */
            v = (clamp_step(c, cur) + moved) % c->count;
            if (v < 0) v += c->count;
        } else {
            v = clamp_step(c, clamp_step(c, cur) + moved);
        }
        return v;
    }
    if (cur < 0) cur = 0; else if (cur > 32767) cur = 32767;
    int32_t d, step = c->step ? c->step : COARSE_Q15;                      /* e6: the control's step, else 1 % */
    if (fine && c->fine_step) d = steps * c->fine_step;                    /* e6: a declared fine step */
    else if (fine) {                                                       /* e6: 1/16 of the coarse step, remainder kept */
        s_fine[enc] += steps * step;
        d = s_fine[enc] / FINE_DIV;
        s_fine[enc] -= d * FINE_DIV;
    } else d = steps * step;
    int32_t v = cur + d;
    return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
extern "C" int coop_engine_ui_knob(int knob, int steps)
{
    if (!coop_fn_held()) return 0;                                         /* turn, shift + turn: the stock's */
    int n = shown_engine();
    if (!n) return 0;
    coop_fn_use();                                                         /* fn was used: no help on the release */
    int enc = knob & 3;
    struct ctl *c = find(n, SHIFT_HELD ? A_SHIFT_FN_TURN : A_FN_TURN, enc);
    if (!c) { g_ctl_ignored++; return 1; }
    int inst = SYNTH_INST;
    int v = turn_value(c, enc, ENG_KNOBS(inst)[c->word], steps, SHIFT_HELD && c->action == A_FN_TURN);
    if (v < 0) return 1;
    set_word(inst, c->word, v);
    touched(n, c);
    g_ctl_turns++;
    return 1;
}

/* ---- clicks (coop_ui_key, KeyHandler thread): 1 = consumed ---- */
static uint8_t s_press[4];                                                 /* 0 none, else 1 + (shift | fn << 1) at the press */
extern "C" int coop_engine_ui_key(int code, int pressed)
{
    if (code < KEY_ENC0 || code > KEY_ENC0 + 3) return 0;
    int enc = code - KEY_ENC0;
    int layer = (SHIFT_HELD ? 1 : 0) | (coop_fn_held() ? 2 : 0);
    if (pressed) {
        if (!shown_engine()) return 0;
        s_press[enc] = (uint8_t)(1 + layer);
        if (layer & 2) coop_fn_use();
        return 1;
    }
    if (!s_press[enc]) return 0;
    int was = s_press[enc] - 1;
    s_press[enc] = 0;
    int n = shown_engine();
    if (!n) return 1;
    if (was != layer) { g_ctl_cancelled++; return 1; }                     /* a modifier changed on the way: part of a combo */
    static const uint8_t action[4] = { A_CLICK, A_SHIFT_CLICK, A_FN_CLICK, A_SHIFT_FN_CLICK };
    struct ctl *c = find(n, action[layer], enc);
    if (!c) { g_ctl_ignored++; return 1; }
    int inst = SYNTH_INST;
    int v = clamp_step(c, ENG_KNOBS(inst)[c->word]) + 1;
    if (v >= c->count) v = 0;                                              /* a click wraps */
    set_word(inst, c->word, v);
    touched(n, c);
    g_ctl_clicks++;
    return 1;
}

/* ---- randomise (coop_random.cc, synth T1 of a plugin engine): the declared random controls into v[5..7] ---- */
extern "C" int coop_engine_ui_randomise(int inst, int16_t *v, uint32_t (*rnd)(void))
{
    int n = ENG_KNOBS(inst)[4] - MAGIC;
    if (n <= 0 || n >= SLOTS || !coop_engine_at(n) || !g_ctl[n]) return 0;
    int changed = 0;
    for (int k = 0; k < g_ctl[n]->n; k++) {
        const struct ctl *c = &g_ctl[n]->c[k];
        if (!c->random) continue;
        v[c->word] = (int16_t)(c->kind == 's' ? (int)(rnd() % c->count) : (int)(rnd() % 32768u));
        changed = 1;
    }
    return changed;
}

/* ---- the screen exports (sdk/coop_screen.h) ---- */
extern "C" void coop_scr_words(int16_t *out8)                              /* all eight words of the shown synth slot */
{
    int inst = SYNTH_INST & 7;
    const volatile int16_t *k = ENG_KNOBS(inst);
    for (int i = 0; i < 8; i++) out8[i] = k[i];
}
extern "C" int coop_scr_mods(void) { return (SHIFT_HELD ? 1 : 0) | (coop_fn_held() ? 2 : 0); }   /* bit 0 shift, bit 1 fn */

/* ---- the popup (coop_ui.cc coop_ui_frame, Gfx thread) ---- */
static const uint8_t c_enc[4][4] = { { 0x69, 0x8E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 }, { 0xFF, 0x3A, 0x5D, 255 } };
static const uint8_t c_bg[4] = { 0, 0, 0, 255 };
static struct coop_text s_tx[4];
typedef struct { int32_t x, y; } pt;
#define RASTER_LINE ((void (*)(void *, const pt *, const pt *, int32_t, const uint8_t *))0x01A24A4Cu)
static int popup_live(void)                                                /* the popup is up (not expired) */
{
    uint32_t at = s_pop_at;
    if (!at) return 0;
    if (now() - at > POPUP_CYCLES) { if (s_pop_at == at) s_pop_at = 0; return 0; }   /* expired: cleared, so CYCLES wrapping
                                                                             (32 bits, ~10.7 s) can't show it again */
    return 1;
}
static void popup_draw(void *r, const struct ctl *c, int v)                /* "name  value" top left in the encoder's colour */
{
    char s[24]; int o = 0;
    for (const char *q = c->name; *q && o < 10; q++) s[o++] = *q;
    s[o++] = ' '; s[o++] = ' ';
    if (c->kind == 's') { for (const char *q = c->steps[clamp_step(c, v)]; *q && o < 22; q++) s[o++] = *q; }
    else if (c->kind == 'l') { const char *q = v ? "on" : "off"; while (*q) s[o++] = *q++; }   /* e6: a latch */
    else { int pc = (v < 0 ? 0 : v) * 100 / 32767; if (pc >= 100) s[o++] = '1'; if (pc >= 10) s[o++] = (char)('0' + (pc / 10) % 10); s[o++] = (char)('0' + pc % 10); }
    s[o] = 0;
    pt a = { 8 << 10, 12 << 10 }, b = { 200 << 10, 12 << 10 };            /* a black band under the text */
    RASTER_LINE(r, &a, &b, 18 << 10, c_bg);
    coop_text_draw(&s_tx[c->enc], r, 12, 6, 12, c_enc[c->enc], s, 0);
}
extern "C" void coop_engine_ui_draw(void *r)
{
    if (!popup_live() || s_pop_pos != -1) return;
    int n = shown_engine();
    if (!n || n != s_pop_n || !g_ctl[n]) return;
    const struct ctl *c = &g_ctl[n]->c[s_pop_k];
    g_ctl_popup_frames++;
    popup_draw(r, c, ENG_KNOBS(SYNTH_INST & 7)[c->word]);
}

/* ==== e6: the declared controls of effect plugins (docs/plugins.md "Effect controls", guidelines §4) ====
 * On an effect page (screens 8..15; FX_POS 0 synth, 1 drum, 3 master) the core dispatches the same six layers to the
 * plugin effect in focus, at any of the five positions:
 *   FX 1 (lift): 0 synth, 1 drum, 2 master: the plugin is the position's FX 1 when its type is the filter (5) and word 4
 *       names it; words 5..7 are the stock FX words of the shown sound (synth 0xFF801444 + 16 * inst, drum 0xFF8015C4 +
 *       16 * kit: Patch::SetFxParams 0x019FF664 after the live words, saved with the sound as fx_params[5..7]; master
 *       0xFF8016C4: the live words, then op1.db's master row through the stock's own writer, plugin_host.cc
 *       master_save_service). The filter
 *       type's words 4..7 accept 0..32767 [C op1emu e6 probe: {0, 32767, 0} each], so nothing here can fail the boot check.
 *   FX 2 (drop): 3 the instrument's, 4 the master's: the plugin chosen in slot 2; words 5..7 are coop's (plugin_host.cc
 *       g_s2 ext, saved in /yaffs2/user/coop_slot2.bin after the v3 record).
 * A plugin with no declared controls is left alone (fn + turn stays the stock turn there, as before e6). A latch is a bit
 * of the position's latch word (plugin_host.cc), cleared whenever the plugin is (re)initialised at that position, never
 * saved. The core shows the control's name and value over the page as on an engine's. */
#define FX_POS_B     (*(volatile uint8_t *)0x01DF4E3Cu)                    /* 0 synth, 1 drum, 3 master (coop_ui.cc) */
#define DRUM_KIT     (*(volatile int *)0xFF8016D8u)
#define FX_TYPE_S(i) (*(volatile int *)(0xFF801734u + (uint32_t)(i) * 4))  /* coop_ui.cc */
#define FX_TYPE_D(i) (*(volatile int *)(0xFF801754u + (uint32_t)(i) * 4))
#define FX_TYPE_M    (*(volatile int *)0xFF801090u)
#define FX_W_S(i)    ((volatile int16_t *)(0xFF801444u + (uint32_t)(i) * 16))
#define FX_W_D(i)    ((volatile int16_t *)(0xFF8015C4u + (uint32_t)(i) * 16))
#define FX_W_M       ((volatile int16_t *)0xFF8016C4u)
#define PATCH_D(i)   ((void *)(0x01DDD9A8u + (uint32_t)(i) * 0x38))           /* coop_random.cc */
#define SET_FX       ((void (*)(void *, const int16_t *))0x019FF664u)       /* Patch::SetFxParams (coop_random.cc) */
#define FILTER_TYPE  5
extern "C" int coop_fx2_focused(int pos2);                                 /* coop_ui.cc */
extern "C" int coop_fx2_pick_pending(void);
extern "C" void coop_ui_slot2_touched(void);
extern "C" int coop_slot2_plugin(int pos2);                                /* plugin_host.cc (e6) */
extern "C" int coop_slot2_ext(int pos2, int word);
extern "C" void coop_slot2_ext_set(int pos2, int word, int v);
extern "C" int coop_fx_is_plugin(int n);
extern "C" int coop_fx_latch(int pos);
extern "C" void coop_fx_latch_flip(int pos, int bit);
extern "C" void coop_master_save_request(void);

struct fxt { int n, pos, inst; volatile int16_t *w; };                     /* the plugin in focus: number, position, sound, FX 1 words */
static int fx_target(struct fxt *t)
{
    int s = CUR_SCREEN;
    if (s < 8 || s > 15) return 0;
    int fp = FX_POS_B, pos2 = fp == 3 ? 1 : 0;
    t->w = 0; t->inst = 0;
    if (coop_fx2_focused(pos2)) { t->pos = 3 + pos2; t->n = coop_slot2_plugin(pos2); }
    else {
        int type;
        if (fp == 3) { t->pos = 2; t->w = FX_W_M; type = FX_TYPE_M; }
        else if (fp == 1) { t->pos = 1; t->inst = DRUM_KIT & 7; t->w = FX_W_D(t->inst); type = FX_TYPE_D(t->inst); }
        else { t->pos = 0; t->inst = SYNTH_INST & 7; t->w = FX_W_S(t->inst); type = FX_TYPE_S(t->inst); }
        t->n = type == FILTER_TYPE ? t->w[4] - MAGIC : 0;
    }
    if (t->n <= 0 || t->n >= SLOTS || !coop_fx_is_plugin(t->n) || !g_ctl[t->n] || !g_ctl[t->n]->fx) return 0;
    return 1;
}
static int fx_get(const struct fxt *t, const struct ctl *c)
{
    if (c->kind == 'l') return (coop_fx_latch(t->pos) >> c->word) & 1;
    return t->w ? t->w[c->word] : coop_slot2_ext(t->pos - 3, c->word);
}
static void fx_set(const struct fxt *t, int word, int value)
{
    if (!t->w) { coop_slot2_ext_set(t->pos - 3, word, value); coop_ui_slot2_touched(); return; }   /* FX 2: saved with the slot file */
    int16_t v[8];
    for (int k = 0; k < 8; k++) v[k] = t->w[k];
    v[word] = (int16_t)value;
    t->w[word] = (int16_t)value;
    if (t->pos == 0) SET_FX(PATCH_S(t->inst), v);                          /* the sound's fx_params, as the randomiser writes them */
    else if (t->pos == 1) SET_FX(PATCH_D(t->inst), v);
    else coop_master_save_request();                                       /* the master: op1.db's master row, from the main loop */
}

/* e6 (the owner, 2026-11-11): randomise (coop_random.cc) draws an effect's saved controls as an engine's, per control:
 * steps within their count, continuous over 0..32767, unless the control says "random": false; latches never. v[] is
 * indexed by word (v[5..7]). 1 = something was drawn. */
extern "C" int coop_fx_ui_randomise(int n, int16_t *v, uint32_t (*rnd)(void))
{
    if (n <= 0 || n >= SLOTS || !coop_fx_is_plugin(n) || !g_ctl[n] || !g_ctl[n]->fx) return 0;
    int changed = 0;
    for (int k = 0; k < g_ctl[n]->n; k++) {
        const struct ctl *c = &g_ctl[n]->c[k];
        if (c->kind == 'l' || !c->random) continue;
        v[c->word] = (int16_t)(c->kind == 's' ? (int)(rnd() % c->count) : (int)(rnd() % 32768u));
        changed = 1;
    }
    if (changed) g_fxc_random++;
    return changed;
}

extern "C" int coop_fx_ui_knob(int knob, int steps)
{
    if (!coop_fn_held()) return 0;                                         /* turn, shift + turn: the stock's (and FX 2's) */
    struct fxt t;
    if (!fx_target(&t)) return 0;
    coop_fn_use();
    int enc = knob & 3;
    int fine = SHIFT_HELD;
    struct ctl *c = find(t.n, fine ? A_SHIFT_FN_TURN : A_FN_TURN, enc);
    if (!c) { g_fxc_ignored++; return 1; }
    int v = turn_value(c, enc, fx_get(&t, c), steps, fine && c->action == A_FN_TURN);
    if (v < 0) return 1;
    fx_set(&t, c->word, v);
    touched_at(t.n, c, t.pos);
    if (fine && c->action == A_FN_TURN) g_fxc_fine++; else g_fxc_turns++;
    return 1;
}

static uint8_t s_fx_press[4];                                              /* as s_press, for the effect page */
extern "C" int coop_fx_ui_key(int code, int pressed)
{
    if (code < KEY_ENC0 || code > KEY_ENC0 + 3) return 0;
    int enc = code - KEY_ENC0;
    int layer = (SHIFT_HELD ? 1 : 0) | (coop_fn_held() ? 2 : 0);
    struct fxt t;
    if (pressed) {
        if (!fx_target(&t)) return 0;
        if (t.pos >= 3 && enc == 0 && layer == 0 && coop_fx2_pick_pending()) return 0;   /* FX 2: blue applies the picked effect */
        s_fx_press[enc] = (uint8_t)(1 + layer);
        if (layer & 2) coop_fn_use();
        return 1;
    }
    if (!s_fx_press[enc]) return 0;
    int was = s_fx_press[enc] - 1;
    s_fx_press[enc] = 0;
    if (!fx_target(&t)) return 1;
    if (was != layer) { g_fxc_cancelled++; return 1; }                     /* part of a combo: no plain click */
    static const uint8_t action[4] = { A_CLICK, A_SHIFT_CLICK, A_FN_CLICK, A_SHIFT_FN_CLICK };
    struct ctl *c = find(t.n, action[layer], enc);
    if (!c) { g_fxc_ignored++; return 1; }
    if (c->kind == 'l') { coop_fx_latch_flip(t.pos, c->word); g_fxc_latches++; }
    else {
        int v = clamp_step(c, fx_get(&t, c)) + 1;
        if (v >= c->count) v = 0;                                          /* a click wraps */
        fx_set(&t, c->word, v);
        g_fxc_clicks++;
    }
    touched_at(t.n, c, t.pos);
    return 1;
}

extern "C" void coop_fx_ui_draw(void *r)                                   /* coop_ui.cc, over the effect page (Gfx thread) */
{
    if (!popup_live() || s_pop_pos < 0) return;
    struct fxt t;
    if (!fx_target(&t) || t.pos != s_pop_pos || t.n != s_pop_n) return;
    const struct ctl *c = &g_ctl[t.n]->c[s_pop_k];
    g_fxc_popup_frames++;
    popup_draw(r, c, fx_get(&t, c));
}
