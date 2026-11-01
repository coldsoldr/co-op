/*
 * coop-fw Phase 1: the L1 dumper.
 *
 * A one-shot payload injected into the stock OP-1 app (firmware R246) in free SDRAM region A
 * (0x02c00000). It runs entirely from the main loop: tools/inject_app.py retargets the main-loop
 * tape-service CALL at 0x019F0A24 (stock target tape_c::service, 0x01A54710) to ServiceHook(),
 * which captures the L1 image and the OTP key, writes them to a file, and then forwards to the
 * stock tape service so the device runs normally. Nothing runs in the audio interrupt.
 *
 * Why this works: by the time the main loop calls the tape service, YAFFS is mounted and the
 * runtime-decrypted L1 is in place. L1 instruction SRAM is not readable by ordinary core loads,
 * so it is copied through MDMA0 (the engine the display copy uses); the data regions are copied
 * with plain loads. The OTP page-0xD0 key sits in the te-boot context block at 0x03ffff10, where
 * the init DXE left it (app-boot-chain.md).
 *
 * The key and the decrypted code belong to the owner's own device; the output file
 * (/yaffs2/user/coop_l1.bin) is gitignored and never leaves the owner's hands. The dump exists so
 * coop-fw can reverse-engineer and replace the audio core (the plan, Phase 1).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE, NOTICE).
 */
#include <stdint.h>
#include "fw_addrs.h"

/* ---- the image header inject_app.py reads (OP1X), at the region start ---- */
typedef void (*render_fn)(void *self, int16_t *out, const int16_t *params);
typedef void (*noteon_fn)(void *self, const int16_t *knobs);
typedef void (*void_fn)(void *self);

struct patch {
    uint32_t address, kind, stock;
    const void *value;
};

struct header {
    uint32_t magic, version;
    render_fn render;
    noteon_fn note_on;
    void_fn note_off;
    const struct patch *patches;
    uint32_t num_patches;
    uint32_t reserved;
};

enum { HEADER_MAGIC = 0x5831504fu };          /* "OP1X" */
enum { PATCH_CALL24 = 3 };

/* The stock main-loop tape service the hook replaces (decoded from kennel's recorded CALL bytes at
 * 0x019F0A24: target 0x01A54710). ServiceHook forwards to it so the device behaves normally. */
#define FW_TAPE_SERVICE 0x01A54710u

/* ---- output file format (coop_l1.bin), little-endian ----
 *   u32 magic "C1DP", u32 version, u32 region_count
 *   region_count x { u32 addr, u32 len }     (the table)
 *   u8 key[16]                               (OTP page 0xD0 as the context block holds it)
 *   u32 otp_status, u32 otp_bytes            (version >= 2; otp_bytes 0 when not captured)
 *   u8 otp[otp_bytes]                        (the full OTP, for op1emu's otp.bin; DUMP_OTP builds)
 *   the raw region bytes, in table order
 *
 * The context-block key is always present and is enough for the Phase 1 key check. A DUMP_OTP build
 * also reads the whole OTP through the boot ROM so tools/mk_otp_bin.py can make op1emu's otp.bin.
 */
#define OUT_MAGIC   0x50443143u               /* "C1DP" */
#define OUT_VERSION 2u

/* ---- OTP, read through the BF52x boot ROM (DUMP_OTP builds only) ----
 * Mirrors the init DXE's sequence (app-boot-chain.md, te-boot-init.dis at 0xffa00514): OtpCommand(1)
 * to init, then two 64-bit half reads per 128-bit logical page, then OtpCommand(2) to close. Reads
 * are non-destructive; OTP is one-time-programmable and we never write it. The ROM entry addresses
 * are the ones the OP-1's own init DXE calls. */
#define ROM_OTP_COMMAND 0xEF000018u           /* bfrom_OtpCommand(cmd): 1 = init, 2 = close */
#define ROM_OTP_READ    0xEF00001Au           /* bfrom_OtpRead(page, half, u64* dst): half 0 lo, 1 hi */
#define OTP_PAGES       0x200u                 /* op1emu: 0x200 logical pages x 16 bytes = 8 KiB */
#define OTP_BYTES       (OTP_PAGES * 16u)

struct region {
    uint32_t addr;
    uint32_t len;
    uint32_t src;
    int use_mdma;                             /* instruction SRAM must go through MDMA (DUMP_L1CODE) */
};

/* Staging lives in .bss (an LDR FILL block). 48K instr + 12K dataA + 32K dataB + 4K scratch = 96
 * KiB; round up to 128 KiB. */
#define STAGE_BYTES (128u * 1024u)
static uint8_t g_stage[STAGE_BYTES] __attribute__((aligned(32)));
static uint8_t g_key[16] __attribute__((aligned(4)));
static volatile int g_captured;
static volatile int g_written;
static volatile uint32_t g_passes;

#ifdef DUMP_OTP
static uint8_t g_otp[OTP_BYTES] __attribute__((aligned(8)));
static volatile uint32_t g_otp_status;        /* 0 = not read, 1 = read ok, 2 = a read reported an error */

static void read_otp(void)
{
    typedef int (*otp_cmd_fn)(uint32_t cmd);
    typedef int (*otp_read_fn)(uint32_t page, uint32_t half, void *dst);
    otp_cmd_fn cmd = (otp_cmd_fn)(uintptr_t)ROM_OTP_COMMAND;
    otp_read_fn rd = (otp_read_fn)(uintptr_t)ROM_OTP_READ;
    int bad = 0;
    cmd(1);                                    /* init */
    for (uint32_t p = 0; p < OTP_PAGES; p++) {
        uint8_t *dst = g_otp + p * 16;
        if (rd(p, 0, dst) & 1) bad = 1;        /* bit 0 = error, per the init DXE's test */
        if (rd(p, 1, dst + 8) & 1) bad = 1;
    }
    cmd(2);                                    /* close */
    g_otp_status = bad ? 2 : 1;
}
#endif

static inline void ssync(void) { __asm__ __volatile__("ssync;" ::: "memory"); }

static void copy_plain(uint8_t *dst, uint32_t src, uint32_t bytes)
{
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)src;
    for (uint32_t i = 0; i < bytes; i++) dst[i] = s[i];
}

#ifdef DUMP_L1CODE
/* ---- MDMA1 copy of L1 instruction SRAM to SDRAM (16-bit words) ----
 * L1 instruction SRAM is not readable by core data loads, so it needs DMA. MDMA1 is used because
 * MDMA0 is the firmware's display engine. The busy-wait is bounded: a misconfigured transfer gives
 * up instead of hanging the device. */
static inline void w16(uint32_t mmr, uint16_t v) { *(volatile uint16_t *)mmr = v; }
static inline void w32(uint32_t mmr, uint32_t v) { *(volatile uint32_t *)mmr = v; }

#define DMA_WDSIZE_16 0x0004
#define DMA_EN        0x0001
#define DMA_WNR       0x0002                   /* 1 = destination (writes memory) */
#define MDMA_SPINS    2000000u                 /* bound: ~ms at 400 MHz, far longer than any transfer */

static int mdma1_copy(void *dst, uint32_t src, uint32_t bytes)
{
    uint32_t count = bytes >> 1;               /* the instruction region is a multiple of 2 */
    w16(MDMA1_S_CONFIG, 0);
    w16(MDMA1_D_CONFIG, 0);
    ssync();
    w32(MDMA1_S_START_ADDR, src);
    w16(MDMA1_S_X_MODIFY, 2);
    w16(MDMA1_S_X_COUNT, (uint16_t)count);
    w32(MDMA1_D_START_ADDR, (uint32_t)dst);
    w16(MDMA1_D_X_MODIFY, 2);
    w16(MDMA1_D_X_COUNT, (uint16_t)count);
    ssync();
    w16(MDMA1_S_CONFIG, DMA_WDSIZE_16 | DMA_EN);
    w16(MDMA1_D_CONFIG, DMA_WDSIZE_16 | DMA_WNR | DMA_EN);
    ssync();
    uint32_t spins = 0;
    while (!(*(volatile uint16_t *)MDMA1_D_IRQ_STATUS & 0x1)) {
        if (++spins > MDMA_SPINS) break;       /* give up rather than hang */
    }
    int done = (*(volatile uint16_t *)MDMA1_D_IRQ_STATUS & 0x1) != 0;
    *(volatile uint16_t *)MDMA1_D_IRQ_STATUS = 0x1;       /* W1C DMA_DONE */
    w16(MDMA1_S_CONFIG, 0);
    w16(MDMA1_D_CONFIG, 0);
    ssync();
    return done;
}
#endif

/* Regions captured, in file order. The minimal build dumps only what core loads can read (the data
 * SRAMs and the scratchpad) plus the key; that is enough for the key check and for op1emu. The
 * DUMP_L1CODE build adds L1 instruction SRAM via MDMA1 for a direct device-vs-emulator cross-check. */
static const struct region g_regions[] = {
#ifdef DUMP_L1CODE
    { L1_INSTR_BASE,   L1_INSTR_SIZE,   L1_INSTR_BASE,   1 },
#endif
    { L1_DATA_A_BASE,  L1_DATA_A_SIZE,  L1_DATA_A_BASE,  0 },
    { L1_DATA_B_BASE,  L1_DATA_B_SIZE,  L1_DATA_B_BASE,  0 },
    { L1_SCRATCH_BASE, L1_SCRATCH_SIZE, L1_SCRATCH_BASE, 0 },
};
#define NREGIONS (sizeof(g_regions) / sizeof(g_regions[0]))

static uint32_t total_bytes(void)
{
    uint32_t n = 0;
    for (unsigned r = 0; r < NREGIONS; r++) n += g_regions[r].len;
    return n;
}

static void capture(void)
{
    uint32_t off = 0;
    for (unsigned r = 0; r < NREGIONS; r++) {
        uint8_t *dst = g_stage + off;
        if (g_regions[r].use_mdma) {
#ifdef DUMP_L1CODE
            mdma1_copy(dst, g_regions[r].src, g_regions[r].len);
#endif
        } else {
            copy_plain(dst, g_regions[r].src, g_regions[r].len);
        }
        off += g_regions[r].len;
    }
    const volatile uint8_t *key = (const volatile uint8_t *)(uintptr_t)FW_OTP_KEY_IN_CONTEXT;
    for (int i = 0; i < 16; i++) g_key[i] = key[i];
#ifdef DUMP_OTP
    read_otp();
#endif
    g_captured = 1;
}

/* ---- file write (main-loop thread context) ---- */
typedef int (*yopen_fn)(const char *path, int oflag, int mode);
typedef int (*ywrite_fn)(int fd, const void *buf, unsigned n);
typedef int (*yclose_fn)(int fd);

static void write_file(void)
{
    yopen_fn y_open = (yopen_fn)(uintptr_t)FW_YAFFS_OPEN;
    ywrite_fn y_write = (ywrite_fn)(uintptr_t)FW_YAFFS_WRITE;
    yclose_fn y_close = (yclose_fn)(uintptr_t)FW_YAFFS_CLOSE;

    int fd = y_open("/yaffs2/user/coop_l1.bin", 2 | 0x40 | 0x200, 0x180);  /* O_RDWR|O_CREAT|O_TRUNC */
    if (fd < 0) return;

    uint32_t h[3] = { OUT_MAGIC, OUT_VERSION, (uint32_t)NREGIONS };
    y_write(fd, h, sizeof(h));
    for (unsigned r = 0; r < NREGIONS; r++) {
        uint32_t t[2] = { g_regions[r].addr, g_regions[r].len };
        y_write(fd, t, sizeof(t));
    }
    y_write(fd, g_key, sizeof(g_key));
#ifdef DUMP_OTP
    uint32_t otp_hdr[2] = { g_otp_status, OTP_BYTES };
    y_write(fd, otp_hdr, sizeof(otp_hdr));
    y_write(fd, g_otp, OTP_BYTES);
#else
    uint32_t otp_hdr[2] = { 0u, 0u };          /* version 2 field: OTP not captured in this build */
    y_write(fd, otp_hdr, sizeof(otp_hdr));
#endif
    uint32_t off = 0, left = total_bytes();
    while (left) {
        uint32_t chunk = left > 4096 ? 4096 : left;
        y_write(fd, g_stage + off, chunk);
        off += chunk;
        left -= chunk;
    }
    y_close(fd);
}

/* ServiceHook replaces the CALL tape_c::service at 0x019F0A24. The stock service takes a single int
 * argument in R0 (kennel's STOCK_SERVICE is int(*)(int); tape_c is a singleton, no `this`). Capture
 * on the first pass, write once the filesystem has settled, and always forward to the stock service
 * so the device runs normally. */
extern "C" int ServiceHook(int arg)
{
#ifndef CANARY
    if (!g_captured) capture();
    g_passes++;
    if (g_captured && !g_written && g_passes >= 64) { g_written = 1; write_file(); }
#else
    g_passes++;                                /* CANARY: prove the hook boots; do nothing else */
#endif
    return ((int (*)(int))(uintptr_t)FW_TAPE_SERVICE)(arg);
}

/* One patch: retarget the main-loop tape-service CALL to ServiceHook. inject_app.py recomputes the
 * stock CALL from `stock` (the target) and checks the bytes before writing (kind 3). */
extern "C" const struct patch g_patches[] = {
    { FW_CI_SERVICE_CALL, PATCH_CALL24, FW_TAPE_SERVICE, (const void *)&ServiceHook },
};

extern "C" __attribute__((section(".op1header"), used))
const struct header op1_header = {
    HEADER_MAGIC, 1u,
    0,            /* no render hook: the dumper does not touch the audio path */
    0,
    0,
    g_patches,
    1u,
    0u,
};
