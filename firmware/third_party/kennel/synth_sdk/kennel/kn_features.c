#include "../op1_synth.h"
/*
 * Feature registry and the core's hook points for features (docs/feature-hooks.md): the note pipeline behind
 * VoiceCtl's note on / off calls, the music context and key sync; the clock pages are kn_pages.c's.
 * Built-in features register here at boot; plugin files of kind 3 through the loader (kn_boot.c). The order is fixed
 * once (kn_features_finish, end of the loader) before the first note, so no locking is needed afterwards.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "../op1_feature.h"

#define VC_NOTE_ON  ((void (*)(const op1_note *))0x01A84604)   /* voicectl_synth_note_on */
#define VC_NOTE_OFF ((void (*)(const op1_note *))0x01A85058)   /* voicectl_synth_note_off */
#define KN_FEATURES_MAX 48

/* built-in features: weak, so a feature shipped as a file instead (not compiled in) leaves 0 here */
extern const op1_feature_t FEATURE_samplerplus __attribute__((weak)), FEATURE_randomiser __attribute__((weak));
extern const op1_feature_t FEATURE_chord __attribute__((weak));
extern const op1_feature_t FEATURE_sysmon __attribute__((weak)), FEATURE_usbout __attribute__((weak)),
    FEATURE_settings __attribute__((weak)), FEATURE_oblique __attribute__((weak));   /* clock pages (kn_pages.c) */
extern const op1_feature_t FEATURE_quad __attribute__((weak));
extern const op1_feature_t FEATURE_markers __attribute__((weak)), FEATURE_punch __attribute__((weak)),
                           FEATURE_countin __attribute__((weak)), FEATURE_fasterase __attribute__((weak));
extern const op1_feature_t FEATURE_scale __attribute__((weak));
extern const op1_feature_t FEATURE_sampletools __attribute__((weak));
extern const op1_feature_t FEATURE_drumplus __attribute__((weak));
extern const op1_feature_t FEATURE_midiseq __attribute__((weak)), FEATURE_midicc __attribute__((weak));   /* op1r 35 MIDI */
extern const op1_feature_t FEATURE_euclid __attribute__((weak)), FEATURE_dots __attribute__((weak)),
    FEATURE_oracle __attribute__((weak)), FEATURE_peal __attribute__((weak));   /* sequencer types */
/* each hook point's own finish (its kn_<point>.c): reads the ordered registry with kn_feature_count/_at */
extern void kn_pages_finish(void) __attribute__((weak));
extern void kn_seq_finish(void) __attribute__((weak));
extern void kn_lfo_finish(void) __attribute__((weak));
int kn_feature_initializing;
extern void kn_drum_synth_finish(void) __attribute__((weak));
extern void kn_drum_finish(void) __attribute__((weak));
extern void kn_tape_finish(void) __attribute__((weak));

op1_music_t op1_music;
static const op1_feature_t *features[KN_FEATURES_MAX];          /* registered, in priority order once finished */
static int n_features;
static const op1_feature_t *stages[KN_FEATURES_MAX];            /* the note pipeline */
static int n_stages;

int kn_plugin_off(const char *name);                            /* kn_settings.c: the plugins page's off list */
void kn_log_line(const char *what, const char *msg);            /* kn_boot.c */
static uint8_t builtin[KN_FEATURES_MAX];                        /* registered from the firmware, not from a file */
static int registering_builtins;
static int builtin_count(void)
{
    const op1_feature_t *const list[] = { &FEATURE_chord, &FEATURE_sysmon, &FEATURE_usbout, &FEATURE_settings,
        &FEATURE_oblique, &FEATURE_quad, &FEATURE_markers, &FEATURE_punch, &FEATURE_countin, &FEATURE_fasterase,
        &FEATURE_scale, &FEATURE_sampletools, &FEATURE_drumplus, &FEATURE_midiseq, &FEATURE_midicc,
        &FEATURE_euclid, &FEATURE_dots, &FEATURE_oracle, &FEATURE_peal, &FEATURE_samplerplus, &FEATURE_randomiser };
    int n = 0;
    for (unsigned i = 0; i < sizeof list / sizeof *list; i++) if (list[i]) n++;
    return n;
}
static int same(const char *a, const char *b) { if (!a || !b) return 0; while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* inventory (kn_inv.c; weak: the host tests of the registry run without it) */
extern void kn_inv_feature_result(const op1_feature_t *f, int r, int builtin) __attribute__((weak));
extern void kn_inv_lost(const op1_feature_t *loser, const char *claim, const char *winner) __attribute__((weak));

static int do_register(const op1_feature_t *f)
{
    if (!f || f->api != OP1_FEATURE_API) return -1;
    if (f->name && kn_plugin_off(f->name)) return -2;           /* switched off by the user */
    for (int i = 0; i < n_features; i++)
        if (same(features[i]->name, f->name)) {                 /* a file named like a built-in replaces it */
            if (registering_builtins) return -3;
            return -4;                                        /* first file wins: user directory precedes content */
        }
    if (n_features >= KN_FEATURES_MAX - (registering_builtins ? 0 : builtin_count())) return -1;
    builtin[n_features] = (uint8_t)registering_builtins;
    features[n_features++] = f;
    return 0;
}

int kn_feature_register(const op1_feature_t *f)
{
    int r = do_register(f);
    if (kn_inv_feature_result) kn_inv_feature_result(f, r, registering_builtins);
    return r;
}

/* a beats b: built-in over file, then the lower priority, then the name */
static int beats(int a, int b)
{
    if (builtin[a] != builtin[b]) return builtin[a];
    if (features[a]->priority != features[b]->priority) return features[a]->priority < features[b]->priority;
    const char *x = features[a]->name ? features[a]->name : "", *y = features[b]->name ? features[b]->name : "";
    while (*x && *x == *y) { x++; y++; }
    return (unsigned char)*x <= (unsigned char)*y;
}

/* exclusive claims: keep one owner per claim, switch the losers off completely (before any hook point sees them) */
static void resolve_claims(void)
{
    static uint8_t off[KN_FEATURES_MAX];
    /* Resolve in the complete precedence order, so a rejected candidate owns nothing. */
    for (int i = 1; i < n_features; i++)
        for (int j = i; j > 0 && beats(j, j - 1); j--) {
            const op1_feature_t *t = features[j]; features[j] = features[j - 1]; features[j - 1] = t;
            uint8_t b = builtin[j]; builtin[j] = builtin[j - 1]; builtin[j - 1] = b;
        }
    for (int a = 0; a < n_features; a++)
        for (const char *const *ca = features[a]->claims; ca && *ca && !off[a]; ca++)
            for (int b = a + 1; b < n_features; b++) {
                if (off[b]) continue;
                for (const char *const *cb = features[b]->claims; cb && *cb; cb++) {
                    if (!same(*ca, *cb)) continue;
                    int lose = b, win = a;                    /* only earlier accepted owners can reject b */
                    off[lose] = 1;
                    if (kn_inv_lost) kn_inv_lost(features[lose], *ca, features[win]->name);
                    static char m[64];
                    char *o = m;
                    for (const char *s = features[lose]->name; s && *s && o < m + 20; s++) *o++ = *s;
                    for (const char *s = " off: "; *s; s++) *o++ = *s;
                    for (const char *s = *ca; *s && o < m + 44; s++) *o++ = *s;
                    for (const char *s = " taken"; *s; s++) *o++ = *s;
                    *o = 0;
                    kn_log_line(features[win]->name ? features[win]->name : "?", m);
                    op1_toast(m);
                    break;
                }
                if (off[a]) break;
            }
    int k = 0;
    for (int i = 0; i < n_features; i++)
        if (!off[i]) { builtin[k] = builtin[i]; features[k++] = features[i]; }
    n_features = k;
}

#define BUILTIN(x) do { if (&FEATURE_##x) kn_feature_register(&FEATURE_##x); } while (0)
void kn_features_finish(void)
{
    static int done;
    if (done) return;
    done = 1;
    registering_builtins = 1;
    BUILTIN(chord);
    BUILTIN(samplerplus);
    BUILTIN(randomiser);
    BUILTIN(sysmon);
    BUILTIN(usbout);
    BUILTIN(settings);
    BUILTIN(oblique);
    BUILTIN(quad);
    BUILTIN(markers);
    BUILTIN(punch);
    BUILTIN(countin);
    BUILTIN(fasterase);
    BUILTIN(scale);
    BUILTIN(euclid);
    BUILTIN(dots);
    BUILTIN(oracle);
    BUILTIN(peal);
    BUILTIN(drumplus);
    BUILTIN(midiseq);
    BUILTIN(midicc);
    BUILTIN(sampletools);
    registering_builtins = 0;
    resolve_claims();
    for (int i = 1; i < n_features; i++)                        /* insertion sort by priority, stable */
        for (int j = i; j > 0 && features[j - 1]->priority > features[j]->priority; j--) {
            const op1_feature_t *t = features[j]; features[j] = features[j - 1]; features[j - 1] = t;
            uint8_t u = builtin[j]; builtin[j] = builtin[j - 1]; builtin[j - 1] = u;
        }
    for (int i = 0; i < n_features; i++) {
        const op1_feature_t *f = features[i];
        if (f->note_on && f->note_off) stages[n_stages++] = f;
    }
    kn_feature_initializing=1;
    for (int i = 0; i < n_features; i++) if (features[i]->event) features[i]->event(OP1_EV_REGISTER, 0);
    kn_feature_initializing=0;
    /* Factory rows survive boots: remove an absent/disabled sampler type too, after the features registered (sampler+
     * registers its row then) and only when there is one: no write transaction on boots where nothing changed. */
#ifndef OP1_HOST
    {
        extern const op1_samplerplus_t *op1_sampler;
        void *fdb = ((void *(*)(void))0x0196DD38)();
        if (!op1_sampler && op1_db_has(fdb, "SELECT id FROM synth_types WHERE type='sampler+'"))
            op1_db_write(fdb, "DELETE FROM synth_types WHERE type='sampler+'");
    }
#endif
    if(kn_drum_synth_finish)kn_drum_synth_finish();
    if (kn_pages_finish) kn_pages_finish();
    if (kn_seq_finish) kn_seq_finish();
    if (kn_lfo_finish) kn_lfo_finish();
    if (kn_drum_finish) kn_drum_finish();
    if (kn_tape_finish) kn_tape_finish();
}

int kn_feature_count(void) { return n_features; }
const op1_feature_t *kn_feature_at(int i) { return i >= 0 && i < n_features ? features[i] : 0; }

static int skip_scale(int next,const op1_note *n)
{
    return (n->extra==OP1_NOTE_SCALE_FINAL) && next<n_stages && same(stages[next]->name,"scale");
}
void op1_note_on(int next, const op1_note *n)
{
    if(skip_scale(next,n))next++;
    if (next < n_stages) stages[next]->note_on(n, next + 1);
    else if(n->extra==OP1_NOTE_SCALE_FINAL){op1_note clean=*n;clean.extra=0;VC_NOTE_ON(&clean);}
    else VC_NOTE_ON(n);
}

void op1_note_off(int next, const op1_note *n)
{
    if(skip_scale(next,n))next++;
    if (next < n_stages) stages[next]->note_off(n, next + 1);
    else if(n->extra==OP1_NOTE_SCALE_FINAL){op1_note clean=*n;clean.extra=0;VC_NOTE_OFF(&clean);}
    else VC_NOTE_OFF(n);
}

/* VoiceCtl key_down / key_up: CALL voicectl_synth_note_on / _note_off (hook.S hook_chordon / hook_chordoff) */
void note_pipe_on(const op1_note *n)  { op1_note_on(0, n); }
void note_pipe_off(const op1_note *n) { op1_note_off(0, n); }

/* every key event (dispatcher.c key_event_hook) */
void kn_features_key_sync(void)
{
    for (int i = 0; i < n_features; i++)
        if (features[i]->key_sync) features[i]->key_sync();
}
