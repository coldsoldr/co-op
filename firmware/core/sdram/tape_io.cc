/*
 * coop-fw stereo tape, milestone T2 (docs/tape-stereo.md section 8): the file side of the lane-pair cache.
 *
 * Image: the stock FAT16 image file (/yaffs2/user/tape_c.raw, 0x18000000 bytes, kennel kn_tapes.c layout) now
 * holds FOUR STEREO AIFC tracks (sowt, 2 channels, 0xF28000 frames each, 63.5 MB) and no album. A marker
 * file /yaffs2/user/coop_tape.txt says the image has this layout; without it the first boot rebuilds the
 * image sparse (truncate, presize, boot sector, FATs, directories, headers) exactly as kennel's make_tape
 * does for its extra tapes. Going back to stock: stock's mount misses its six files and rebuilds its own
 * image (the recordings are gone, nothing else).
 *
 * Two stock functions are replaced through region-C trampolines (tools/l1_stereo_insert.py --tape-stereo):
 *   tape_image_mount_and_index 0x1996f38 (callers 0x199758e, 0x19f0354) -> coop_tape_mount_and_index:
 *     the layout is fixed, so the data offsets are constants: TRACK_BASE 0x018c1810[t] and the coop table;
 *     the album offsets 0xff9002c0/c4 point at tracks 1/2 so the (inert) album code never leaves the file.
 *   tape_cache_io_step 0x1a54710 (callers 0x19f0a24 service(0), 0x19f0be2 service(1)) -> coop_tape_io_step:
 *     the stock step with lane pairs: a chunk of track t is 32,768 interleaved frames (128 KiB) in the file at
 *     base[t] + chunk * 0x20000, de-interleaved into lanes t (L) and 4+t (R) of the node on load and
 *     interleaved from them on flush through a 128 KiB bounce buffer.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

/* ---- stock entry points and state (kennel kn_tapes.c / kn_tape.c, docs/tape-stereo.md) [C] ---- */
#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE       ((int (*)(int))0x01A99908u)
#define Y_READ        ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE       ((int (*)(int, const void *, int))0x01A99F84u)
#define Y_LSEEK       ((int32_t (*)(int, int32_t, int))0x01A9A0E4u)
#define Y_ACCESS      ((int (*)(const char *, int))0x01A9A6B0u)
#define PRESIZE       ((int (*)(int))0x01A99FB4u)                     /* tape_image_presize: 0x18000000 bytes */
#define BOOT_SECTOR   ((void (*)(int))0x019988C8u)                    /* fat_image_write_boot_template(fd) */
#define DISK_BIND     ((void (*)(int))0x019761D8u)                    /* fat_disk_bind_tape_image(fd) */
#define FAT_MOUNT     ((int (*)(int, void *))0x01998E74u)             /* f_mount(volume, fs); fs 0 unmounts */
#define FAT_FS        ((uint8_t *)0x01AABEE4u)                        /* FatFs volume: +4 window dirty, +0x2C window sector */
#define IMG_PATH      ((const char *)0x01AABCE4u)                     /* tape_c.raw path (built at boot) */
#define IMG_FD        (*(volatile int32_t *)0x018C180Cu)              /* tape_image_fat_handle */
#define SET_FILE      ((void (*)(int))0x01A5404Cu)                    /* tape_set_file_handle (the cache's fd copy) */
#define INVALIDATE_ALL ((void (*)(void))0x01A54078u)                  /* tape_cache_invalidate_clean */
#define TRACK_BASE    ((volatile int32_t *)0x018C1810u)               /* the stock 4-entry data offset table */
#define ALBUM_A_BASE  (*(volatile int32_t *)0xFF9002C0u)
#define ALBUM_B_BASE  (*(volatile int32_t *)0xFF9002C4u)
#define CACHE_HEAD    (*(uint8_t *volatile *)0x01B4EF3Cu)             /* node list head; node: B+0 valid, B+1 loading, B+2 flush req, W+4 dirty, W+6 chunk, +0x10 data, +0x14 next */
#define CACHE_FD      (*(volatile int32_t *)0x01B4EF44u)
#define NODE_VALID    0
#define NODE_LOADING  1
#define NODE_FLUSHREQ 2
#define NODE_DIRTY    4
#define NODE_CHUNK    6
#define NODE_DATA     0x10
#define NODE_NEXT     0x14
#define TRACKS        4
#define LANE_BYTES    0x10000
#define FRAMES_PER_CHUNK 32768
#define CHUNKS        485

/* ---- image layout (kennel kn_tapes.c constants; stereo tracks) ---- */
#define IMG_SIZE      0x18000000
#define FAT1          0x1000
#define FAT2          0x19000
#define ROOT          0x31000
#define DATA          0x35000
#define CLUSTER       0x2000
#define CL_OFF(c)     (DATA + ((int32_t)(c) - 2) * CLUSTER)
#define TRACK_FRAMES  0xF28000
#define AIFC_HDR      0x70
#define TRACK_BYTES   (AIFC_HDR + TRACK_FRAMES * 4)                   /* 63,570,032: stereo */
#define CL_COUNT(b)   (((b) + CLUSTER - 1) / CLUSTER)
#define TRACK_CL      CL_COUNT(TRACK_BYTES)                           /* 7761 */
#define TAPE_DIR      2
#define TRACK1        3
#define LAST_CL       (TRACK1 + TRACKS * TRACK_CL - 1)                /* 31046 (the FAT holds 49152 entries) */
#define DOS_DATE      ((46 << 9) | (1 << 5) | 1)
#define MARKER_PATH   "/yaffs2/user/coop_tape.txt"
#define MARKER_TEXT   "coop_tape 1 stereo4\n"

__attribute__((section(".data"))) volatile uint32_t g_coop_tape_image = 1;   /* 0 = never rebuild (stock mount replaced, tables from constants only) */
volatile int32_t g_coop_track_base[TRACKS];       /* file offset of each stereo track's frames */
volatile uint32_t g_tape_io_loads, g_tape_io_flushes, g_tape_io_errors, g_tape_mounts, g_tape_rebuilds, g_tape_rebuild_err;


static inline int32_t track_data_off(int t) { return CL_OFF(TRACK1 + t * TRACK_CL) + AIFC_HDR; }

/* ---- metadata writer (kennel kn_tape_image_meta, stereo layout) ---- */
static uint16_t fat_entry(int32_t c)
{
    if (c < 2 || c > LAST_CL) return 0;
    if (c == TAPE_DIR) return 0xFFFF;
    int32_t first = TRACK1 + (c - TRACK1) / TRACK_CL * TRACK_CL;
    return c == first + TRACK_CL - 1 ? 0xFFFF : (uint16_t)(c + 1);
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void zero(uint8_t *p, int n) { while (n--) *p++ = 0; }
static void cpy(uint8_t *d, const char *s, int n) { while (n--) *d++ = (uint8_t)*s++; }

static void dirent(uint8_t *e, const char *name11, int dir, int32_t cluster, uint32_t size)
{
    zero(e, 32);
    cpy(e, name11, 11);
    e[11] = dir ? 0x10 : 0x20;
    e[12] = name11[0] == '.' ? 0 : dir ? 0x08 : 0x18;
    put16(e + 16, DOS_DATE); put16(e + 18, DOS_DATE); put16(e + 24, DOS_DATE);
    put16(e + 26, (uint32_t)cluster); put32(e + 28, size);
}

static void aifc(uint8_t *h, int channels, uint32_t frames)
{
    static const char desc[] = "Signed integer (little-endian) linear PCM";
    uint32_t data = frames * (uint32_t)channels * 2;
    zero(h, AIFC_HDR);
    cpy(h, "FORM", 4); be32(h + 4, AIFC_HDR + data - 8); cpy(h + 8, "AIFCFVER", 8); be32(h + 0x10, 4);
    be32(h + 0x14, 0xA2805140);
    cpy(h + 0x18, "COMM", 4); be32(h + 0x1C, 0x40);
    h[0x20] = 0; h[0x21] = (uint8_t)channels; be32(h + 0x22, frames); h[0x26] = 0; h[0x27] = 16;
    h[0x28] = 0x40; h[0x29] = 0x0E; h[0x2A] = 0xAC; h[0x2B] = 0x44;
    cpy(h + 0x32, "sowt", 4); h[0x36] = sizeof desc - 1; cpy(h + 0x37, desc, sizeof desc - 1);
    cpy(h + 0x60, "SSND", 4); be32(h + 0x64, data + 8);
}

/* Every nonzero byte of the coop image except the boot sector: both FATs, the root, the TAPE directory, four
 * stereo AIFC headers. The audio stays holes (zeros). Host-testable: coop_tape_image_meta(wr, ctx, buf). */
extern "C" int coop_tape_image_meta(int (*w)(void *ctx, int32_t off, const uint8_t *buf, int32_t len), void *ctx, uint8_t *buf)
{
    int r;
    int32_t fat_used = (LAST_CL + 1) * 2, fat_len = (fat_used + 511) & ~511;
    for (int copy = 0; copy < 2; copy++)
        for (int32_t at = 0; at < fat_len; at += CLUSTER) {
            int32_t n = fat_len - at < CLUSTER ? fat_len - at : CLUSTER;
            for (int32_t i = 0; i < n; i += 2) put16(buf + i, fat_entry((at + i) / 2));
            if ((r = w(ctx, (copy ? FAT2 : FAT1) + at, buf, n))) return r;
        }
    zero(buf, 512);
    dirent(buf, "TAPE       ", 1, TAPE_DIR, 0);
    if ((r = w(ctx, ROOT, buf, 512))) return r;
    zero(buf, 512);
    dirent(buf, ".          ", 1, TAPE_DIR, 0);
    dirent(buf + 32, "..         ", 1, 0, 0);
    static const char tn[] = "TRACK_1 AIF";
    char name[12];
    for (int t = 0; t < TRACKS; t++) {
        for (int i = 0; i < 12; i++) name[i] = tn[i];
        name[6] = (char)('1' + t);
        dirent(buf + 64 + 32 * t, name, 0, TRACK1 + t * TRACK_CL, TRACK_BYTES);
    }
    if ((r = w(ctx, CL_OFF(TAPE_DIR), buf, 512))) return r;
    for (int t = 0; t < TRACKS; t++) {
        aifc(buf, 2, TRACK_FRAMES);
        if ((r = w(ctx, CL_OFF(TRACK1 + t * TRACK_CL), buf, AIFC_HDR))) return r;
    }
    return 0;
}

#ifndef COOP_HOST
static uint8_t g_bounce[FRAMES_PER_CHUNK * 4] __attribute__((aligned(4)));   /* 128 KiB: one stereo chunk; scratch for the writer */

static int wr_image(void *ctx, int32_t off, const uint8_t *buf, int32_t len)
{
    int fd = (int)(intptr_t)ctx;
    return Y_LSEEK(fd, off, 0) == off && Y_WRITE(fd, buf, len) == len ? 0 : -1;
}

static int marker_ok(void)
{
    int fd = Y_OPEN(MARKER_PATH, 0, 0);
    if (fd < 0) return 0;
    char got[32];
    int n = Y_READ(fd, got, sizeof got - 1);
    Y_CLOSE(fd);
    if (n <= 0) return 0;
    got[n] = 0;
    const char *want = MARKER_TEXT;
    for (int i = 0; want[i]; i++) if (got[i] != want[i]) return 0;
    return 1;
}

static int write_marker(void)
{
    int fd = Y_OPEN(MARKER_PATH, 0x242, 0x180);
    if (fd < 0) return -1;
    int len = sizeof MARKER_TEXT - 1;
    int ok = Y_WRITE(fd, MARKER_TEXT, len) == len;
    return Y_CLOSE(fd) == 0 && ok ? 0 : -1;
}

/* Rebuild the image sparse around the open, mounted image (kennel make_tape + mount_tape). 0 ok. */
static int rebuild_image(void)
{
    g_tape_rebuilds++;
    INVALIDATE_ALL();
    FAT_MOUNT(0, 0);
    if (IMG_FD >= 0) Y_CLOSE(IMG_FD);
    IMG_FD = -1;
    int fd = Y_OPEN(IMG_PATH, 0x242, 0x180);                           /* O_RDWR | O_CREAT | O_TRUNC: the old audio becomes holes */
    if (fd < 0) return -1;
    int ok = PRESIZE(fd) >= 0 && Y_LSEEK(fd, 0, 2) == IMG_SIZE;
    if (ok) { BOOT_SECTOR(fd); ok = coop_tape_image_meta(wr_image, (void *)(intptr_t)fd, g_bounce) == 0; }
    if (Y_CLOSE(fd) != 0) ok = 0;
    fd = Y_OPEN(IMG_PATH, 2, 0x180);                                   /* as tape_image_open_at_boot opens it */
    if (fd < 0) return -1;
    IMG_FD = fd;
    SET_FILE(fd);
    DISK_BIND(fd);
    *(uint32_t *)(FAT_FS + 0x2C) = 0xFFFFFFFFu;                        /* the window holds nothing of this image */
    FAT_FS[4] = 0;
    FAT_MOUNT(0, FAT_FS);
    if (!ok) return -1;
    return write_marker();
}

/* Stands in for tape_image_mount_and_index (0x1996f38; the callers ignore the result). */
extern "C" void coop_tape_clamp_track(void);   /* tape_stereo.cc */

extern "C" int coop_tape_mount_and_index(void)
{
    g_tape_mounts++;
    coop_tape_clamp_track();
    if (g_coop_tape_image && !marker_ok()) {
        if (rebuild_image() != 0) g_tape_rebuild_err++;
    }
    for (int t = 0; t < TRACKS; t++) {
        g_coop_track_base[t] = track_data_off(t);
        TRACK_BASE[t] = track_data_off(t);
    }
    ALBUM_A_BASE = track_data_off(0);                                  /* the album code is inert; keep its reads in the file */
    ALBUM_B_BASE = track_data_off(1);
    return 0;
}

/* ---- chunk I/O: tape_cache_io_step (0x1a54710) with lane pairs ---- */
static void deinterleave_n(const uint8_t *src, uint8_t *lane_l, uint8_t *lane_r, int frames)
{
    const uint32_t *s = (const uint32_t *)src;
    uint16_t *l = (uint16_t *)lane_l, *r = (uint16_t *)lane_r;
    for (int i = 0; i < frames; i++) { uint32_t f = s[i]; l[i] = (uint16_t)f; r[i] = (uint16_t)(f >> 16); }
}

static void interleave(uint8_t *dst, const uint8_t *lane_l, const uint8_t *lane_r)
{
    uint32_t *d = (uint32_t *)dst;
    const uint16_t *l = (const uint16_t *)lane_l, *r = (const uint16_t *)lane_r;
    for (int i = 0; i < FRAMES_PER_CHUNK; i++) d[i] = (uint32_t)l[i] | ((uint32_t)r[i] << 16);
}

static inline int32_t chunk_off(int t, int chunk) { return g_coop_track_base[t] + (int32_t)chunk * (FRAMES_PER_CHUNK * 4); }

/* bytes [at, at + len) of track t's chunk through g_bounce */
static int rw_bytes(int write, int t, int chunk, int32_t at, int32_t len)
{
    int fd = CACHE_FD;
    int32_t off = chunk_off(t, chunk) + at;
    if (chunk < 0 || chunk >= CHUNKS) return -1;
    if (Y_LSEEK(fd, off, 0) != off) return -1;
    int n = write ? Y_WRITE(fd, g_bounce, len) : Y_READ(fd, g_bounce, len);
    return n == len ? 0 : -1;
}
static int rw_chunk(int write, int t, int chunk) { return rw_bytes(write, t, chunk, 0, (int32_t)sizeof g_bounce); }

/* Node loads in eight parts (docs/performance.md, T3e): one part = half a track-chunk = 64 KiB of file, the size the
 * stock step moves per read, so a main-loop pass blocks no longer than stock's for a load. The lookup requests the
 * next chunk 0.74 s ahead; eight passes take about 60 ms. The node stays invalid until its last part; the stock
 * cancel protocol (the audio thread sets NODE_LOADING nonzero) is checked at the end as stock does, and a node whose
 * chunk changed under us restarts from part 0. */
#define LOAD_PARTS    (TRACKS * 2)
#define PART_FRAMES   (FRAMES_PER_CHUNK / 2)
static uint8_t *g_load_node;
static int16_t g_load_chunk;
static int g_load_part;

/* Held saves while recording (kennel kn_tape.c, op1r 49): the stock service step flushes the chunk being recorded
 * on every main-loop pass. A node saved less than HOLD_CYCLES ago keeps its flush request while recording and the
 * step goes on to the loads; when recording ends every held chunk goes out in that pass. Power loss while recording
 * loses at most the last HOLD_CYCLES of the take. */
#define HOLD_CYCLES   400000000u                                       /* 1 s at 400 MHz */
#define RECORDING     (*(volatile uint8_t *)0x018C17D8u)               /* d_tape_rec_state: recording byte */
#define NODE_BASE     0x0107d034u
#define NODE_STRIDE   0x28
#define NODE_SLOTS    32
static uint32_t g_saved_at[NODE_SLOTS];
static uint8_t g_saved_once[NODE_SLOTS];
static uint8_t g_was_recording;
volatile uint32_t g_tape_io_held;
volatile uint32_t g_tape_io_step_cycles_max, g_tape_io_load_cycles_max, g_tape_io_flush_cycles_max;   /* CYCLES per step / node load / chunk flush */
volatile uint32_t g_tape_io_step_cycles_win, g_tape_io_load_cycles_win, g_tape_io_flush_cycles_win, g_tape_io_steps_win;
extern "C" void coop_stats_tick(int recording);   /* coop_stats.cc */
static inline void peak(volatile uint32_t *max, volatile uint32_t *win, uint32_t dt) { if (dt > *max) *max = dt; if (dt > *win) *win = dt; }

static inline uint32_t now_cycles(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static inline int node_slot(const uint8_t *node)
{
    uint32_t i = ((uint32_t)(uintptr_t)node - NODE_BASE) / NODE_STRIDE;
    return i < NODE_SLOTS ? (int)i : -1;
}

/* R0 = mode (0: the main loop's service step, nonzero: the shutdown flush). Returns 1 if it did a chunk. */
static int io_step(int mode);
extern "C" int coop_tape_io_step(int mode)
{
    uint32_t t0 = now_cycles();
    int r = io_step(mode);
    peak(&g_tape_io_step_cycles_max, &g_tape_io_step_cycles_win, now_cycles() - t0);
    g_tape_io_steps_win++;
    return r;
}

static int io_step(int mode)
{
    coop_tape_clamp_track();
    int recording = RECORDING != 0 && mode == 0;
    if (mode == 0) coop_stats_tick(recording);
    int flush_all = !recording && g_was_recording;                     /* the take ended (or a forced flush): nothing held */
    g_was_recording = (uint8_t)recording;
    uint32_t now = now_cycles();
    for (uint8_t *node = CACHE_HEAD; node; node = *(uint8_t **)(node + NODE_NEXT)) {
        uint8_t *data = *(uint8_t **)(node + NODE_DATA);
        int16_t chunk = *(int16_t *)(node + NODE_CHUNK);
        if (node[NODE_FLUSHREQ]) {                                     /* flush: every dirty lane pair */
            int slot = node_slot(node);
            if (recording && !flush_all && slot >= 0 && g_saved_once[slot] && now - g_saved_at[slot] < HOLD_CYCLES) {
                g_tape_io_held++;
                continue;                                              /* held: the request stays, loads may go on */
            }
            if (slot >= 0) { g_saved_at[slot] = now; g_saved_once[slot] = 1; }
            node[NODE_FLUSHREQ] = 0;
            for (int t = 0; t < TRACKS; t++) {
                uint16_t bits = (uint16_t)((1u << t) | (1u << (TRACKS + t)));   /* both passes set bit t; 4+t is legacy */
                if (!(*(uint16_t *)(node + NODE_DIRTY) & bits)) continue;
                *(uint16_t *)(node + NODE_DIRTY) &= (uint16_t)~bits;  /* cleared before the write, as stock */
                uint32_t f0 = now_cycles();
                interleave(g_bounce, data + t * LANE_BYTES, data + (TRACKS + t) * LANE_BYTES);
                if (rw_chunk(1, t, chunk) != 0) g_tape_io_errors++;
                g_tape_io_flushes++;
                peak(&g_tape_io_flush_cycles_max, &g_tape_io_flush_cycles_win, now_cycles() - f0);
            }
            return 1;
        }
        if (mode != 0) return 0;
        if (chunk < 0 || node[NODE_VALID]) continue;
        if (g_load_node && g_load_node != node && !g_load_node[NODE_VALID]
            && *(int16_t *)(g_load_node + NODE_CHUNK) == g_load_chunk) {  /* finish the node in progress first */
            node = g_load_node;
            data = *(uint8_t **)(node + NODE_DATA);
            chunk = g_load_chunk;
        }
        if (node != g_load_node || chunk != g_load_chunk) {
            g_load_node = node; g_load_chunk = chunk; g_load_part = 0;
            node[NODE_LOADING] = (uint8_t)mode;                        /* 0: a load in progress may be cancelled by setting it */
        }
        int t = g_load_part >> 1, half = g_load_part & 1;
        int32_t at = half * (PART_FRAMES * 4);
        uint32_t p0 = now_cycles();
        if (rw_bytes(0, t, chunk, at, PART_FRAMES * 4) != 0) { g_tape_io_errors++; zero(g_bounce, PART_FRAMES * 4); }
        deinterleave_n(g_bounce, data + t * LANE_BYTES + half * (LANE_BYTES / 2),
                       data + (TRACKS + t) * LANE_BYTES + half * (LANE_BYTES / 2), PART_FRAMES);
        peak(&g_tape_io_load_cycles_max, &g_tape_io_load_cycles_win, now_cycles() - p0);   /* per part: what a pass blocks for */
        if (++g_load_part < LOAD_PARTS) return 1;
        g_load_node = 0;
        g_tape_io_loads++;
        if (node[NODE_LOADING] == 0) {
            node[NODE_VALID] = *(int16_t *)(node + NODE_CHUNK) == chunk ? 1 : 0;
        } else {
            node[NODE_VALID] = 0;
        }
        return 1;
    }
    return 0;
}
#endif
