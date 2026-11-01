/* Development update, firmware side: apply phase at boot (docs/35). Built always, hooked only with
 * make_firmware.py --dev: the boot's CALL access("/yaffs2/te-boot.op1") (0x019F01B8, after mount_yaffs, before USB /
 * audio / GUI = single-threaded) goes to du_boot_access, which applies a staged update first.
 * Apply: manifest valid -> every staged file's size + CRC checked, the LDR's whole block chain and the R246
 * fingerprint checked (any failure: staging removed, nothing applied) -> content files copied to their final paths
 * -> OP1_vdk.ldr flashed to the boot area from block 4 with the stock
 * primitives in the stock order (erase 4..131 = 0x019EABB4, write_boot_block per block), every block number passing
 * du_guard (logical and physical after the bad-block mapping) -> staging removed -> "restart" (as the stock updater).
 * Never called: 0x019EAC08 (erases te-boot), flash_te_boot; paths with te-boot are refused by du_path_ok. */
#include "du.h"
#include "../kn_mem.h"                                         /* kn_farbss: the big tables live in .farbss */

#define Y_OPEN    ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE   ((int (*)(int))0x01A99908)
#define Y_READ    ((int (*)(int, void *, unsigned))0x01A99C90)
#define Y_LSEEK  ((int (*)(int, int, int))0x01A9A0E4)
#define Y_WRITE   ((int (*)(int, const void *, unsigned))0x01A99F84)
#define Y_UNLINK  ((int (*)(const char *))0x01A9A3FC)
#define Y_ACCESS  ((int (*)(const char *, int))0x01A9A6B0)        /* 0 = exists */
#define Y_ERROR   ((int (*)(void))0x01A98B24)
#define Y_MKDIR   ((int (*)(const char *, int))0x01A9A888)
#define O_READ    0
#define O_CREATE  0x241                                           /* O_WRONLY | O_CREAT | O_TRUNC (as the firmware) */
#define NEW_ARR   ((void *(*)(unsigned))0x01AA370C)
#define DEL_ARR   ((void (*)(void *))0x01AA2B04)
#define CONSOLE   ((void (*)(const char *))0x019D2BF0)            /* boot console line (the stock updater's) */
#define NAND_WAIT ((void (*)(void))0x01A5E454)                    /* as flash_firmware 0x019A1B48 */
#define NAND_LOCK ((void (*)(int))0x019E9DE4)
#define NAND_ERASE_MAIN ((void (*)(void))0x019EABB4)              /* erases blocks 4..131 only (checked) */
#define NAND_WRITE_BLOCK ((int (*)(int, const void *))0x019EAAAC) /* logical block, 128 KB; 0 ok */
#define UNMOUNT   ((void (*)(void))0x019A5F20)                    /* unmount_yaffs: as the stock end of a te-boot update */
#define DELAY_MS  ((void (*)(int))0x0193E5C0)
#define WDOG_CTL  (*(volatile uint16_t *)0xFFC00200)
#define WDOG_CNT  (*(volatile uint32_t *)0xFFC00204)
#define BAD_COUNT (*(volatile int *)0xFF907D94)
#define BAD_LIST  ((volatile int *)0x01DD94CC)

static int mapped(int logical)                                    /* write_boot_block's logical -> physical */
{
    int p = logical, n = BAD_COUNT;
    for (int i = 0; i < n && i < 128; i++) if (BAD_LIST[i] <= p) p++;
    return p;
}

int du_fw_flash(const uint8_t *ldr, uint32_t size)
{
    int c = du_ldr_check(ldr, size, du_r246_fp, du_r246_fp_n);    /* (apply checked it too: nothing changes on failure) */
    if (c) return c;
    int nb = (int)((size + DU_BLOCK - 1) / DU_BLOCK);
    uint32_t tail = size - (uint32_t)(nb - 1) * DU_BLOCK;
    int need = du_blocks_needed(size);
    if (!du_guard(DU_FIRST_BLOCK + need - 1)) return DU_F_NOSPACE;
    uint8_t *last = NEW_ARR(DU_BLOCK);
    if (!last) return DU_F_WRITE;
    /* point of no return: the stock routine erases 4..131 and builds the bad-block list from blocks that fail */
    NAND_WAIT(); NAND_LOCK(0); NAND_ERASE_MAIN(); NAND_LOCK(1);
    int r = DU_F_OK;
    for (int i = 0; i < need; i++)                                 /* plan with that list before the first write */
        if (!du_guard(mapped(DU_FIRST_BLOCK + i))) r = DU_F_NOSPACE;
    for (int i = 0; i < need && r == DU_F_OK; i++) {
        int lb = DU_FIRST_BLOCK + i;
        if (!du_guard(lb) || !du_guard(mapped(lb))) { r = DU_F_GUARD; break; }   /* write retries can add bad blocks */
        const uint8_t *src = ldr + (uint32_t)i * DU_BLOCK;
        if (i >= nb - 1) {                                         /* last data block / 0xFF end block */
            uint32_t len = i == nb - 1 ? tail : 0;
            for (uint32_t k = 0; k < DU_BLOCK; k++) last[k] = k < len ? src[k] : 0xFF;
            src = last;
        }
        NAND_WAIT(); NAND_LOCK(0);
        if (NAND_WRITE_BLOCK(lb, src)) r = DU_F_WRITE;            /* (writes, reads back, retries itself) */
        NAND_LOCK(1);
    }
    DEL_ARR(last);
    return r;
}

static uint8_t io[4096] kn_farbss;
static int file_crc(const char *path, uint32_t size, uint32_t want)
{
    int fd = Y_OPEN(path, O_READ, 0x100);
    if (fd < 0) return 0;
    uint32_t got = 0, c = 0; int ok = 1;
    for (;;) {
        int n = Y_READ(fd, io, sizeof io);
        if (n <= 0) { if (n < 0) ok = 0; break; }
        c = du_crc32(c, io, (uint32_t)n); got += (uint32_t)n;
    }
    if (Y_CLOSE(fd)) ok = 0;
    return ok && got == size && c == want;
}

static void cat(char *o, const char *a, const char *b) { int k = 0; while (*a) o[k++] = *a++; while (*b) o[k++] = *b++; o[k] = 0; }

/* Backups live outside the transfer directory: ABORT must never erase recovery. */
#define BACKUP_DIR "/yaffs2/kennel_recovery"
#define JOURNAL BACKUP_DIR "/journal"
#define OLD_MANIFEST BACKUP_DIR "/manifest"
#define FAILED BACKUP_DIR "/failed"
typedef struct { uint32_t exists, size, crc; } old_file;
static struct { uint32_t magic, count, core_len, core_crc, crc; old_file files[DU_MAX_FILES]; } recovery kn_farbss;
#define originals recovery.files
extern const uint8_t _core_ro_end[];
static int original_core(void)
{
    return recovery.core_len && recovery.core_len < 0x40000 &&
        du_crc32(0, (const uint8_t *)0x01E00000u, recovery.core_len) == recovery.core_crc;
}
/* Recovery may run after a different boot-menu install. A different core is
 * not by itself evidence that our staged firmware was flashed. Compare every
 * loaded block overlapping this core's read-only image, rejecting encrypted/fill
 * overlaps and later overrides. File reads are boot-only and bounded. */
static uint32_t image_u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int matches_running_core(int fd, const uint8_t *image, uint32_t size)
{
    uint32_t len=(uint32_t)((uintptr_t)_core_ro_end - 0x01E00000u);
    const uint8_t *core=(const uint8_t *)0x01E00000u;
    if(!len || len>=0x40000 || size>127u*DU_BLOCK)return 0;
    int covered=0;
    for(uint32_t off=0;off<size;){
        uint8_t h[16];
        if(size-off<16)return 0;
        if(image){for(int i=0;i<16;i++)h[i]=image[off+i];}
        else if(Y_LSEEK(fd,(int)off,0)!=(int)off || Y_READ(fd,h,16)!=16)return 0;
        uint8_t x=0;for(int i=0;i<16;i++)x^=h[i];
        if(h[3]!=0xad || x)return 0;
        uint32_t flags=(uint32_t)h[0]|(uint32_t)h[1]<<8;
        uint32_t addr=image_u32(h+4), count=image_u32(h+8);
        off+=16;
        if(!(flags&0x100) && count>size-off)return 0;
        if(count && addr<0x01E00000u+len && addr+count>=addr && addr+count>0x01E00000u){
            if(flags&0x2d00)return 0; /* fill/callback/indirect cannot prove plain core identity */
            uint32_t lo=addr<0x01E00000u?0x01E00000u:addr;
            uint32_t hi=addr+count<0x01E00000u+len?addr+count:0x01E00000u+len;
            if(lo==0x01E00000u && hi==lo+len)covered=1;
            uint32_t at=off+lo-addr;
            if(!image && Y_LSEEK(fd,(int)at,0)!=(int)at)return 0;
            for(uint32_t done=0;done<hi-lo;){
                uint32_t n=hi-lo-done;if(n>sizeof io)n=sizeof io;
                const uint8_t *b=image?image+at+done:io;
                if(!image && Y_READ(fd,io,n)!=(int)n)return 0;
                for(uint32_t i=0;i<n;i++)if(b[i]!=core[lo-0x01E00000u+done+i])return 0;
                done+=n;
            }
        }else if(addr+count<addr)return 0;
        if(!(flags&0x100))off+=count;
    }
    return covered;
}
static int installed_core_matches(void)
{
    int fd=Y_OPEN("/yaffs2/OP1_vdk.ldr",O_READ,0x100);
    if(fd<0)return 0;
    int n=Y_LSEEK(fd,0,2);
    int ok=n>0 && matches_running_core(fd,0,(uint32_t)n);
    if(Y_CLOSE(fd))ok=0;
    return ok;
}
static uint32_t journal_crc(void)
{
    uint32_t saved = recovery.crc; recovery.crc = 0;
    uint32_t crc = du_crc32(0, (const uint8_t *)&recovery, sizeof recovery);
    recovery.crc = saved; return crc;
}
static void backup_name(int i, char *out)
{
    cat(out, BACKUP_DIR "/", "b000");
    int k = 0; while (out[k]) k++;
    out[k - 3] = '0' + i / 100; out[k - 2] = '0' + i / 10 % 10; out[k - 1] = '0' + i % 10;
}
/* YAFFS stores negative errno (R246 yaffs_set_error calls); accept either sign for ENOENT only. */
static int absent(const char *p)
{
    if (Y_ACCESS(p, 0) == 0) return 0;
    int e = Y_ERROR(); return e == -2 || e == 2;
}
static int remove_checked(const char *p)
{
    if (absent(p)) return 1;
    return Y_UNLINK(p) == 0 && absent(p);
}
static int copy_file(const char *src, const char *dst, uint32_t *size, uint32_t *crc)
{
    char parent[DU_MAX_PATH + 16]; cat(parent, dst, "");
    for (int k = 8; parent[k]; k++)
        if (parent[k] == '/') { parent[k] = 0; Y_MKDIR(parent, 0x1FF); parent[k] = '/'; }
    int in = Y_OPEN(src, O_READ, 0x100);
    if (in < 0) return 0;
    int out = Y_OPEN(dst, O_CREATE, 0x180);
    if (out < 0) { Y_CLOSE(in); return 0; }
    int ok = 1; uint32_t got = 0, c = 0;
    for (;;) {
        int n = Y_READ(in, io, sizeof io);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        if (Y_WRITE(out, io, (unsigned)n) != n) { ok = 0; break; }
        got += (uint32_t)n; c = du_crc32(c, io, (uint32_t)n);
    }
    if (Y_CLOSE(out)) ok = 0;
    if (Y_CLOSE(in)) ok = 0;
    if (size) *size = got;
    if (crc) *crc = c;
    return ok && file_crc(dst, got, c);
}

static int write_marker(const char *p, const void *data, unsigned n)
{
    int fd = Y_OPEN(p, O_CREATE, 0x180);
    if (fd < 0) return 0;
    int ok = Y_WRITE(fd, data, n) == (int)n;
    if (Y_CLOSE(fd)) ok = 0;
    return ok;
}
static int read_exact(const char *p, void *data, unsigned n)
{
    int fd = Y_OPEN(p, O_READ, 0x100);
    if (fd < 0) return 0;
    unsigned got = 0;
    while (got < n) { int k = Y_READ(fd, (uint8_t *)data + got, n - got); if (k <= 0) break; got += k; }
    uint8_t extra; int end = Y_READ(fd, &extra, 1);
    int closed = Y_CLOSE(fd);
    return got == n && end == 0 && !closed;
}
static void backup_cleanup(void)
{
    char p[40];
    for (int i = 0; i < DU_MAX_FILES; i++) { backup_name(i, p); Y_UNLINK(p); }
    Y_UNLINK(OLD_MANIFEST); Y_UNLINK(FAILED);
}
/* Journal is published only after every old file and the old manifest are verified.
 * Ordinary apply errors restore the entire batch, before audio/screens/databases open. */
static int prepare_backup(const du_file *f, int nf, int ldr)
{
    if (!absent(JOURNAL)) return 0;
    Y_MKDIR(BACKUP_DIR, 0x1FF);
    backup_cleanup();
    for (int i = 0; i < DU_MAX_FILES; i++) originals[i].exists = originals[i].size = originals[i].crc = 0;
    char src[DU_MAX_PATH + 16], dst[40];
    for (int i = 0; i < nf; i++) {
        if (i == ldr) continue;
        cat(src, "/yaffs2/", f[i].path);
        if (absent(src)) continue;
        if (Y_ACCESS(src, 0)) return 0;
        originals[i].exists = 1; backup_name(i, dst);
        if (!copy_file(src, dst, &originals[i].size, &originals[i].crc)) return 0;
    }
    if (!copy_file(DU_MANIFEST, OLD_MANIFEST, 0, 0)) return 0;
    recovery.magic = 0x4b4e5231; recovery.count = (uint32_t)nf;
    recovery.core_len = (uint32_t)((uintptr_t)_core_ro_end - 0x01E00000u);
    if (!recovery.core_len || recovery.core_len >= 0x40000) return 0;
    recovery.core_crc = du_crc32(0, (const uint8_t *)0x01E00000u, recovery.core_len);
    recovery.crc = journal_crc();
    if (!write_marker(JOURNAL, &recovery, sizeof recovery)) { Y_UNLINK(JOURNAL); return 0; }
    return read_exact(JOURNAL, &recovery, sizeof recovery) && recovery.crc == journal_crc();
}
static int rollback(const du_file *f, int nf)
{
    if (!original_core()) return 0;                         /* never restore old plugins under a newly flashed core */
    /* Verify all backups before modifying any destination. */
    char src[40], dst[DU_MAX_PATH + 16];
    for (int i = 0; i < nf; i++) {
        if (f[i].path[0] == 'O' || !originals[i].exists) continue;
        backup_name(i, src);
        if (!file_crc(src, originals[i].size, originals[i].crc)) return 0;
    }
    for (int i = 0; i < nf; i++) {
        if (f[i].path[0] == 'O') continue;
        cat(dst, "/yaffs2/", f[i].path);
        if (!originals[i].exists) { if (!remove_checked(dst)) return 0; continue; }
        if (file_crc(dst, originals[i].size, originals[i].crc)) continue;
        backup_name(i, src);
        if (!copy_file(src, dst, 0, 0) || !file_crc(dst, originals[i].size, originals[i].crc)) return 0;
    }
    /* Keep staging/backups for diagnosis; a new Send explicitly starts over. */
    if (!write_marker(FAILED, "failed\n", 7)) return 0;
    return remove_checked(JOURNAL);
}

static void cleanup(void)
{
    char name[40];
    for (int i = 0; i <= DU_MAX_FILES; i++) { du_stage_name(i, name); Y_UNLINK(name); }
    Y_UNLINK(DU_MANIFEST);
}

volatile int du_fw_result = -1;                                   /* last apply: 0 ok, else step (emulator tests) */
static int du_fw_flashed;                                         /* the last apply wrote OP1_vdk.ldr */

static int apply(void)
{
    static char text[DU_MAX_FILES * (DU_MAX_PATH + 32) + 64] kn_farbss;
    static du_file f[DU_MAX_FILES] kn_farbss;
    int recovering = !absent(JOURNAL);
    if (!recovering && !absent(FAILED)) return 601;             /* explicit new Send required */
    int fd = Y_OPEN(recovering ? OLD_MANIFEST : DU_MANIFEST, O_READ, 0x100);
    if (fd < 0) return 1;
    int n = Y_READ(fd, text, sizeof text - 1);
    Y_CLOSE(fd);
    if (n <= 0) return 2;
    text[n] = 0;
    int nf;
    if (du_manifest_parse(text, f, &nf)) return 3;
    if (recovering) {
        if (!read_exact(JOURNAL, &recovery, sizeof recovery) || recovery.magic != 0x4b4e5231 ||
            recovery.count != (uint32_t)nf || recovery.crc != journal_crc()) return 602;
        if (original_core()) {
            if (!rollback(f, nf)) return 602;
            return 600;
        }
        /* A boot-menu package refreshes the on-disk LDR; our raw NAND flasher
         * deliberately does not. A later complete install supersedes this old
         * transaction. Preserve backups for diagnosis, and never replay it. */
        if(installed_core_matches()){
            if(!write_marker(FAILED,"superseded\n",11) || !remove_checked(DU_MANIFEST) || !remove_checked(JOURNAL))return 603;
            return 606;
        }
        /* Otherwise only the actual staged target may finish this transaction. */
    }
    char name[40];
    int ldr = -1;
    for (int i = 0; i < nf; i++) {                                 /* everything checked before anything changes */
        if (f[i].size == DU_DELETED) continue;                     /* a delete: no staged file */
        du_stage_name(i, name);
        if (!file_crc(name, f[i].size, f[i].crc)) return 10;
        if (f[i].path[0] == 'O') ldr = i;                          /* (du_path_ok: "OP1_vdk.ldr", "content/...", "plugins/...") */
    }
    uint8_t *buf = 0;
    if (ldr >= 0) {                                                /* LDR read and checked before anything changes */
        buf = NEW_ARR(f[ldr].size);
        if (!buf) return 80;
        du_stage_name(ldr, name);
        fd = Y_OPEN(name, O_READ, 0x100);
        uint32_t got = 0;
        if (fd >= 0) {
            for (;;) {
                int k = Y_READ(fd, buf + got, f[ldr].size - got > 65536 ? 65536 : f[ldr].size - got);
                if (k <= 0) break;
                got += (uint32_t)k;
            }
            Y_CLOSE(fd);
        }
        int c = got != f[ldr].size || du_crc32(0, buf, got) != f[ldr].crc ? 81 : du_ldr_check(buf, got, du_r246_fp, du_r246_fp_n);
        if (c) { DEL_ARR(buf); return c == 81 ? c : 90 + c; }     /* 91 bad LDR, 97 not an R246 OP1_vdk */
    }
    if(recovering && (!buf || !matches_running_core(-1,buf,f[ldr].size))){
        if(buf)DEL_ARR(buf);
        return 604; /* unknown running core: retain journal, touch no destinations */
    }
    if (!recovering && !prepare_backup(f, nf, ldr)) { if (buf) DEL_ARR(buf); return 82; }
    for (int i = 0; i < nf; i++) {
        if (i == ldr) continue;
        CONSOLE(f[i].path);
        if (f[i].size == DU_DELETED) {                             /* plugin file removed (missing: fine) */
            char dst[DU_MAX_PATH + 16];
            cat(dst, "/yaffs2/", f[i].path);
            if (!remove_checked(dst)) { rollback(f, nf); if (buf) DEL_ARR(buf); return 100 + i; }
            continue;
        }
        du_stage_name(i, name);
        char dst[DU_MAX_PATH + 16]; cat(dst, "/yaffs2/", f[i].path);
        if (!copy_file(name, dst, 0, 0) || !file_crc(dst, f[i].size, f[i].crc)) {
            rollback(f, nf); if (buf) DEL_ARR(buf); return 100 + i;
        }
    }
    du_fw_flashed = buf != 0;
    if (buf) {
        CONSOLE("Writing firmware...");
        int r = du_fw_flash(buf, f[ldr].size);
        DEL_ARR(buf);
        if (r) { rollback(f, nf); return 300 + r; }
    }
    /* Commit is durable before any backup can be removed. */
    if (!remove_checked(JOURNAL)) return 603;
    backup_cleanup();
    return 0;
}

int du_boot_access(const char *path, int mode)
{
    static int done;
    if (!done) {
        done = 1;
        if (Y_ACCESS(DU_MANIFEST, 0) == 0 || !absent(JOURNAL)) {
            CONSOLE("kennel: applying update...");
            int r = apply();
            du_fw_result = r;
            if (!absent(JOURNAL)) {
                /* Unresolved rollback/commit cannot expose a mixed batch to the running core. */
                CONSOLE("kennel: update recovery pending; retrying after restart (COM for boot recovery).");
                UNMOUNT(); DELAY_MS(500); WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0;
                for (;;) ;
            }
            if (r == 0 || r < 100) cleanup();
            if(r==606){CONSOLE("kennel: older update superseded by installed firmware; staging discarded.");return Y_ACCESS(path,mode);}
            if (r == 600 || r == 601 || (r >= 100 && r < 300)) {
                CONSOLE("kennel: update failed; previous files restored. Send again to retry.");
                return Y_ACCESS(path, mode);
            }
            if (r != 0 && r < 100) CONSOLE("kennel: invalid update discarded.");   /* nothing changed: boot on */
            else if (r == 0 && !du_fw_flashed) CONSOLE("kennel: updated.");   /* plugins load later this boot */
            else {
                CONSOLE(r == 0 ? "kennel: applied, restarting..." : "kennel: update FAILED (te-boot safe), restarting...");
                UNMOUNT(); DELAY_MS(500);
                /* system reset by watchdog, as te-boot after an update (te-boot 0x000169EC). Not power-off: at this
                 * point nothing watches the soft power switch, and over USB the OP-1 stayed "off" (docs/35). */
                WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0;
                for (;;) ;
            }
        }
    }
    return Y_ACCESS(path, mode);
}

/* ---- transfer phase (running firmware, --dev only): docs/35 ----
 * PlayerManager::tick -> midi_poll: CALL midi_rx_parse (0x019E3CC8) -> du_rx_poll (timer context, no blocking):
 * copies our SysEx messages (F0 00 20 76 7D ...) from the USB-MIDI RX ring into a queue, then runs the stock parser
 * (which ignores them). The main loop (ci_service) -> du_service: du_handle with YAFFS2 writes, reply as USB-MIDI
 * SysEx packets. COMMIT shows a prompt over the current screen (screens/duprompt.c): T1 = manifest, reply, restart
 * (watchdog; the boot hook applies), T2 = staging discarded. */
#define RX_RING   ((volatile uint8_t *)0x01AB0510)                /* 1024 x 4-byte USB-MIDI packets */
#define RX_WR     (*(volatile uint32_t *)0x01AB1510)              /* free-running counters (& 0x3FF) */
#define RX_RD     (*(volatile uint32_t *)0x01AB1514)
#define RX_PARSE  ((void (*)(void))0x019E31A4)
#define TX_ENQ    ((void (*)(int, const void *, unsigned))0x019E0D18)   /* usbmidi_tx_enqueue(_, packets, count) */
#define SCREEN_TABLE   ((void **)0x01DF4E54)
#define CURRENT_SCREEN (*(volatile int *)0x018B65C8)
#define QN 16

static uint8_t q[QN][DU_MAX_MSG] kn_farbss;
static uint16_t qlen[QN];
static volatile unsigned qw, qr;
static uint8_t as[DU_MAX_MSG];
static int an = -1;                                               /* bytes assembled, -1: outside a message */
static unsigned rx_pos;
volatile int du_fw_rx, du_fw_drop, du_fw_staged;                  /* counters for tests */

static void rx_byte(uint8_t b)
{
    if (b == 0xF0) an = 0;
    if (an < 0) return;
    if (an >= DU_MAX_MSG) { an = -1; return; }
    as[an++] = b;
    if (b != 0xF7) return;
    if (an > DU_SYSEX_HDR && as[1] == 0 && as[2] == 0x20 && as[3] == 0x76 && as[4] == 0x7D) {
        if (qw - qr < QN) {
            uint8_t *d = q[qw % QN];
            for (int i = 0; i < an; i++) d[i] = as[i];
            qlen[qw % QN] = (uint16_t)an; qw++; du_fw_rx++;
        } else du_fw_drop++;                                       /* host resends (DATA: offset reply) */
    }
    an = -1;
}

void kn_midi_raw_packet(const volatile uint8_t *pk);
void du_rx_poll(void)
{
    unsigned wr = RX_WR;
    if (wr - rx_pos > 1024) rx_pos = RX_RD;                        /* first call / lapped */
    for (; rx_pos != wr; rx_pos++) {
        volatile uint8_t *p = RX_RING + (rx_pos & 0x3FF) * 4;
        int cin = p[0] & 15, k = cin == 5 ? 1 : cin == 6 ? 2 : (cin == 4 || cin == 7) ? 3 : 0;
        for (int i = 0; i < k; i++) rx_byte(p[1 + i]);
        kn_midi_raw_packet(p);                                     /* features' raw MIDI in (kennel/kn_midi.c) */
    }
    RX_PARSE();
}

static int fd_w = -1;
static int f_open(void *c, const char *p) { (void)c; if (!absent(JOURNAL) || !absent(FAILED)) return 1; Y_MKDIR(DU_STAGE_DIR, 0x1FF); fd_w = Y_OPEN(p, O_CREATE, 0x180); return fd_w < 0; }
static int f_write(void *c, const uint8_t *p, uint32_t n) { (void)c; return Y_WRITE(fd_w, p, n) != (int)n; }
static int f_close(void *c) { (void)c; int r = Y_CLOSE(fd_w); fd_w = -1; return r; }
static int f_remove(void *c, const char *p)
{
    (void)c;
    if (!absent(JOURNAL)) return -1;
    if (!remove_checked(p)) return -1;
    const char *a = p, *b = DU_MANIFEST;
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) backup_cleanup();                         /* explicit ABORT/new Send releases old recovery */
    return 0;
}
/* ---- prompt: COMMIT -> "UPDATE READY", T1 restart / T2 cancel ---- */
void *du_prompt_obj;                                              /* screen object (dispatcher.c, boot) */
extern int dp_files, dp_kb, dp_restarting;
static volatile int prompt, confirmed, cancel_req, restart_req;
static void *saved_scr; static int saved_id = -1;
static du_session sess kn_farbss;

static void prompt_show(int on)                                   /* over the current screen, follows screen changes */
{
    int cur = CURRENT_SCREEN;
    if (saved_id >= 0 && (!on || saved_id != cur || SCREEN_TABLE[saved_id] != du_prompt_obj)) {
        if (SCREEN_TABLE[saved_id] == du_prompt_obj) SCREEN_TABLE[saved_id] = saved_scr;   /* (else someone swapped it) */
        saved_id = -1;
    }
    if (on && du_prompt_obj && saved_id < 0 && cur >= 0 && cur < 0x44) {
        saved_scr = SCREEN_TABLE[cur]; saved_id = cur; SCREEN_TABLE[cur] = du_prompt_obj;
    }
}

int du_prompt_key(int code, int pressed)                          /* key hook (dispatcher.c): 1 = swallowed */
{
    if (!prompt || (code != 0x06 && code != 0x07)) return 0;
    if (pressed && !confirmed && !cancel_req) { if (code == 0x06) confirmed = 1; else cancel_req = 1; }
    return 1;
}

static int f_key(void *c)                                         /* COMMIT asks: first time shows the prompt */
{
    (void)c;
    if (!prompt) {
        uint32_t b = 0;
        for (int i = 0; i < sess.n; i++) if (sess.f[i].size != DU_DELETED) b += sess.f[i].size;
        dp_files = sess.n; dp_kb = (int)((b + 1023) / 1024); dp_restarting = 0;
        confirmed = cancel_req = 0; prompt = 1;
    }
    return confirmed;
}
static void f_reboot(void *c) { (void)c; du_fw_staged = 1; restart_req = 1; }   /* after the OK reply */
/* ---- inventory (kennel, docs/inventory.md): LIST reads kn_inv.c's text, OFF reads / writes "kennel.off" ---- */
int kn_inv_read(uint32_t off, uint8_t *out, int cap, uint32_t *total);
const char *kn_off_boot(void);
int op1_setting_get_str(const char *feature, const char *key, char *out, int size);
void kn_off_store(const char *names);
static int f_inv(void *c, uint32_t off, uint8_t *out, int cap, uint32_t *total) { (void)c; return kn_inv_read(off, out, cap, total); }
static int f_off_get(void *c, int at_boot, char *out, int cap)
{
    (void)c;
    if (!at_boot) { int n = op1_setting_get_str("kennel", "off", out, cap); return n < cap ? n : cap - 1; }
    const char *l = kn_off_boot();
    int n = 0;
    for (; l[n] && n < cap - 1; n++) out[n] = l[n];
    out[n] = 0;
    return n;
}
static int f_off_set(void *c, const char *names)                  /* stored, then read back */
{
    (void)c;
    static char back[DU_OFF_MAX + 2];
    kn_off_store(names);
    int n = op1_setting_get_str("kennel", "off", back, (int)sizeof back);
    for (int i = 0; i <= n && i <= DU_OFF_MAX; i++) if (back[i] != names[i]) return 1;
    return n > DU_OFF_MAX;
}
static const du_fs fw_fs = { f_open, f_write, f_close, f_remove, f_key, f_reboot, 0, f_inv, f_off_get, f_off_set };

static void send_sysex(const uint8_t *m, int n)
{
    static uint32_t pk[DU_MAX_MSG / 3 + 2];
    int np = 0;
    for (int i = 0; i < n; i += 3) {
        int k = n - i < 3 ? n - i : 3, cin = n - i > 3 ? 4 : 4 + k;   /* 4: continues, 5/6/7: ends with 1/2/3 */
        uint32_t w = (uint32_t)cin;
        for (int j = 0; j < k; j++) w |= (uint32_t)m[i + j] << (8 * (j + 1));
        pk[np++] = w;
    }
    TX_ENQ(0, pk, (unsigned)np);
}

void du_service(void)
{
    static int inited;
    while (qr != qw) {
        if (!inited) { du_init(&sess, &fw_fs); inited = 1; }
        static uint8_t rep[DU_MAX_MSG];
        int n = du_handle(&sess, q[qr % QN], qlen[qr % QN], rep);
        qr++;
        if (n > 0) send_sysex(rep, n);
    }
    if (cancel_req && inited) { du_abort(&sess); cancel_req = 0; prompt = 0; }   /* next COMMIT: STATE = cancelled */
    if (prompt || saved_id >= 0) prompt_show(prompt);
    if (restart_req) {
        dp_restarting = 1;
        { extern int bgs_drain(int ms); bgs_drain(3000); }        /* delayed patch saves (bgsave.c): no shutdown path here */
        { extern int op1_tape_flush(void); op1_tape_flush(); }    /* tape chunks still in RAM (kn_tape.c: held while recording) */
        DELAY_MS(400);                                             /* reply out, "RESTARTING" drawn (GUI thread) */
        UNMOUNT();                                                 /* flushes all if nothing is open (else refused) */
        DELAY_MS(100);
        WDOG_CTL = 0xAD0; WDOG_CNT = 1; WDOG_CTL = 0xFF0;          /* system reset (as te-boot) -> boot hook applies */
        for (;;) ;
    }
}
