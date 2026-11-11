/*
 * Four tapes (docs/tapes.md): switch R246's tape between tape 1 (TE's: /yaffs2/user/tape_c.raw, tape.db) and tapes
 * 2..4 (/yaffs2/user/tape<N>/...), each with its own tracks, clips, loop, tempo and album. Requested from any thread
 * (op1_tape_switch), done on the main loop (kn_tapes_service, from ci_service), which owns the tape file and cache.
 * A new tape is a sparse image: only its FAT16 metadata and AIFC headers are written (kn_tape_image_meta), the audio
 * stays a YAFFS hole that reads as silence. Boot is always tape 1 (stock's start-up and marker recovery only ever see
 * tape 1). R246 addresses: docs/tapes.md.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_feature_tape.h"
#include "../op1_synth.h"                                     /* op1_db_write */

void kn_event(int id, int32_t arg);
void *kn_alloc(uint32_t size);

/* ---- image layout (R246's, OP1-Emulator docs/firmware/tape.md) ---- */
#define IMG_SIZE      0x18000000
#define FAT1          0x1000
#define FAT2          0x19000
#define FAT_BYTES     0x18000                     /* 192 sectors */
#define ROOT          0x31000
#define DATA          0x35000
#define CLUSTER       0x2000
#define CL_OFF(c)     (DATA + ((int32_t)(c) - 2) * CLUSTER)
#define TRACK_FRAMES  0xF28000
#define SIDE_FRAMES   0xF23FA0
#define TRACK_BYTES   (0x70 + TRACK_FRAMES * 2)  /* 31,785,072 */
#define SIDE_BYTES    (0x70 + SIDE_FRAMES * 4)   /* 63,504,112 */
#define CL_COUNT(b)   (((b) + CLUSTER - 1) / CLUSTER)
#define TAPE_DIR      2
#define TRACK1        3
#define ALBUM_DIR     (TRACK1 + 4 * CL_COUNT(TRACK_BYTES))      /* 15527 */
#define SIDE_A        (ALBUM_DIR + 1)
#define LAST_CL       (SIDE_A + 2 * CL_COUNT(SIDE_BYTES) - 1)   /* 31031 */
#define DOS_TIME      0x0000
#define DOS_DATE      ((46 << 9) | (1 << 5) | 1)               /* 2026-01-01 (R246 writes the clock; nobody reads it) */

/* FAT16 entry of cluster c: contiguous chains, 0xFFFF at each end, entries 0 and 1 zero as R246 leaves them */
static uint16_t fat_entry(int32_t c)
{
    if (c < 2 || c > LAST_CL) return 0;
    if (c == TAPE_DIR || c == ALBUM_DIR) return 0xFFFF;
    int32_t first, n;
    if (c < ALBUM_DIR) { n = CL_COUNT(TRACK_BYTES); first = TRACK1 + (c - TRACK1) / n * n; }
    else { n = CL_COUNT(SIDE_BYTES); first = SIDE_A + (c - SIDE_A) / n * n; }
    return c == first + n - 1 ? 0xFFFF : (uint16_t)(c + 1);
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
    e[12] = name11[0] == '.' ? 0 : dir ? 0x08 : 0x18;          /* NT case: lowercase name (and extension) */
    put16(e + 14, DOS_TIME); put16(e + 16, DOS_DATE); put16(e + 18, DOS_DATE);
    put16(e + 22, DOS_TIME); put16(e + 24, DOS_DATE);
    put16(e + 26, (uint32_t)cluster); put32(e + 28, size);
}

/* the AIFC header (0x70 bytes): little-endian PCM ('sowt'), 44.1 kHz, 16 bit */
static void aifc(uint8_t *h, int channels, uint32_t frames)
{
    static const char desc[] = "Signed integer (little-endian) linear PCM";
    uint32_t data = frames * (uint32_t)channels * 2;
    zero(h, 0x70);
    cpy(h, "FORM", 4); be32(h + 4, 0x70 + data - 8); cpy(h + 8, "AIFCFVER", 8); be32(h + 0x10, 4);
    be32(h + 0x14, 0xA2805140);                                 /* AIFC version 1 */
    cpy(h + 0x18, "COMM", 4); be32(h + 0x1C, 0x40);
    h[0x20] = 0; h[0x21] = (uint8_t)channels; be32(h + 0x22, frames); h[0x26] = 0; h[0x27] = 16;
    h[0x28] = 0x40; h[0x29] = 0x0E; h[0x2A] = 0xAC; h[0x2B] = 0x44;   /* 44100 as an 80-bit float */
    cpy(h + 0x32, "sowt", 4); h[0x36] = sizeof desc - 1; cpy(h + 0x37, desc, sizeof desc - 1);
    cpy(h + 0x60, "SSND", 4); be32(h + 0x64, data + 8);
}

/* Writes every non-zero byte of a fresh image except the boot sector (R246's fat_image_write_boot_template writes
 * it): both FATs, the root directory, the TAPE and ALBUM directories, the six AIFC headers. wr(off, buf, len); buf
 * is scratch of 8 KiB. 0 or the first nonzero wr result. */
int kn_tape_image_meta(int (*wr)(void *ctx, int32_t off, const uint8_t *buf, int32_t len), void *ctx, uint8_t *buf)
{
    int r;
    int32_t fat_used = (LAST_CL + 1) * 2, fat_len = (fat_used + 511) & ~511;
    for (int copy = 0; copy < 2; copy++)
        for (int32_t at = 0; at < fat_len; at += CLUSTER) {
            int32_t n = fat_len - at < CLUSTER ? fat_len - at : CLUSTER;
            for (int32_t i = 0; i < n; i += 2) put16(buf + i, fat_entry((at + i) / 2));
            if ((r = wr(ctx, (copy ? FAT2 : FAT1) + at, buf, n))) return r;
        }
    zero(buf, 512);                                            /* root: TAPE, ALBUM */
    dirent(buf, "TAPE       ", 1, TAPE_DIR, 0);
    dirent(buf + 32, "ALBUM      ", 1, ALBUM_DIR, 0);
    if ((r = wr(ctx, ROOT, buf, 512))) return r;
    zero(buf, 512);                                            /* TAPE: ., .., four tracks */
    dirent(buf, ".          ", 1, TAPE_DIR, 0);
    dirent(buf + 32, "..         ", 1, 0, 0);
    static const char tn[] = "TRACK_1 AIF";
    char name[12];
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 12; i++) name[i] = tn[i];
        name[6] = (char)('1' + t);
        dirent(buf + 64 + 32 * t, name, 0, TRACK1 + t * CL_COUNT(TRACK_BYTES), TRACK_BYTES);
    }
    if ((r = wr(ctx, CL_OFF(TAPE_DIR), buf, 512))) return r;
    zero(buf, 512);                                            /* ALBUM: ., .., two sides */
    dirent(buf, ".          ", 1, ALBUM_DIR, 0);
    dirent(buf + 32, "..         ", 1, 0, 0);
    dirent(buf + 64, "SIDE_A  AIF", 0, SIDE_A, SIDE_BYTES);
    dirent(buf + 96, "SIDE_B  AIF", 0, SIDE_A + CL_COUNT(SIDE_BYTES), SIDE_BYTES);
    if ((r = wr(ctx, CL_OFF(ALBUM_DIR), buf, 512))) return r;
    for (int t = 0; t < 4; t++) {
        aifc(buf, 1, TRACK_FRAMES);
        if ((r = wr(ctx, CL_OFF(TRACK1 + t * CL_COUNT(TRACK_BYTES)), buf, 0x70))) return r;
    }
    for (int s = 0; s < 2; s++) {
        aifc(buf, 2, SIDE_FRAMES);
        if ((r = wr(ctx, CL_OFF(SIDE_A + s * CL_COUNT(SIDE_BYTES)), buf, 0x70))) return r;
    }
    return 0;
}

#ifndef KN_TAPES_META_ONLY                         /* harness/tapes_image_test.c builds the writer alone */
/* ---- R246 ---- */
#ifdef OP1_HOST                                    /* harness/tapes_test.c */
#include "../harness/tapes_host.h"
#else
#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE       ((int (*)(int))0x01A99908)
#define Y_READ        ((int (*)(int, void *, int))0x01A99C90)
#define Y_WRITE       ((int (*)(int, const void *, int))0x01A99F84)
#define Y_LSEEK       ((int32_t (*)(int, int32_t, int))0x01A9A0E4)
#define Y_UNLINK      ((int (*)(const char *))0x01A9A3FC)
#define Y_MKDIR       ((int (*)(const char *, int))0x01A9A886)
#define Y_ACCESS      ((int (*)(const char *, int))0x01A9A6B0)
#define Y_FREE()      ((uint32_t)((int (*)(const char *))0x01A9B1C0)("/yaffs2"))
#define DELAY_MS      ((void (*)(int))0x0193E5C0)
#define IMG_PATH      ((char *)0x01AABCE4)                             /* tape_c.raw path (0x200) */
#define MARKER_PATH   ((char *)0x01AAC934)                             /* marker.txt path (0x200) */
#define IMG_FD        (*(volatile int32_t *)0x018C180C)               /* tape_image_fat_handle */
#define SET_FILE      ((void (*)(int))0x01A5404C)                     /* tape_set_file_handle */
#define PRESIZE       ((int (*)(int))0x01A99FB4)                      /* tape_image_presize: 0x18000000 bytes */
#define BOOT_SECTOR   ((void (*)(int))0x019988C8)                     /* fat_image_write_boot_template */
#define DISK_BIND     ((void (*)(int))0x019761D8)                     /* fat_disk_bind_tape_image */
#define FAT_MOUNT     ((int (*)(int, void *))0x01998E74)              /* f_mount(volume, fs); fs 0 unmounts */
#define FAT_FS        ((uint8_t *)0x01AABEE4)                      /* FatFs volume: +1 drive, +3 FATs, +4 window dirty,
                                                                       * +0x18 FAT size, +0x20 FAT start, +0x2C window
                                                                       * sector, +0x30 window (move_window 0x0199A800) */
#define DISK_WRITE    ((int (*)(int, const void *, uint32_t, int))0x01976288)   /* (drive, buf, sector, count) */
#define MOUNT_INDEX   ((void (*)(void))0x01996F38)                    /* tape_image_mount_and_index */
#define INVALIDATE_ALL ((void (*)(void))0x01A54078)                   /* tape_cache_invalidate_clean */
#define DB_BOX        (*(void **volatile *)0x01BAB72C)                /* tape.db: the box db2 calls take */
#define DB_LOCK       (*(void *volatile *)0x01BAB718)                 /* db2 statement lock */
#define SEM_PEND      ((void (*)(void *, int))0xFFA0B6E8)
#define SEM_POST      ((void (*)(void *))0xFFA0B96C)
#define SQL_CLOSE     ((int (*)(void *))0x019D6134)
#define SQL_OPEN      ((int (*)(const char *, void **, int, const char *))0x019D73C8)   /* open_v2: VFS 0 */
#define DB_GET        ((void (*)(void *, void *, const char *))0x0196FBD0)
#define BG_HEAD       (*(volatile int32_t *)0x01BA33E4)
#define BG_TAIL       (*(volatile int32_t *)0x01BA33E8)
#define BG_JOB        (*(void *volatile *)0x01BA29D8)
#define CLIPS_INST    ((void *(*)(void))0x0195BBB4)
#define CLIPS_HAS     ((uint8_t *)0x01AAA570)
#define CLIPS_FREE    (*(volatile int32_t *)0x01AAA770)
#define CLIPS_LOADED  (*(volatile uint8_t *)0x01AA7368)
#define CLIPS_LOAD    ((void (*)(void *))0x0195CE88)
#define CLIPS_REMOVE  ((void (*)(void *, int))0x0195D688)             /* tape_clips_remove_channel(pool, track) */
#define TRACK_REMADE  ((volatile uint8_t *)0xFF906EFC)                /* set by the mount check per recreated track */
#define TAPE_RESET    ((void (*)(void *))0x01A56CD8)                  /* loop off, track 0, position 0, saved */
#define CTRL          ((void *(*)(void))0x01A56D94)                   /* tape_controller_instance */
#define TAPE_SAVE     ((void (*)(void))0x01A569BC)                    /* tape_db_save */
#define LOOP_ON       (*(volatile uint8_t *)0xFF8029C5)
#define LOOP_EFF      (*(volatile uint8_t *)0xFF8029C6)
#define SET_LOOP_IN   ((void (*)(int32_t))0x01A550A8)
#define SET_LOOP_OUT  ((void (*)(int32_t))0x01A55100)
#define CUR_TRACK     (*(volatile int32_t *)0xFF802964)
#define LOCATE        ((void (*)(int32_t))0x01A55024)
#define TAPE_STATE    (*(volatile int32_t *)0x018C17D4)
#define TAPE_RUN      (*(volatile int32_t *)0xFF802960)               /* 1 recording */
#define ALBUM_STATE   (*(volatile int32_t *)0xFF9002B4)               /* 6 / 7 recording */
#define ALBUM_REQ     ((void (*)(int))0x01915590)                     /* 0 stops */
#define DISK_ACTIVE   (*(volatile uint8_t *)0xFF906EF9)
#define CLIP_CONS     (*(volatile int32_t *)0x01AA7330)
#define CLIP_PROD     (*(volatile int32_t *)0x01AA7334)
#define DISPATCH_KEY  ((void (*)(int, int))0x019B6C74)                /* KeyHandler::dispatch_key */
#define TEMPO_CTRL    ((void *(*)(void))0x01A5ED44)                   /* tempo_controller_get */
#define KNOB_BPM10    ((volatile int32_t *)0x01B60E18)                /* tempobpmdecimal: value +0x10 = v * scale +0xC */
#define KNOB_BPM      ((void *)0x01B60D98)
#define KNOB_SET      ((void (*)(void *, int32_t))0x019696E8)
#define SET_BPM       ((void (*)(float))0x01A5E9A4)
#endif

#define NEW_TAPE_FREE (64u << 20)                  /* a new tape needs 64 MB free */
static const char USER[] = "/yaffs2/user/";

typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } dstr;   /* db2_get's string */

static volatile int8_t req = -1;                  /* tape asked for, -1 none */
static int8_t cur;                                /* 0..3 */
static int8_t stop_wait;                          /* passes left waiting for STOP */
static uint8_t *scratch;

int op1_tape_current(void) { return cur; }

/* "/yaffs2/user/" + "tape<N>/" (tapes 2..4) + name */
static void tape_path(char *o, int tape, const char *name)
{
    const char *s = USER;
    while (*s) *o++ = *s++;
    if (tape) { const char *d = "tape"; while (*d) *o++ = *d++; *o++ = (char)('1' + tape); *o++ = '/'; }
    while (*name) *o++ = *name++;
    *o = 0;
}
static int exists(int tape, const char *name) { char p[64]; tape_path(p, tape, name); return Y_ACCESS(p, 0) == 0; }

static int write_file(const char *path, const void *data, int len)
{
    int fd = Y_OPEN(path, 0x242, 0x180);
    if (fd < 0) return -1;
    int ok = Y_WRITE(fd, data, len) == len;
    return Y_CLOSE(fd) == 0 && ok ? 0 : -1;
}

static int wr_image(void *ctx, int32_t off, const uint8_t *buf, int32_t len)
{
    int fd = (int)(long)ctx;
    return Y_LSEEK(fd, off, 0) == off && Y_WRITE(fd, buf, len) == len ? 0 : -1;
}

/* a new tape (1..3): sparse image, the factory tape.db, marker "0", then `ready` */
static int make_tape(int tape)
{
    char p[64], q[64];
    tape_path(p, tape, "");
    p[sizeof USER - 1 + 5] = 0;                                     /* "/yaffs2/user/tape<N>" */
    Y_MKDIR(p, 0);
    tape_path(p, tape, "ready"); Y_UNLINK(p);
    tape_path(p, tape, "tape_c.raw");
    int fd = Y_OPEN(p, 0x242, 0x180);
    if (fd < 0) return -1;
    int ok = PRESIZE(fd) >= 0 && Y_LSEEK(fd, 0, 2) == IMG_SIZE;
    if (ok) { BOOT_SECTOR(fd); ok = kn_tape_image_meta(wr_image, (void *)(long)fd, scratch) == 0; }
    if (Y_CLOSE(fd) != 0) ok = 0;
    if (!ok) return -1;
    int in = Y_OPEN("/yaffs2/content/tape.db", 0, 0);              /* the factory copy (4 KiB) */
    if (in < 0) return -1;
    int n = Y_READ(in, scratch, 0x2000);
    Y_CLOSE(in);
    tape_path(q, tape, "tape.db");
    if (n <= 0 || n >= 0x2000 || write_file(q, scratch, n)) return -1;
    tape_path(q, tape, "marker.txt");
    if (write_file(q, "0", 1)) return -1;
    tape_path(q, tape, "ready");
    return write_file(q, "1", 1);
}

/* a tape whose marker is above '0' (a power cut during its defragmentation) is made again */
static int marker_dirty(int tape)
{
    char p[64]; uint8_t c = '0';
    tape_path(p, tape, "marker.txt");
    int fd = Y_OPEN(p, 0, 0);
    if (fd < 0) return 0;
    Y_READ(fd, &c, 1);
    Y_CLOSE(fd);
    return c > '0';
}

/* ---- tape.db: the tape row and the tempo ---- */
static int32_t db_int(const char *sql, int32_t def)
{
    dstr s; zero((uint8_t *)&s, sizeof s);
    DB_GET(&s, DB_BOX, sql);
    if (!s.size) return def;
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    int32_t v = 0, neg = 0, any = 0;
    if (*p == '-') { neg = 1; p++; }
    for (uint32_t i = 0; i < s.size && *p >= '0' && *p <= '9'; i++, p++) { v = v * 10 + (*p - '0'); any = 1; }
    return any ? (neg ? -v : v) : def;
}
static const char KN_TABLE[] = "CREATE TABLE IF NOT EXISTS kn_tape (id INTEGER PRIMARY KEY, bpm10 INTEGER)";
static int32_t bpm10_now(void)
{
    TEMPO_CTRL();
    return KNOB_BPM10[4] / (KNOB_BPM10[3] ? KNOB_BPM10[3] : 1);     /* value / scale */
}
static void save_bpm(void)
{
    char sql[64], *o = sql;
    const char *h = "INSERT OR REPLACE INTO kn_tape VALUES (0,";
    while (*h) *o++ = *h++;
    int32_t v = bpm10_now(); char d[8]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 7);
    while (k) *o++ = d[--k];
    *o++ = ')'; *o = 0;
    op1_db_write(DB_BOX, KN_TABLE);
    op1_db_write(DB_BOX, sql);
}
int32_t op1_tape_bpm10(void) { return bpm10_now(); }

static void apply_bpm(int32_t bpm10)
{
    if (bpm10 < 400 || bpm10 > 2200) return;
    TEMPO_CTRL();
    KNOB_SET((void *)KNOB_BPM10, bpm10);                            /* as the tempo screen's fine knob does */
    KNOB_SET(KNOB_BPM, bpm10 / 10);
    SET_BPM((float)bpm10 / 10.0f);
}

/* ---- the switch ---- */
static int busy_reason(void)
{
    if (TAPE_RUN == 1 || ALBUM_STATE == 6 || ALBUM_STATE == 7) return OP1_TAPE_SW_RECORDING;
    if (DISK_ACTIVE) return OP1_TAPE_SW_DISK;
    if (CLIP_CONS != CLIP_PROD) return OP1_TAPE_SW_BUSY;
    return 0;
}

int op1_tape_switch(int tape)
{
    if (tape < 0 || tape > 3) return OP1_TAPE_SW_BAD;
    if (tape == cur) return OP1_TAPE_SW_SAME;
    int r = busy_reason();
    if (r) return r;
    if (!exists(tape, "ready") && tape && Y_FREE() < NEW_TAPE_FREE) return OP1_TAPE_SW_FULL;
    req = (int8_t)tape;
    stop_wait = 100;
    return 0;
}

static int bg_idle(void) { return BG_HEAD == BG_TAIL && !BG_JOB; }

static void set_paths(int tape)
{
    tape_path(IMG_PATH, tape, "tape_c.raw");
    tape_path(MARKER_PATH, tape, "marker.txt");
}

/* open tape `tape`'s image and mount it as R246's start-up does; 0 ok */
static int mount_tape(int tape)
{
    set_paths(tape);
    int fd = Y_OPEN(IMG_PATH, 2, 0x180);
    if (fd < 0) return -1;
    IMG_FD = fd;
    SET_FILE(fd);
    DISK_BIND(fd);
    *(uint32_t *)(FAT_FS + 0x2C) = 0xFFFFFFFF;     /* the window holds nothing of this image */
    FAT_FS[4] = 0;
    FAT_MOUNT(0, FAT_FS);
    MOUNT_INDEX();                                  /* checks the six files, data offsets */
    return 0;
}
/* FatFs keeps one sector window per volume (move_window): a dirty one belongs to the image still bound, and its
 * sector number survives a remount (the mount reads the new boot sector into the window without updating it), so a
 * later read of that sector would get the old bytes. Write a dirty window back now; forget its sector before the next
 * mount. */
static void fat_window_flush(void)
{
    uint8_t *fs = FAT_FS;
    if (!fs[4]) return;
    uint32_t s = *(uint32_t *)(fs + 0x2C), fsize = *(uint32_t *)(fs + 0x18), fat = *(uint32_t *)(fs + 0x20);
    DISK_WRITE(fs[1], fs + 0x30, s, 1);
    if (s < fat + fsize)                            /* a FAT sector: the other copies too */
        for (int n = fs[3]; n > 1; n--) { s += fsize; DISK_WRITE(fs[1], fs + 0x30, s, 1); }
    fs[4] = 0;
}
static void unmount_tape(void)
{
    INVALIDATE_ALL();                               /* every node is clean after the flush: all dropped */
    fat_window_flush();                             /* into the old image, while it is still bound */
    FAT_MOUNT(0, 0);
    if (IMG_FD >= 0) Y_CLOSE(IMG_FD);
    IMG_FD = -1;
}

/* tape.db of `tape` into the box db2 uses, under the statement lock; 0 ok (on failure the old one is open again) */
static int swap_db(int from, int to)
{
    char p[64];
    void *lock = DB_LOCK, **box = DB_BOX;
    if (lock) SEM_PEND(lock, 0);
    int r = SQL_CLOSE(*box);
    if (r == 0) {
        tape_path(p, to, "tape.db");
        r = SQL_OPEN(p, box, 2, 0);
        if (r) { tape_path(p, from, "tape.db"); SQL_OPEN(p, box, 2, 0); }
    }
    if (lock) SEM_POST(lock);
    return r;
}

static void load_tape_state(int fresh)
{
    void *pool = CLIPS_INST();
    for (int i = 0; i < 512; i++) CLIPS_HAS[i] = 0;
    CLIPS_FREE = 512;
    CLIPS_LOADED = 0;
    CLIPS_LOAD(pool);
    int remade = 0;
    for (int t = 0; t < 4; t++)
        if (TRACK_REMADE[t]) { CLIPS_REMOVE(pool, t); TRACK_REMADE[t] = 0; remade = 1; }
    LOOP_ON = LOOP_EFF = (uint8_t)(db_int("SELECT loop FROM tape", 0) != 0);
    SET_LOOP_IN(db_int("SELECT loop_in FROM tape", 0));
    SET_LOOP_OUT(db_int("SELECT loop_out FROM tape", 0));
    int32_t tr = db_int("SELECT track FROM tape", 0);
    CUR_TRACK = tr >= 0 && tr < 4 ? tr : 0;
    LOCATE(db_int("SELECT pos FROM tape", 0));
    if (remade) TAPE_RESET(CTRL());
    op1_db_write(DB_BOX, KN_TABLE);
    if (fresh) save_bpm();                          /* a new tape starts at the tempo of this moment */
    else apply_bpm(db_int("SELECT bpm10 FROM kn_tape WHERE id=0", 0));
}

static void do_switch(int to)
{
    int from = cur, fresh = 0;
    TAPE_SAVE();                                    /* loop, loop points, track, position of the old tape */
    op1_db_write(DB_BOX, KN_TABLE);
    save_bpm();
    for (int i = 0; i < 200 && !bg_idle(); i++) DELAY_MS(10);
    if (!bg_idle()) { op1_toast("tape busy, try again"); return; }
    if (op1_tape_flush() < 0) { op1_toast("tape still writing"); return; }
    if (to && (!exists(to, "ready") || marker_dirty(to))) {
        if (!scratch) scratch = kn_alloc(0x2000);
        if (!scratch || make_tape(to)) { op1_toast("could not make the tape"); return; }
        fresh = 1;
    }
    kn_event(OP1_EV_TAPE_SWITCH, from | to << 8 | 1 << 16);
    if (swap_db(from, to)) { op1_toast("could not open the tape"); kn_event(OP1_EV_TAPE_SWITCH, from | from << 8); return; }
    unmount_tape();
    if (mount_tape(to)) {                           /* the image is there (checked or just made): not expected */
        swap_db(to, from);
        mount_tape(from);
        load_tape_state(0);
        op1_toast("could not open the tape");
        kn_event(OP1_EV_TAPE_SWITCH, from | from << 8);
        return;
    }
    cur = (int8_t)to;
    load_tape_state(fresh);
    kn_event(OP1_EV_TAPE_SWITCH, from | to << 8);
}

/* main loop (ci_service) */
void kn_tapes_service(void)
{
    int to = req;
    if (to < 0) return;
    if (busy_reason()) { req = -1; return; }        /* recording / disk mode started meanwhile: dropped */
    if (TAPE_STATE != 0 && TAPE_STATE != 3) {       /* playing or armed: STOP first, then a later pass */
        if (stop_wait == 100) { DISPATCH_KEY(0x23, 1); DISPATCH_KEY(0x23, 0); }
        if (--stop_wait <= 0) req = -1;
        return;
    }
    if (ALBUM_STATE >= 2 && ALBUM_STATE <= 4) { ALBUM_REQ(0); return; }   /* album playing: stop, next pass */
    req = -1;
    do_switch(to);
}

/* flash left (bytes); REC's flash check */
uint32_t op1_flash_free(void) { return Y_FREE(); }
#endif
