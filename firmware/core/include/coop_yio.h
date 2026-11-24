/*
 * coop-fw: the stock yaffs read and write, with the even-buffer rule (docs/brick-safety.md "Even buffers for the
 * stock file layer").
 *
 * TE's NAND driver asserts `te_yflash2.c:600 !((unsigned int)data & 1)`: a data buffer at an odd address halts the
 * device, at every boot if the read is a boot read (found by op1-sdk, docs/lessons.md 2026-11-15 "PLAY: a boot
 * failure from an odd buffer address"). A uint8_t / char array has alignment 1 on the Blackfin and -fdata-sections
 * gives each one its own section, so where it lands changes with unrelated edits.
 *
 * coop_y_read / coop_y_write hand an even buffer straight to the stock (0x01A99C90 / 0x01A99F84, as every file's
 * Y_READ / Y_WRITE) and move an odd one through a 64-byte aligned stack bounce, counting it in g_coop_odd_io (0 in a
 * good image: every coop buffer is even; tools/even_bufs.py checks the static ones at link time).
 */
#pragma once
#include <stdint.h>

#define COOP_Y_READ_STOCK   ((int (*)(int, void *, int))0x01A99C90u)
#define COOP_Y_WRITE_STOCK  ((int (*)(int, const void *, int))0x01A99F84u)
#define COOP_Y_BOUNCE       64

inline volatile uint32_t g_coop_odd_io = 0;                              /* odd buffers bounced (one per call) */

static __attribute__((noinline, unused)) int coop_y_read_odd(int fd, uint8_t *d, int n)
{
    uint8_t tmp[COOP_Y_BOUNCE] __attribute__((aligned(4)));
    int done = 0;
    g_coop_odd_io++;
    while (done < n) {
        int k = n - done < COOP_Y_BOUNCE ? n - done : COOP_Y_BOUNCE;
        int r = COOP_Y_READ_STOCK(fd, tmp, k);
        if (r < 0) return done ? done : r;
        for (int i = 0; i < r; i++) d[done + i] = tmp[i];
        done += r;
        if (r < k) break;                                               /* end of file */
    }
    return done;
}
static __attribute__((noinline, unused)) int coop_y_write_odd(int fd, const uint8_t *s, int n)
{
    uint8_t tmp[COOP_Y_BOUNCE] __attribute__((aligned(4)));
    int done = 0;
    g_coop_odd_io++;
    while (done < n) {
        int k = n - done < COOP_Y_BOUNCE ? n - done : COOP_Y_BOUNCE;
        for (int i = 0; i < k; i++) tmp[i] = s[done + i];
        int r = COOP_Y_WRITE_STOCK(fd, tmp, k);
        if (r < 0) return done ? done : r;
        done += r;
        if (r < k) break;                                               /* disk full */
    }
    return done;
}
static inline int coop_y_read(int fd, void *buf, int n)
{
    if ((uintptr_t)buf & 1u) return coop_y_read_odd(fd, (uint8_t *)buf, n);
    return COOP_Y_READ_STOCK(fd, buf, n);
}
static inline int coop_y_write(int fd, const void *buf, int n)
{
    if ((uintptr_t)buf & 1u) return coop_y_write_odd(fd, (const uint8_t *)buf, n);
    return COOP_Y_WRITE_STOCK(fd, buf, n);
}
