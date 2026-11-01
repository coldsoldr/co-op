/*
 * Sampler preset guard: an OP-1 that holds more sampler presets than this firmware's limit still boots.
 *
 * R246's preset loader (patch_table_loader 0x01A0567C; boot: OP_Main 0x019F0456 -> 0x01A059D0, and again after a
 * save, 0x019AD40C) reads every row of synth_user ("SELECT id,patch,folder FROM synth_user": no ORDER BY, so rowid
 * order, oldest first) into the next free entry of the synth patch table, then counts it. For a sampler row it adds
 * one to the sampler count (0x018B6108, 9 factory + user) and asserts count <= limit (`R1 = imm7` at 0x01A05954;
 * 42 in R246, 63 on the workshop's 300-preset bases), patch.cpp:629, a fatal panic once OP_Main has armed it.
 * A device that came from a firmware with a higher limit (109 on some) or got extra presets otherwise stopped there.
 *
 * hook.S replaces the first instruction of that panic branch (0x01A05984) with a jump to _kn_sampler_over_stub,
 * which calls kn_sampler_over(limit) and continues the loader at the next row (0x01A0596A) without counting the
 * entry, so the next row overwrites it (as every reload does). Result: the first `limit` sampler presets by DB id are
 * loaded; the rest stay in the database and on flash, untouched, for a firmware with a higher limit. Nothing runs
 * while the count is within the limit (the branch is only taken past it). The save gate (0x019FEF0C) and the
 * disk-mode sync are unchanged: with the count at the limit, both refuse new sampler presets, as on a full device.
 *
 * The owner is told once per boot (kn_core.c: after any plugin boot toast, plus a log line), by the text below.
 *
 * Disk mode with that many sampler presets: entering disk mode exports every synth_user row to the FAT view
 * (0x0199DA6C, called from disk_mode_toggle). For a sampler row it first checks that the .raw exists with
 * yaffs_open (CALL at 0x0199DD7A) and keeps only the sign of the result: the handle is never closed, one leak per
 * sampler preset. Past about 94 own sampler presets yaffs runs out of handles, the export's own open of the next
 * .raw fails and R246 stops with filesync_common.cpp:236 "missing source file" (seen in the emulator with 100).
 * The hook (`.hook_diskprobe`) closes the handle again; the caller still sees the same result sign.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include <stdint.h>

#ifdef KN_SAMPLER_HOST
int host_yaffs_open(const char *path, int flags, int mode);
int host_yaffs_close(int fd);
#define KN_Y_OPEN  host_yaffs_open
#define KN_Y_CLOSE host_yaffs_close
#else
#define KN_Y_OPEN  ((int (*)(const char *, int, int))0x01A997F4)  /* yaffs_open (kn_files.c) */
#define KN_Y_CLOSE ((int (*)(int))0x01A99908)                      /* yaffs_close */
#endif

#ifdef KN_SAMPLER_HOST                                  /* host test: the counts live in the test */
extern int32_t KN_SAMPLER_HOST[];
#define KN_SAMPLER_COUNT ((volatile int32_t *)KN_SAMPLER_HOST)
#else
#define KN_SAMPLER_COUNT ((volatile int32_t *)0x018B6108)   /* preset counts object: +0 sampler presets */
#endif

volatile uint32_t kn_sampler_skipped;     /* sampler presets the loader left out (boot pass; a reload adds again) */
volatile uint32_t kn_sampler_limit;       /* the loader's limit, factory included */

/* the loader's over-limit branch (hook.S _kn_sampler_over_stub): the count was already stored as limit + 1 */
void kn_sampler_over(int32_t limit)
{
    *KN_SAMPLER_COUNT = limit;            /* back to the limit: the entry is not kept, as on a device at exactly the limit */
    kn_sampler_limit = (uint32_t)limit;
    kn_sampler_skipped++;
}

static char *put_u(char *o, uint32_t n)
{
    char d[10]; int k = 0;
    do d[k++] = (char)('0' + n % 10); while (n /= 10);
    while (k) *o++ = d[--k];
    return o;
}
static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

/* "12 sampler sounds not loaded, limit 63", or 0 when nothing was left out. m holds 48 bytes. The stock font has no
 * parentheses (they draw as "?"); comma and colon exist. */
int kn_sampler_guard_text(char *m)
{
    uint32_t n = kn_sampler_skipped;
    if (!n) return 0;
    char *o = put_u(m, n);
    o = put_s(o, n == 1 ? " sampler sound not loaded, limit " : " sampler sounds not loaded, limit ");
    o = put_u(o, kn_sampler_limit);
    *o = 0;
    return 1;
}

/* disk-mode export, sampler row: CALL yaffs_open at 0x0199DD7A (an existence check) -> the same, handle closed */
int kn_export_probe_open(const char *path, int flags, int mode)
{
    int fd = KN_Y_OPEN(path, flags, mode);
    if (fd >= 0) KN_Y_CLOSE(fd);
    return fd;
}
