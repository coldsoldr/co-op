/*
 * coop's bridge on the host (core/bridge, docs/bridge.md B1). Every fixture is built here, in memory: no plugin file
 * of anyone's is in the repository.
 *   bridge_test                 the checks below
 *   bridge_test --loader        kn_parse_as or kn_parse: which parse this build's loader has (docs/bridge.md)
 *   bridge_test --tsv FILE...   one line a real .knl / .opmod (file, dialect, kind, phase, needs, known, unlisted):
 *                               tools/bridge/survey.py --check compares it with its own verdicts
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../../../core/bridge/bridge.h"
#include "../../../sdk/coop_plugin.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a), *b_ = (b); if (!a_ || std::strcmp(a_, b_)) { g_fail++; \
    std::printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_); } } while (0)

static uint32_t crc32z(const std::vector<uint8_t> &d, size_t skip)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < d.size(); i++) {
        c ^= (i >= skip && i < skip + 4) ? 0 : d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void p16(std::vector<uint8_t> &d, size_t o, uint32_t v) { d[o] = (uint8_t)v; d[o + 1] = (uint8_t)(v >> 8); }
static void p32(std::vector<uint8_t> &d, size_t o, uint32_t v) { p16(d, o, v); p16(d, o + 2, v >> 16); }
static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---- a kennel-format file made here (third_party/kennel/docs/plugin-format.md): 64 bytes of text, an entry object in
 * .data, 32 bytes of .bss; imports called from the text (PCREL24) or pointed at from .data (ABS32) ---- */
struct Knl {
    const char *magic = "KNPL";
    int kind = KN_KIND_FX, api = 1;
    uint32_t flags = 0;
    const char *name = "testfx";
    std::vector<std::string> calls, datas;      /* imports: called / addressed */
    bool settings_ext = false;                  /* kennel's extension tag 7 */
    std::vector<uint32_t> abs;                  /* 32-bit constants the text builds with P0.H / P0.L */
    bool svg = false;
};
enum { TEXT = 64, DATA = 64, IMAGE = TEXT + DATA, BSS = 32 };

static std::vector<uint8_t> make_knl(const Knl &k)
{
    std::vector<uint8_t> img(IMAGE, 0);
    struct Rel { uint32_t off, typ, sym; int32_t add; };
    std::vector<Rel> rel;
    std::vector<std::string> imports;
    size_t t = 0;
    for (auto &c : k.calls) {                   /* CALL import: 0xE300 0x0000, the relocation on the second halfword */
        p16(img, t, 0xE300);
        rel.push_back({ (uint32_t)t + 2, 4, (uint32_t)imports.size(), 0 });
        imports.push_back(c);
        t += 4;
    }
    for (uint32_t a : k.abs) {                  /* P0.H = hi; P0.L = lo */
        p16(img, t, 0xE148); p16(img, t + 2, a >> 16);
        p16(img, t + 4, 0xE108); p16(img, t + 6, a & 0xFFFF);
        t += 8;
    }
    p16(img, t, 0xE14A); p16(img, t + 4, 0xE10A);          /* P2.H / P2.L = the entry object's address: relocated */
    rel.push_back({ (uint32_t)t + 2, 2, 0xFFFF, TEXT });
    rel.push_back({ (uint32_t)t + 6, 3, 0xFFFF, TEXT });
    t += 8;
    for (; t + 2 <= TEXT; t += 2) p16(img, t, 0x0000);     /* NOP */
    /* the entry object at .data: init, render, press[4], render_stereo: pointers into the text */
    rel.push_back({ TEXT + 0, 1, 0xFFFF, 0 });
    rel.push_back({ TEXT + 4, 1, 0xFFFF, 8 });
    rel.push_back({ TEXT + 12, 1, 0xFFFF, 16 });
    uint32_t dref = TEXT + 40;
    for (auto &n : k.datas) {
        rel.push_back({ dref, 1, (uint32_t)imports.size(), 4 });
        imports.push_back(n);
        dref += 4;
    }
    std::string str;
    std::vector<uint32_t> name_off;
    for (auto &n : imports) { name_off.push_back((uint32_t)str.size()); str += n; str.push_back('\0'); }
    if (str.empty()) str.push_back('\0');

    std::vector<uint8_t> f;
    size_t ho = 0;
    if (k.svg) {
        const char *s = "<svg/>";
        f.assign(s, s + 7);
        while (f.size() % 4) f.push_back(0);
        ho = f.size();
    }
    const size_t svg_size = k.svg ? 7 : 0;
    f.resize(ho + KN_HEADER_SIZE, 0);
    const size_t img_off = f.size();
    f.insert(f.end(), img.begin(), img.end());
    const size_t imp_off = f.size();
    f.resize(f.size() + 4 * imports.size());
    for (size_t i = 0; i < imports.size(); i++) p32(f, imp_off + 4 * i, name_off[i]);
    const size_t rel_off = f.size();
    f.resize(f.size() + 12 * rel.size());
    for (size_t i = 0; i < rel.size(); i++) {
        p32(f, rel_off + 12 * i, rel[i].off); f[rel_off + 12 * i + 4] = (uint8_t)rel[i].typ;
        p16(f, rel_off + 12 * i + 6, rel[i].sym); p32(f, rel_off + 12 * i + 8, (uint32_t)rel[i].add);
    }
    const size_t str_off = f.size();
    f.insert(f.end(), str.begin(), str.end());
    while (f.size() % 4) f.push_back(0);
    size_t ext_off = 0;
    if (k.settings_ext) {                       /* tag 7, four bytes: to coop's parser a scratch size of 0x00010000 */
        ext_off = f.size();
        f.resize(f.size() + 12, 0);
        p16(f, ext_off, 7); p16(f, ext_off + 2, 4); p32(f, ext_off + 4, 0x00010000);
    }
    std::memcpy(&f[ho], k.magic, 4);
    p16(f, ho + 4, 1); p16(f, ho + 6, KN_HEADER_SIZE);
    p32(f, ho + 8, (uint32_t)f.size());
    p16(f, ho + 16, k.api); f[ho + 18] = (uint8_t)k.kind;
    std::strncpy((char *)&f[ho + 20], k.name, 15);
    p32(f, ho + 36, k.flags);
    p32(f, ho + 56, TEXT); p32(f, ho + 60, TEXT); p32(f, ho + 64, 0);          /* text, rodata off / size */
    p32(f, ho + 68, TEXT); p32(f, ho + 72, DATA);                                /* data */
    p32(f, ho + 76, IMAGE); p32(f, ho + 80, BSS); p32(f, ho + 84, IMAGE); p32(f, ho + 88, 4);
    p32(f, ho + 92, TEXT); p32(f, ho + 96, KN_NONE);                             /* entry, screen */
    p32(f, ho + 100, (uint32_t)img_off); p32(f, ho + 104, (uint32_t)rel_off); p32(f, ho + 108, (uint32_t)rel.size());
    p32(f, ho + 112, (uint32_t)imp_off); p32(f, ho + 116, (uint32_t)imports.size());
    p32(f, ho + 120, (uint32_t)str_off); p32(f, ho + 124, (uint32_t)str.size());
    p32(f, ho + 128, 0); p32(f, ho + 132, (uint32_t)svg_size); p32(f, ho + 136, KN_NONE); p32(f, ho + 140, (uint32_t)ext_off);
    p32(f, ho + 12, crc32z(f, ho + 12));
    return f;
}

/* ---- the loader's path for a .knl: normalise, the one parser, the imports ---- */
struct Loaded { int norm = 0, parsed = -1; const char *err = 0; br_verdict v; kn_info in; std::vector<uint8_t> f; };
static Loaded run_knl(const Knl &k)
{
    Loaded L;
    L.f = make_knl(k);
    L.norm = br_knl_normalise(L.f.data(), (uint32_t)L.f.size(), &L.v, &L.err);
    if (L.norm == 0) {
        L.parsed = br_knl_parse(L.f.data(), (uint32_t)L.f.size(), &L.in, &L.err);
        if (!L.parsed) br_knl_imports(L.f.data(), &L.in, &L.v);
    }
    return L;
}

static void test_knl()
{
    const char *err = 0;
    kn_info in;
    {   /* coop's loader refuses a .knl as it stands, by its magic */
        auto f = make_knl(Knl());
        CHECK(kn_parse(f.data(), (uint32_t)f.size(), COOP_PLUGIN_API, &in, &err) != 0);
        CHECK_STR(err, "not a coop plugin (magic)");
    }
    {   /* a plain mono effect: normalised, parsed by the one loader, loads from B2 */
        auto L = run_knl(Knl());
        CHECK(L.norm == 0 && L.parsed == 0);
        CHECK(L.v.phase == 2 && !L.v.reason);
        CHECK_STR(L.v.name, "testfx");
        CHECK(L.in.kind == KN_KIND_FX && L.in.load_size == IMAGE + BSS && L.in.scratch == 0);
        CHECK(L.v.load_size == IMAGE + BSS);
    }
    {   /* behind a screen SVG, as most kennel files are */
        Knl k; k.svg = true;
        auto L = run_knl(k);
        CHECK(L.norm == 0 && L.parsed == 0 && L.in.svg && L.in.svg_size == 7 && L.v.phase == 2);
    }
    {   /* a damaged file and a coop file are not the bridge's */
        auto f = make_knl(Knl());
        f[KN_HEADER_SIZE + 3] ^= 1;
        br_verdict v;
        CHECK(br_knl_normalise(f.data(), (uint32_t)f.size(), &v, &err) == -1);
        CHECK_STR(err, "CRC mismatch");
        Knl c; c.magic = "COOP";
        auto g = make_knl(c);
        CHECK(br_knl_normalise(g.data(), (uint32_t)g.size(), &v, &err) == -1);
        CHECK_STR(err, "not a kennel plugin (magic)");
        Knl a; a.api = 2;
        auto h = make_knl(a);
        CHECK(br_knl_normalise(h.data(), (uint32_t)h.size(), &v, &err) == -1);
        CHECK_STR(err, "built for a different kennel core");
    }
    {   /* an import the bridge does not have: refused with its name, nothing relocated */
        Knl k; k.calls = { "_op1_dbg", "_op1_tape_secret" };
        auto L = run_knl(k);
        CHECK(L.norm == 0 && L.parsed == 0);
        CHECK(L.v.phase == BR_PHASE_NEVER);
        CHECK_STR(L.v.reason, "needs an import the bridge does not provide");
        CHECK_STR(L.v.needs, "_op1_tape_secret");
        kn_export ex[64];
        const int n = br_knl_exports(8, ex, 64);
        std::vector<uint8_t> dst(4096, 0xAA);
        uint32_t used = 0;
        CHECK(kn_relocate_far(L.f.data(), &L.in, dst.data(), 0x00400000, 4096, ex, n, &err, &used) != 0);
        CHECK_STR(err, "an import is not exported by the core");
        CHECK(dst[0] == 0xAA);                       /* every import is looked up before a byte is written */
    }
    {   /* an import of a later phase, and one that is never served */
        Knl k; k.calls = { "_op1_fx_big" };
        auto L = run_knl(k);
        CHECK(L.v.phase == 8);
        CHECK_STR(L.v.needs, "_op1_fx_big");
        Knl s; s.kind = KN_KIND_SYNTH; s.calls = { "_op1_harmony_context" };
        auto M = run_knl(s);
        CHECK(M.v.phase == BR_PHASE_NEVER);
        CHECK_STR(M.v.needs, "_op1_harmony_context");
    }
    {   /* kennel's flag 0x20 is its own-envelope engine and coop's scratch effect: it never reaches kn_parse */
        Knl k; k.kind = KN_KIND_SYNTH; k.flags = BR_KNF_OWN_ENV;
        auto f = make_knl(k);
        auto before = f;
        br_verdict v;
        CHECK(br_knl_normalise(f.data(), (uint32_t)f.size(), &v, &err) == 1);
        CHECK(v.phase == 8);
        CHECK_STR(v.needs, "flag engine extension");
        CHECK(f == before);                          /* refused: the bytes are untouched */
        Knl x; x.flags = BR_KNF_OWN_ENV;             /* on an effect the bit means nothing in kennel */
        auto g = make_knl(x);
        CHECK(br_knl_normalise(g.data(), (uint32_t)g.size(), &v, &err) == 1 && v.phase == BR_PHASE_NEVER);
        CHECK_STR(v.reason, "flags this bridge does not know");
    }
    {   /* kinds and flags coop has no place for */
        Knl k; k.kind = KN_KIND_FEATURE;
        auto L = run_knl(k);
        CHECK(L.norm == 1 && L.v.phase == BR_PHASE_NEVER);
        CHECK_STR(L.v.needs, "kind feature");
        Knl i; i.kind = KN_KIND_SYNTH; i.flags = KN_FLAG_INPUT;
        auto M = run_knl(i);
        CHECK(M.norm == 1 && M.v.phase == BR_PHASE_NEVER);
        CHECK_STR(M.v.needs, "flag input");
        Knl s; s.kind = KN_KIND_SYNTH; s.flags = KN_FLAG_USES_SAMPLE;
        auto N = run_knl(s);
        CHECK(N.v.phase == 8);
        Knl p; p.kind = KN_KIND_SYNTH;
        auto O = run_knl(p);
        CHECK(O.norm == 0 && O.v.phase == 4);
    }
    {   /* kennel's extension tag 7 (settings rows) is coop's scratch size: parked, so the file is not read as a scratch
           effect with a size from someone's settings text */
        Knl k; k.settings_ext = true;
        auto raw = make_knl(k);
        std::memcpy(raw.data(), "COOP", 4);          /* what a careless magic swap would hand the parser */
        p32(raw, 12, crc32z(raw, 12));
        CHECK(kn_parse(raw.data(), (uint32_t)raw.size(), COOP_PLUGIN_API, &in, &err) != 0);
        CHECK_STR(err, "scratch flag and size disagree");
        auto L = run_knl(k);
        CHECK(L.norm == 0 && L.parsed == 0 && L.in.scratch == 0);
        CHECK(g16(L.f.data() + g32(L.f.data() + 140)) == BR_KN_EXT_PARKED);
    }
    {   /* relocation against the bridge's table: calls through far stubs to the stand-ins, data imports by address */
        Knl k; k.calls = { "_op1_dbg", "_op1_l1_claim" }; k.datas = { "_op1_fx_page" }; k.flags = KN_FLAG_STEREO;
        auto L = run_knl(k);
        CHECK(L.norm == 0 && L.parsed == 0 && L.v.phase == 2);
        kn_export ex[64];
        const int n = br_knl_exports(2, ex, 64);
        CHECK(n >= 3);
        CHECK(br_knl_exports(1, ex, 64) == 0);       /* this tree's level: B1 resolves nothing on a device */
        CHECK(br_knl_exports(BR_LEVEL, ex, 64) == 0);
        br_knl_exports(2, ex, 64);
        const uint32_t base = 0x00400000;
        std::vector<uint8_t> dst(4096, 0);
        uint32_t used = 0;
        CHECK(kn_relocate_far(L.f.data(), &L.in, dst.data(), base, 4096, ex, n, &err, &used) == 0);
        CHECK(used == ((IMAGE + BSS + 3u) & ~3u) + 2 * KN_STUB_SIZE);
        CHECK(g32(dst.data() + TEXT + 40) == (uint32_t)(uintptr_t)br_op1_fx_page + 4);
        CHECK(g32(dst.data() + TEXT + 0) == base && g32(dst.data() + TEXT + 4) == base + 8);
        const uint8_t *stub = dst.data() + IMAGE + BSS;                          /* P1.L / P1.H = br_op1_dbg */
        CHECK((g16(stub + 2) | g16(stub + 6) << 16) == (uint32_t)(uintptr_t)br_op1_dbg);
    }
    {   /* the stand-ins answer what kennel's SDK documents for a core with the service off */
        int fresh = 7; uint32_t size = 9; int16_t out[BR_FX_BLOCK];
        for (auto &o : out) o = 5;
        CHECK(br_op1_l1_claim(1, out, 64, &fresh) == 0 && fresh == 0);
        fresh = 7;
        CHECK(br_op1_l1_claim_state(1, out, 32, 64, &fresh) == 0 && fresh == 0);
        CHECK(br_op1_l1_sine_quarter() == 0);
        CHECK(br_op1_fx_fast(0, &size) == 0 && size == 0);
        CHECK(br_op1_sc_source(3, out) == 0 && out[0] == 0 && out[BR_FX_BLOCK - 1] == 0);
        CHECK(br_stub0() == 0 && br_op1_fx_page[2][3] == 0 && br_op1_eng_page[3] == 0);
        br_op1_dbg("x", 1, 2);
    }
    {   /* the addresses the code carries: known, unlisted in R246's L1, plain numbers, and the relocated pair */
        Knl k; k.abs = { 0x01AB8BA0, 0xFF801950, 0xFF801234, 0x01234567, 0x00012345 };
        auto L = run_knl(k);
        br_audit a;
        br_knl_audit(L.f.data(), &L.in, &a);
        CHECK(a.known == 2 && a.unlisted == 1 && a.first_unlisted == 0xFF801234 && a.pairs == 3);
        CHECK(br_addr_known(0xFFB00000) && !br_addr_known(0xFFB00004));
    }
    {   /* every table entry: a name once per dialect, phases in order, a stand-in only for a function or object */
        for (int i = 0; i < br_imports_count; i++) {
            const br_import *e = &br_imports[i];
            CHECK(br_import_find(e->dialect, e->name) == e);
            CHECK(e->full >= e->loads && e->loads <= 9);
            CHECK(e->dialect == BR_KNL ? e->name[0] == '_' : e->name[0] != '_');
            if (e->loads == BR_PHASE_NEVER) CHECK(!e->shim);
        }
        CHECK(br_addrs_count > 20);
    }
}

/* ---- the effect adapter: two instances of a mono kennel effect, one a channel ---- */
struct Inst { int32_t z[3]; int inits[3]; void *seen_scratch[3]; int renders, stereo_calls; };
static Inst gL, gR;
static void *g_tab[3];
static void one_pole(Inst &s, int pos, int16_t *buf, const int16_t *p)
{
    s.renders++;
    s.seen_scratch[pos] = g_tab[pos];
    for (int i = 0; i < BR_FX_BLOCK; i++) { s.z[pos] += ((buf[i] - s.z[pos]) * p[0]) >> 15; buf[i] = (int16_t)s.z[pos]; }
}
static void l_init(int pos) { gL.z[pos] = 0; gL.inits[pos]++; gL.seen_scratch[pos] = g_tab[pos]; }
static void r_init(int pos) { gR.z[pos] = 0; gR.inits[pos]++; gR.seen_scratch[pos] = g_tab[pos]; }
static void l_render(int pos, int16_t *b, const int16_t *p) { one_pole(gL, pos, b, p); }
static void r_render(int pos, int16_t *b, const int16_t *p) { one_pole(gR, pos, b, p); }
static void l_stereo(int pos, int16_t *l, int16_t *r, const int16_t *p)
{
    (void)pos; (void)p;
    gL.stereo_calls++;
    for (int i = 0; i < BR_FX_BLOCK; i++) { int16_t t = l[i]; l[i] = r[i]; r[i] = t; }   /* a true stereo effect: swap */
}

static void test_fx()
{
    static const br_knl_fx fl = { l_init, l_render, { 0, 0, 0, 0 }, l_stereo };
    static const br_knl_fx fr = { r_init, r_render, { 0, 0, 0, 0 }, 0 };
    static char stock[3], right[3];
    for (int i = 0; i < 3; i++) g_tab[i] = &stock[i];
    br_fx_inst x = { &fl, &fr, 0, g_tab, { &right[0], &right[1], &right[2] }, 0 };
    const int16_t params[16] = { 16384 };
    int32_t l[BR_FX_BLOCK], r[BR_FX_BLOCK];
    int16_t ref[BR_FX_BLOCK];

    CHECK(br_fx_init(&x, 1) == 0 && gL.inits[1] == 1 && gR.inits[1] == 1);
    CHECK(gL.seen_scratch[1] == &stock[1] && gR.seen_scratch[1] == &right[1] && g_tab[1] == &stock[1]);
    CHECK(br_fx_init(&x, 3) == -1 && br_fx_init(&x, 4) == -1);       /* kennel knows three positions */

    /* left alone carries a signal: the right instance hears silence and stays silent (no shared state) */
    for (int i = 0; i < BR_FX_BLOCK; i++) { l[i] = (int32_t)((uint32_t)(1000 + 37 * i) << 16); r[i] = 0; ref[i] = (int16_t)(1000 + 37 * i); }
    br_fx_render(&x, 1, l, r, params, BR_FX_BLOCK);
    int32_t z = 0; int same = 1, silent = 1;
    for (int i = 0; i < BR_FX_BLOCK; i++) {
        z += ((ref[i] - z) * params[0]) >> 15;
        if (l[i] != (int32_t)((uint32_t)z << 16)) same = 0;
        if (r[i]) silent = 0;
    }
    CHECK(same && silent && gL.renders == 1 && gR.renders == 1);
    CHECK(gR.seen_scratch[1] == &right[1] && gL.seen_scratch[1] == &stock[1] && g_tab[1] == &stock[1]);
    CHECK(gR.z[1] == 0 && gL.z[1] != 0);

    /* the widened int16 bus goes through exactly; other Q31 values round and saturate */
    static const br_knl_fx thru = { [](int) {}, [](int, int16_t *, const int16_t *) {}, { 0, 0, 0, 0 }, 0 };
    br_fx_inst t = { &thru, &thru, 0, 0, { 0, 0, 0 }, 0 };
    for (int i = 0; i < BR_FX_BLOCK; i++) { l[i] = (int32_t)((uint32_t)(i * 1021 - 32768) << 16); r[i] = -l[i] - 65536; }
    int32_t l0[BR_FX_BLOCK], r0[BR_FX_BLOCK];
    std::memcpy(l0, l, sizeof l); std::memcpy(r0, r, sizeof r);
    br_fx_render(&t, 0, l, r, params, BR_FX_BLOCK);
    CHECK(!std::memcmp(l0, l, sizeof l) && !std::memcmp(r0, r, sizeof r));
    l[0] = 0x7FFFFFFF; l[1] = (int32_t)0x80000000; l[2] = 0x00018000; l[3] = 0x00017FFF; l[4] = -0x8000; l[5] = -0x8001;
    br_fx_render(&t, 0, l, r, params, BR_FX_BLOCK);
    CHECK(l[0] == 0x7FFF0000 && l[1] == (int32_t)0x80000000 && l[2] == 0x00020000 && l[3] == 0x00010000 && l[4] == 0 && l[5] == -0x10000);

    /* a block that is not 64 samples, or a position past the master: dry, counted, the file is not called */
    const int calls = gL.renders;
    std::memcpy(l0, l, sizeof l);
    br_fx_render(&x, 1, l, r, params, 32);
    br_fx_render(&x, 3, l, r, params, BR_FX_BLOCK);
    CHECK(gL.renders == calls && x.dry_blocks == 2 && !std::memcmp(l0, l, sizeof l));

    /* kennel's stereo flag: one call with both channels at the master, the mono render once a channel elsewhere */
    x.stereo = 1;
    for (int i = 0; i < BR_FX_BLOCK; i++) { l[i] = 0x00010000; r[i] = 0x00020000; }
    br_fx_render(&x, 2, l, r, params, BR_FX_BLOCK);
    CHECK(gL.stereo_calls == 1 && l[5] == 0x00020000 && r[5] == 0x00010000 && gL.renders == calls);
    br_fx_render(&x, 0, l, r, params, BR_FX_BLOCK);
    CHECK(gL.stereo_calls == 1 && gL.renders == calls + 1 && gR.renders == 2);
}

/* ---- an .opmod made here: header, manifest, 16 bytes of image, imports, one relocation ---- */
struct Opmod {
    std::string manifest = "id test.fx\nname fx\ncore 2.9\nadds effect fx\n";
    std::vector<std::string> imports = { "opus_effect_register" };
    uint32_t places = 0, exports = 0, rel_kind = 0;
};
static std::vector<uint8_t> make_opmod(const Opmod &o)
{
    std::vector<uint8_t> f(0x60, 0);
    uint32_t w[20] = { 0 };
    w[0] = (uint32_t)f.size(); w[1] = (uint32_t)o.manifest.size();
    f.insert(f.end(), o.manifest.begin(), o.manifest.end());
    while (f.size() % 4) f.push_back(0);
    w[2] = (uint32_t)f.size(); w[3] = 16; w[4] = 32; w[5] = 0;
    f.resize(f.size() + 16, 0);
    std::string str;
    std::vector<uint32_t> offs;
    for (auto &n : o.imports) { offs.push_back((uint32_t)str.size()); str += n; str.push_back('\0'); }
    const uint32_t exp_name = (uint32_t)str.size();
    str += "lib_fn"; str.push_back('\0');
    w[6] = (uint32_t)f.size(); w[7] = (uint32_t)o.imports.size();
    f.resize(f.size() + 4 * o.imports.size());
    for (size_t i = 0; i < offs.size(); i++) p32(f, w[6] + 4 * i, offs[i]);
    w[8] = (uint32_t)f.size(); w[9] = 1;
    f.resize(f.size() + 12);
    p32(f, w[8], 4u | o.rel_kind << 28); p32(f, w[8] + 4, 0); p32(f, w[8] + 8, 8);   /* a local word at image + 4 */
    w[12] = (uint32_t)f.size(); w[13] = (uint32_t)str.size();
    f.insert(f.end(), str.begin(), str.end());
    while (f.size() % 4) f.push_back(0);
    w[14] = 1;
    w[16] = (uint32_t)f.size(); w[17] = o.exports;
    f.resize(f.size() + 8 * o.exports);
    for (uint32_t i = 0; i < o.exports; i++) { p32(f, w[16] + 8 * i, exp_name); p32(f, w[16] + 8 * i + 4, 0); }
    w[18] = (uint32_t)f.size(); w[19] = o.places;
    f.resize(f.size() + 24 * o.places);
    p32(f, 0, 0x444D504F); p16(f, 4, 1); p16(f, 6, 0x60); p32(f, 8, (uint32_t)f.size());
    for (int i = 0; i < 20; i++) p32(f, 0x10 + 4 * i, w[i]);
    p32(f, 12, crc32z(f, 12));
    return f;
}

static br_verdict run_opmod(const Opmod &o, const char **why)
{
    auto f = make_opmod(o);
    br_opmod m;
    br_verdict v;
    std::memset(&v, 0, sizeof v);
    *why = br_opmod_check(f.data(), (uint32_t)f.size(), &m);
    if (!*why) br_opmod_verdict(&m, &v);
    return v;
}

static void test_opmod()
{
    const char *why;
    {
        auto v = run_opmod(Opmod(), &why);
        CHECK(!why && v.phase == 6 && v.kind == KN_KIND_FX && !v.reason);
        CHECK_STR(v.name, "test.fx");
    }
    {
        Opmod o; o.manifest = "id test.eng\ncore 2.9\nadds engine e\n"; o.imports = { "opus_engine_add", "fw_snprintf" };
        auto v = run_opmod(o, &why);
        CHECK(!why && v.phase == 7 && v.kind == KN_KIND_SYNTH);
    }
    {   /* an import nobody maps: named */
        Opmod o; o.imports = { "opus_effect_register", "opus_on_post_fx" };
        auto v = run_opmod(o, &why);
        CHECK(!why && v.phase == BR_PHASE_NEVER);
        CHECK_STR(v.needs, "opus_on_post_fx");
    }
    {   /* places, libraries, things coop has no slot for, a missing core line */
        Opmod p; p.places = 1;
        CHECK(run_opmod(p, &why).phase == BR_PHASE_NEVER && !why);
        CHECK_STR(run_opmod(p, &why).needs, "place");
        Opmod e; e.exports = 1;
        CHECK_STR(run_opmod(e, &why).needs, "exports");
        Opmod n; n.manifest += "needs opus.orbit3d 0.2.6\n";
        CHECK_STR(run_opmod(n, &why).needs, "opus.orbit3d 0.2.6");
        Opmod l; l.manifest = "id t.l\ncore 2.22\nadds lfo matrix\n";
        CHECK_STR(run_opmod(l, &why).needs, "lfo matrix");
        Opmod c; c.manifest = "id t.c\nadds effect x\n";
        CHECK_STR(run_opmod(c, &why).needs, "core");
        Opmod a; a.manifest = "id t.a\ncore 2.9\n";
        CHECK_STR(run_opmod(a, &why).needs, "adds");
    }
    {   /* damage is caught before anything is read further */
        auto f = make_opmod(Opmod());
        br_opmod m;
        f[0x64] ^= 1;
        CHECK_STR(br_opmod_check(f.data(), (uint32_t)f.size(), &m), "damaged: crc");
        Opmod b; b.rel_kind = 3;
        run_opmod(b, &why);
        CHECK_STR(why, "a branch relocation (this core places images anywhere)");
        std::vector<uint8_t> tiny(8, 0);
        CHECK_STR(br_opmod_check(tiny.data(), 8, &m), "not an Opus plugin file (magic)");
    }
}

static int tsv(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        FILE *fp = std::fopen(argv[i], "rb");
        if (!fp) { std::fprintf(stderr, "cannot open %s\n", argv[i]); return 2; }
        std::vector<uint8_t> f;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) f.insert(f.end(), buf, buf + n);
        std::fclose(fp);
        const char *base = std::strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        br_verdict v;
        std::memset(&v, 0, sizeof v);
        const char *err = 0;
        if (f.size() >= 4 && !std::memcmp(f.data(), "OPMD", 4)) {
            br_opmod m;
            if ((err = br_opmod_check(f.data(), (uint32_t)f.size(), &m))) { std::fprintf(stderr, "%s: %s\n", base, err); return 2; }
            br_opmod_verdict(&m, &v);
            std::printf("%s\topmod\t-\t%d\t%s\t0\t0\n", base, v.phase, v.needs);
            continue;
        }
        const int norm = br_knl_normalise(f.data(), (uint32_t)f.size(), &v, &err);
        if (norm < 0) { std::fprintf(stderr, "%s: %s\n", base, err); return 2; }
        kn_info in;
        br_audit a = { 0, 0, 0, 0 };
        if (norm == 0) {
            if (br_knl_parse(f.data(), (uint32_t)f.size(), &in, &err)) {
                std::printf("%s\tknl\t%s\tPARSE\t%s\t0\t0\n", base, v.kind == KN_KIND_FX ? "fx" : "synth", err);
                continue;
            }
            br_knl_imports(f.data(), &in, &v);
            br_knl_audit(f.data(), &in, &a);
        } else {                                     /* refused by kind or flags (the bytes untouched): the audit still runs */
            const uint32_t ho = f[0] == '<' ? (uint32_t)((std::strlen((char *)f.data()) + 4) & ~3u) : 0;
            in.hdr_off = ho; in.text_size = g32(f.data() + ho + 56); in.image_size = g32(f.data() + ho + 84);
            br_knl_audit(f.data(), &in, &a);
        }
        std::printf("%s\tknl\t%s\t%d\t%s\t%u\t%u\n", base,
                    v.kind == KN_KIND_FX ? "fx" : v.kind == KN_KIND_SYNTH ? "synth" : "feature", v.phase, v.needs, a.known, a.unlisted);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--tsv")) return tsv(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--loader")) { std::puts(br_knl_parse_exact() ? "kn_parse_as" : "kn_parse"); return 0; }
    test_knl();
    test_fx();
    test_opmod();
    std::printf("bridge_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
