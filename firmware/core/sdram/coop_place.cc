/*
 * coop-fw e13: where plugin memory goes, chosen at boot (docs/plugins.md "e13").
 *
 * e11 took every plugin byte from the stock heap 0; e12 took all of it from coop's arena in regions A / B and was
 * slower on the device (the plugins' code stalls about doubled). e13 can do either per class of memory, so the choice
 * can be measured on the device with a file write and a power cycle:
 *
 *   eng    a synth engine's image (code, data, .bss)        heap | arena
 *   fx     an effect's image                                 heap | arena
 *   voice  the engines' per-voice state                      heap | arena
 *   buf    the effect position buffers                       heap | arena
 * (screen objects, control tables and the boot scan's staging block are cold and always the arena's.)
 *
 * The policy file, /yaffs2/user/coop_place.txt (SysEx PUT writes it): ASCII tokens separated by spaces or newlines,
 *   eng=heap|arena  fx=heap|arena  voice=heap|arena  buf=heap|arena
 *   img=heap|arena  (eng and fx)   all=heap|arena  (all four)
 *   pf=0..4         the performance monitor's mode (coop_stats.cc g_coop_pf_mode; 1 = line fills counted)
 *   probe=0|1       the live fill probe (coop_stats.cc: a cold code and data pass in region A and in heap 0, every
 *                   stats window)
 * Later tokens win. No file: the built-in default. An unknown token, a bad value or a file of 128 bytes or more: the
 * whole file is ignored (the default), never a partial reading. Nothing here can fault on a bad file: it is read into
 * a fixed buffer and compared byte by byte.
 *
 * The marker: a boot that reads a valid file first writes /yaffs2/user/coop_place.try and removes it once the main
 * loop has run for a second after the plugin scan (the tape's cache is built by then: the e6 hang would have come
 * first). A boot that finds the marker did not get that far last time: it ignores the policy file and runs the
 * default, and says so (place_src 3), until a host deletes the marker (SysEx DELETE) or the file. So a policy that
 * hangs the boot costs one power cycle, and the OP-1 is on the default again with SysEx up.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_place.h"

#define Y_OPEN   ((int (*)(const char *, int, int))0x01A997F4u)           /* plugin_host.cc's */
#define Y_CLOSE  ((int (*)(int))0x01A99908u)
#define Y_READ   ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE  ((int (*)(int, const void *, int))0x01A99F84u)
#define Y_UNLINK ((int (*)(const char *))0x01A9A3FCu)                     /* coop_sysex.cc's */
#define PLACE_FILE "/yaffs2/user/coop_place.txt"
#define PLACE_TRY  "/yaffs2/user/coop_place.try"
#define FILE_CAP   128
#define DONE_CYCLES 400000000u                                            /* ~1 s at 400 MHz */

extern volatile uint32_t g_coop_pf_mode;                                  /* coop_stats.cc */
extern volatile uint32_t g_plugins_scan_done;                             /* plugin_host.cc */
/* The built-in default (docs/plugins.md "e13"): images and voice state in heap 0, as e11 ran them on the device;
 * the position buffers in the arena. */
struct coop_place g_coop_place = { COOP_IN_HEAP, COOP_IN_HEAP, COOP_IN_HEAP, COOP_IN_ARENA, COOP_PLACE_DEFAULT, 0 };
/* Image-time test switch (--set-word g_coop_place_word=0x1EFVB: bit 16 set, then one hex digit each for eng, fx,
 * voice, buf, 1 = heap): another built-in default, for the gates. 0 = the default above. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_place_word = 0; }
volatile uint32_t g_place_marker_writes, g_place_marker_clears, g_place_marker_errors;
/* probe=1 also reads the SDRAM controller's and the PLL's registers once, here at boot and after the marker is
 * written (so a read that faulted would cost one power cycle, not a reflash): EBIU_SDGCTL (32-bit, 0xFFC00A10: CAS
 * latency, tRAS, tRP, tRCD, tWR), EBIU_SDBCTL (0xFFC00A14: bank size, column width), EBIU_SDRRC (0xFFC00A18),
 * EBIU_SDSTAT (0xFFC00A1C), PLL_CTL (0xFFC00000: MSEL), PLL_DIV (0xFFC00004: CSEL, SSEL) [ADSP-BF52x hardware
 * reference, the 16-bit ones read as 16 bits]. The stats print them (`sdram` line) for the question why a cache line
 * from 0x02xxxxxx costs more than one from heap 0. op1emu is not asked (the read is skipped when the performance
 * monitor is not real: g_coop_pf_live, set later, cannot be used here, so the PFCTL probe is repeated). */
volatile uint32_t g_place_mmr[6], g_place_mmr_read;
static void read_sdram_mmrs(void)
{
    volatile uint32_t *const pfctl = (volatile uint32_t *)0xFFE08000u;
    uint32_t old = *pfctl;
    *pfctl = old | 1u;                                                     /* PFPWR sticks on the chip, reads back 0 in op1emu */
#ifndef OP1_HOST
    __asm__ __volatile__("ssync;" ::: "memory");
#endif
    int real = (*pfctl & 1u) != 0;
    *pfctl = old;
    if (!real) return;
    g_place_mmr[0] = *(volatile uint32_t *)0xFFC00A10u;
    g_place_mmr[1] = *(volatile uint16_t *)0xFFC00A14u;
    g_place_mmr[2] = *(volatile uint16_t *)0xFFC00A18u;
    g_place_mmr[3] = *(volatile uint16_t *)0xFFC00A1Cu;
    g_place_mmr[4] = *(volatile uint16_t *)0xFFC00000u;
    g_place_mmr[5] = *(volatile uint16_t *)0xFFC00004u;
    g_place_mmr_read = 1;
}
static uint8_t g_marker_pending;
static uint32_t g_done_at;                                                /* CYCLES | 1 when the scan was first seen done */

static int same(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return b[n] == 0; }
/* one token [t, t + n): 1 = taken into *p */
static int token(const char *t, int n, struct coop_place *p, int *pf, int *probe)
{
    int eq = 0;
    while (eq < n && t[eq] != '=') eq++;
    if (eq == 0 || eq >= n - 1) return 0;
    const char *v = t + eq + 1;
    int vn = n - eq - 1;
    if (same(t, "pf", eq)) { if (vn != 1 || v[0] < '0' || v[0] > '4') return 0; *pf = v[0] - '0'; return 1; }
    if (same(t, "probe", eq)) { if (vn != 1 || (v[0] != '0' && v[0] != '1')) return 0; *probe = v[0] - '0'; return 1; }
    int where;
    if (same(v, "heap", vn)) where = COOP_IN_HEAP; else if (same(v, "arena", vn)) where = COOP_IN_ARENA; else return 0;
    if (same(t, "eng", eq)) p->eng = (uint8_t)where;
    else if (same(t, "fx", eq)) p->fx = (uint8_t)where;
    else if (same(t, "voice", eq)) p->voice = (uint8_t)where;
    else if (same(t, "buf", eq)) p->buf = (uint8_t)where;
    else if (same(t, "img", eq)) p->eng = p->fx = (uint8_t)where;
    else if (same(t, "all", eq)) p->eng = p->fx = p->voice = p->buf = (uint8_t)where;
    else return 0;
    return 1;
}
/* The whole text or nothing: 1 = every token was understood (an empty file is a valid file that changes nothing). */
extern "C" int coop_place_parse(const char *text, int len, struct coop_place *p, int *pf, int *probe)
{
    struct coop_place q = *p;
    int f = -1, pr = -1, i = 0;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) i++;
        int s = i;
        while (i < len && text[i] != ' ' && text[i] != '\n' && text[i] != '\r' && text[i] != '\t') i++;
        if (i > s && !token(text + s, i - s, &q, &f, &pr)) return 0;
    }
    *p = q; *pf = f; *probe = pr;
    return 1;
}

extern "C" void coop_place_boot(void)                                     /* main thread, the start of the plugin boot */
{
    struct coop_place *p = &g_coop_place;
    uint32_t w = g_coop_place_word;
    if (w & 0x10000u) { p->eng = (w >> 12) & 1; p->fx = (w >> 8) & 1; p->voice = (w >> 4) & 1; p->buf = w & 1; }
    static char text[FILE_CAP];
    int fd = Y_OPEN(PLACE_FILE, 0, 0);
    if (fd < 0) return;                                                    /* no file: the default, no marker, no write */
    int got = Y_READ(fd, text, FILE_CAP);
    Y_CLOSE(fd);
    int mk = Y_OPEN(PLACE_TRY, 0, 0);
    if (mk >= 0) { Y_CLOSE(mk); p->src = COOP_PLACE_MARKER; return; }      /* the last boot with this file did not finish */
    int pf = -1, probe = -1;
    if (got < 0 || got >= FILE_CAP || !coop_place_parse(text, got, p, &pf, &probe)) { p->src = COOP_PLACE_MALFORMED; return; }
    p->src = COOP_PLACE_FILE;
    if (pf >= 0) g_coop_pf_mode = (uint32_t)pf;                            /* before coop_pf_boot programs the monitor */
    if (probe >= 0) p->probe = (uint8_t)probe;
    mk = Y_OPEN(PLACE_TRY, 0x242, 0x180);                                  /* O_RDWR | O_CREAT | O_TRUNC */
    if (mk >= 0) { int ok = Y_WRITE(mk, "1", 1) == 1; Y_CLOSE(mk); if (ok) { g_marker_pending = 1; g_place_marker_writes++; } else g_place_marker_errors++; }
    else g_place_marker_errors++;
    if (p->probe && g_marker_pending) read_sdram_mmrs();                   /* only under the marker's protection */
}

/* Main loop, every pass (coop_fx_service): the boot is through, the marker goes. */
extern "C" void coop_place_service(void)
{
    if (!g_marker_pending || !g_plugins_scan_done) return;
    uint32_t c;
#ifndef OP1_HOST                                                           /* tests/host/place_test.cc compiles this file on the PC */
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
#else
    c = 0;
#endif
    if (!g_done_at) { g_done_at = c | 1u; return; }
    if (c - g_done_at < DONE_CYCLES) return;
    g_marker_pending = 0;
    if (Y_UNLINK(PLACE_TRY) == 0) g_place_marker_clears++; else g_place_marker_errors++;
}

/* "eng=heap,fx=heap,voice=heap,buf=arena" (no spaces: one field of a stats line or of the inventory header) */
extern "C" int coop_place_text(char *out, int cap)
{
    static const char *const key[4] = { "eng=", ",fx=", ",voice=", ",buf=" };
    const struct coop_place *p = &g_coop_place;
    const uint8_t v[4] = { p->eng, p->fx, p->voice, p->buf };
    int n = 0;
    for (int k = 0; k < 4; k++) {
        for (const char *s = key[k]; *s && n < cap - 1; ) out[n++] = *s++;
        for (const char *s = v[k] ? "heap" : "arena"; *s && n < cap - 1; ) out[n++] = *s++;
    }
    out[n] = 0;
    return n;
}
extern "C" const char *coop_place_src_word(void)
{
    static const char *const w[4] = { "default", "file", "malformed", "marker" };
    return w[g_coop_place.src & 3];
}
