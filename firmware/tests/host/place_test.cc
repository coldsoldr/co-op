/*
 * coop-fw host test: the placement policy file's parser (core/sdram/coop_place.cc, e13, docs/plugins.md "e13").
 * The file is written by a host over SysEx and read at boot, so the parser must take any bytes: a text is valid as a
 * whole or ignored as a whole, and nothing in it can move the policy part of the way. coop_place.cc is compiled here
 * as it is (its file calls are never made).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
volatile uint32_t g_coop_pf_mode, g_plugins_scan_done;
extern "C" { volatile uint32_t g_coop_prof = 1, g_coop_statsfile = 1; volatile uint32_t g_coop_tail_skip = 1, g_coop_strike_spread, g_coop_stock_tail; }
#include "../../core/sdram/coop_place.cc"

static int fails;
static void expect(const char *text, int ok, const char *want, int pf, int probe)
{
    struct coop_place p = { COOP_IN_HEAP, COOP_IN_HEAP, COOP_IN_HEAP, COOP_IN_ARENA, 0, 0, 0, 0, 0, 0 };
    const struct coop_place before = p;
    int f = -7, pr = -7;
    int got = coop_place_parse(text, (int)strlen(text), &p, &f, &pr);
    char now[64];
    g_coop_place = p;
    coop_place_text(now, (int)sizeof now);
    int bad = got != ok;
    if (ok && (strcmp(now, want) || f != pf || pr != probe)) bad = 1;
    if (!ok && (memcmp(&p, &before, sizeof p) || f != -7 || pr != -7)) bad = 1;   /* refused: nothing may have moved */
    if (bad) { fails++; printf("FAIL \"%s\": valid %d (want %d), %s pf %d probe %d\n", text, got, ok, now, f, pr); }
}

int main(void)
{
    const char *def = "eng=heap,fx=heap,voice=heap,buf=arena";
    expect("", 1, def, -1, -1);
    expect("\n", 1, def, -1, -1);
    expect("all=arena", 1, "eng=arena,fx=arena,voice=arena,buf=arena", -1, -1);
    expect("all=heap\n", 1, "eng=heap,fx=heap,voice=heap,buf=heap", -1, -1);
    expect("all=arena img=heap", 1, "eng=heap,fx=heap,voice=arena,buf=arena", -1, -1);
    expect("eng=arena fx=heap voice=heap buf=arena", 1, "eng=arena,fx=heap,voice=heap,buf=arena", -1, -1);
    expect("eng=heap\r\nfx=arena\r\n", 1, "eng=heap,fx=arena,voice=heap,buf=arena", -1, -1);
    expect("  buf=heap\t voice=arena  ", 1, "eng=heap,fx=heap,voice=arena,buf=heap", -1, -1);
    expect("eng=arena eng=heap", 1, def, -1, -1);                         /* the later token wins */
    expect("pf=1", 1, def, 1, -1);
    expect("probe=1 pf=4 all=arena", 1, "eng=arena,fx=arena,voice=arena,buf=arena", 4, 1);
    expect("probe=0", 1, def, -1, 0);
    expect("prof=0", 1, def, -1, -1);                                     /* e14 */
    expect("all=arena prof=1", 1, "eng=arena,fx=arena,voice=arena,buf=arena", -1, -1);
    { struct coop_place p = { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }; int f, pr;
      if (!coop_place_parse("prof=0", 6, &p, &f, &pr) || !p.prof_set || p.prof != 0) { fails++; printf("FAIL prof=0 not taken\n"); }
      if (!coop_place_parse("prof=1 prof=0 prof=1", 20, &p, &f, &pr) || p.prof != 1) { fails++; printf("FAIL prof: the later token\n"); } }
    expect("statsfile=1 prof=0", 1, def, -1, -1);
    expect("statsfile=2", 0, 0, 0, 0);
    expect("tail=0 strike=2 stocktail=1", 1, def, -1, -1);               /* e16 */
    { struct coop_place p = { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 }; int f, pr;
      if (!coop_place_parse("tail=2 strike=6 stocktail=0", 27, &p, &f, &pr) || !p.tail_set || p.tail != 2 || p.strike != 6
          || !p.stock_set || p.stock != 0) { fails++; printf("FAIL e16 tokens not taken\n"); } }
    expect("tail=3", 0, 0, 0, 0);
    expect("strike=7", 0, 0, 0, 0);
    expect("stocktail=", 0, 0, 0, 0);
    expect("prof=2", 0, 0, 0, 0);
    expect("prof=", 0, 0, 0, 0);
    /* refused whole */
    expect("all=arena bogus=1", 0, 0, 0, 0);
    expect("eng=heap fx=aren", 0, 0, 0, 0);
    expect("eng=heaps", 0, 0, 0, 0);
    expect("eng=", 0, 0, 0, 0);
    expect("=heap", 0, 0, 0, 0);
    expect("eng", 0, 0, 0, 0);
    expect("all=arena pf=5", 0, 0, 0, 0);
    expect("all=arena pf=12", 0, 0, 0, 0);
    expect("probe=2", 0, 0, 0, 0);
    expect("ENG=heap", 0, 0, 0, 0);
    expect("eng=heap;fx=arena", 0, 0, 0, 0);
    expect("all=arena \x01\xff", 0, 0, 0, 0);
    /* every byte value once, and every prefix of a valid text: never a crash, never half a policy */
    char any[257];
    for (int i = 0; i < 256; i++) any[i] = (char)(i ? i : 1);
    any[256] = 0;
    expect(any, 0, 0, 0, 0);
    const char *full = "eng=arena fx=arena voice=arena buf=heap pf=1 probe=1";
    for (int n = 0; n <= (int)strlen(full); n++) {
        struct coop_place p = { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        int f = -1, pr = -1;
        (void)coop_place_parse(full, n, &p, &f, &pr);
        if (p.eng > 1 || p.fx > 1 || p.voice > 1 || p.buf > 1) { fails++; printf("FAIL prefix %d\n", n); }
    }
    if (fails) { printf("place: %d failures\n", fails); return 1; }
    printf("place: the policy parser takes valid texts whole and refuses the others whole (25 texts, every byte value, every prefix)\n");
    return 0;
}
