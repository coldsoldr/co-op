/*
 * coop-fw e4: the declared controls of plugin synth engines (docs/engines.md "Controls", guide/plugin-guidelines.md
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
#define POPUP_CYCLES 480000000u                                            /* ~1.2 s at 400 MHz */

extern "C" const coop_engine_t *coop_engine_at(int n);                    /* engine_host.cc */
extern "C" int coop_fn_held(void);                                        /* coop_tape_ui.cc */
extern "C" void coop_fn_use(void);

enum { A_TURN, A_CLICK, A_SHIFT_TURN, A_SHIFT_CLICK, A_FN_TURN, A_FN_CLICK, A_SHIFT_FN_TURN, A_SHIFT_FN_CLICK };
struct ctl {
    uint8_t action, enc, word, kind, count, random, detents, wrap;     /* e5: wrap (was pad) */
    int16_t def;
    char name[NAME_LEN];
    char steps[STEP_MAX][NAME_LEN];
};
struct ctls { int n; struct ctl c[CTL_MAX]; };
static struct ctls *g_ctl[SLOTS];
volatile uint32_t g_ctl_turns, g_ctl_clicks, g_ctl_ignored, g_ctl_cancelled, g_ctl_engines, g_ctl_popup_frames;

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

/* 0 = ok (or nothing declared), else why the file is refused */
extern "C" const char *coop_engine_set_controls(int n, const char *text)
{
    if (!text || !*text) return 0;
    if (n <= 0 || n >= SLOTS) return "bad engine number";
    struct ctls *t = (struct ctls *)MALLOC(sizeof(struct ctls));
    if (!t) return "plugin memory full";
    volatile uint8_t *z = (volatile uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(struct ctls); i++) z[i] = 0;
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
        if (w < 5 || w > 7) return "controls: word must be 5, 6 or 7";
        if (kind != 's' && kind != 'c') return "controls: kind must be steps or continuous";
        if (kind == 'c' && (a == A_CLICK || a == A_SHIFT_CLICK || a == A_FN_CLICK || a == A_SHIFT_FN_CLICK)) return "controls: a click needs a steps control";
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
        } else {
            cnt = 0;
            if (d < 0 || d > 32767) return "controls: default outside 0..32767";
        }
        c->count = (uint8_t)cnt; c->def = (int16_t)d;
        for (int k = 0; k < t->n; k++) {
            if (t->c[k].word == c->word) return "controls: two controls share a word";
            if (t->c[k].action == c->action && t->c[k].enc == c->enc) return "controls: two controls on one action";
        }
        t->n++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    __asm__ __volatile__("ssync;" ::: "memory");
    g_ctl[n] = t;
    g_ctl_engines++;
    return 0;
}

/* the default of the control in `word` (5..7), else 0: for the engine's synth_types row (plugin_host.cc) */
extern "C" int coop_engine_control_default(int n, int word)
{
    const struct ctls *t = (n > 0 && n < SLOTS) ? g_ctl[n] : 0;
    if (t) for (int k = 0; k < t->n; k++) if (t->c[k].word == word) return t->c[k].def;
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
static void touched(int n, const struct ctl *c)
{
    s_pop_k = (int)(c - g_ctl[n]->c);
    s_pop_n = n;
    s_pop_at = now() | 1u;
}

/* ---- turns (coop_ui_knob, KeyHandler thread): 1 = consumed ---- */
static int s_acc[4];
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
    int cur = ENG_KNOBS(inst)[c->word];
    if (c->kind == 's') {
        s_acc[enc] += steps;
        int moved = 0;
        while (s_acc[enc] >= c->detents) { s_acc[enc] -= c->detents; moved++; }
        while (s_acc[enc] <= -c->detents) { s_acc[enc] += c->detents; moved--; }
        if (!moved) return 1;
        int v;
        if (c->wrap) {                                                     /* e5: past the last step to the first */
            v = (clamp_step(c, cur) + moved) % c->count;
            if (v < 0) v += c->count;
        } else {
            v = clamp_step(c, clamp_step(c, cur) + moved);
        }
        set_word(inst, c->word, v);
    } else {
        int32_t v = cur + steps * 328;                                     /* 1 % a detent */
        set_word(inst, c->word, v < 0 ? 0 : (v > 32767 ? 32767 : v));
    }
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
extern "C" void coop_engine_ui_draw(void *r)
{
    uint32_t at = s_pop_at;
    if (!at) return;
    if (now() - at > POPUP_CYCLES) { if (s_pop_at == at) s_pop_at = 0; return; }   /* expired: cleared, so CYCLES wrapping
                                                                             (32 bits, ~10.7 s) can't show it again */
    int n = shown_engine();
    if (!n || n != s_pop_n || !g_ctl[n]) return;
    const struct ctl *c = &g_ctl[n]->c[s_pop_k];
    char s[24]; int o = 0;
    for (const char *q = c->name; *q && o < 10; q++) s[o++] = *q;
    s[o++] = ' '; s[o++] = ' ';
    int v = ENG_KNOBS(SYNTH_INST & 7)[c->word];
    if (c->kind == 's') { for (const char *q = c->steps[clamp_step(c, v)]; *q && o < 22; q++) s[o++] = *q; }
    else { int pc = (v < 0 ? 0 : v) * 100 / 32767; if (pc >= 100) s[o++] = '1'; if (pc >= 10) s[o++] = (char)('0' + (pc / 10) % 10); s[o++] = (char)('0' + pc % 10); }
    s[o] = 0;
    g_ctl_popup_frames++;
    pt a = { 8 << 10, 12 << 10 }, b = { 200 << 10, 12 << 10 };            /* a black band under the text */
    RASTER_LINE(r, &a, &b, 18 << 10, c_bg);
    coop_text_draw(&s_tx[c->enc], r, 12, 6, 12, c_enc[c->enc], s, 0);
}
