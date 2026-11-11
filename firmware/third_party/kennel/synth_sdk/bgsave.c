/* Delayed patch saves. Stock: every knob detent on a synth / drum sound calls 0x019411B8(is_drum, slot), which queues
 * a SaveSlotJob (12 bytes: vtable 0x0190F4AC, +4 byte is_drum, +8 slot) on BgFileWorker's 128-entry ring; the job
 * (run 0x01940574) serialises the slot's patch and runs `UPDATE synth_slots / drum_slots SET patch=...` on op1.db.
 * Upstream measured 10 detents = about 15 op1.db transactions and 1216 NAND pages (2.4 MB).
 * Here the queue call only notes (kind, slot); the jobs are queued once nothing new came for BGS_QUIET (main loop,
 * bgs_service), or at once on a key press (dispatcher.c key_event_hook: a mode change, disk mode, ...). The job saves
 * the patch as it is then, so only the intermediate states are skipped.
 *
 * Power-off and restart (kennel addition, not upstream): R246's shutdown stops BgFileWorker (CALL 0x019412D4 at
 * 0x019F0BDC) before the tape write-back and the yaffs unmount (0x019F0BEA). The stop sets B[0x01BA3408] and posts
 * the wake semaphore; the worker checks that byte before it looks at the ring, so jobs still queued are dropped, and
 * a job queued after the stop is never run. So:
 *   - kn_bgs_stop (hook.S .hook_bgsstop, replaces that CALL): queue what is pending, wait (bounded) until the ring is
 *     empty and no job runs, then the stock stop. This also covers jobs queued by a key press just before.
 *   - bgs_flush_sync (kennel/kn_boot.c kn_boot_unmount, after the stop, before the unmount): anything noted since is
 *     saved synchronously, by calling the job's run method through its vtable as the worker would (so any core
 *     wrapper in that slot, e.g. the drum synths' live-patch busy count, runs too). The worker is gone and the main
 *     loop is not running (this is inside the shutdown function): nothing else uses the job's static SQL buffer
 *     0x01BA340C or replaces live patch JSON meanwhile.
 * Disk mode (kennel/kn_tape.c kn_disk_toggle) and the dev-update watchdog reset (devupd/du_fw.c) flush and wait too.
 * Still lost: power cut (no shutdown path) within BGS_QUIET of the last turn, and jobs still queued when the 3 s wait
 * runs out (as stock).
 *
 * From op1r-src a7b4c3f (op1r 46, commit cc81b2b, pcppcp; GPL-3.0-or-later with the OP-1 firmware permission).
 * Kennel adaptation Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE
 * and NOTICE): the shutdown / disk mode / dev update flush (bgs_drain, kn_bgs_stop, bgs_flush_sync). */
#include "op1_synth.h"

#define BGS_QUIET  600000000u                               /* cycles: 1.5 s at 400 MHz */
#define BGS_WAIT   3000                                     /* ms: longest wait for BgFileWorker's ring */

static volatile uint16_t bgs_pending;                       /* bit kind * 8 + slot */
static volatile uint32_t bgs_last;

#ifdef OP1_HOST
uint32_t bgs_host_now; int bgs_host_jobs[16], bgs_host_njobs; int bgs_host_sync[16], bgs_host_nsync;
int bgs_host_ring, bgs_host_stopped, bgs_host_slept;       /* ring: jobs the "worker" still has (one per 10 ms) */
#define NOW()           bgs_host_now
#define QUEUE(kind, s)  (bgs_host_jobs[bgs_host_njobs++ & 15] = (kind) * 8 + (s), bgs_host_ring++)
#define RUN(kind, s)    (bgs_host_sync[bgs_host_nsync++ & 15] = (kind) * 8 + (s))
#define RING_IDLE()     (bgs_host_ring == 0)
#define DELAY_MS(ms)    (bgs_host_slept += (ms), bgs_host_ring -= bgs_host_ring > 0)
#define BG_STOP()       (bgs_host_stopped = 1)
static uint32_t irq_off(void) { return 0; }
static void irq_on(uint32_t m) { (void)m; }
#else
extern void bgs_queue_stock(int is_drum, int slot);         /* hook.S: the stock queue call (LINK + 0x019411BC) */
#define NOW()           op1_cycles()
#define QUEUE(kind, s)  bgs_queue_stock((kind), (s))
#define JOB_VT          0x0190F4AC                          /* SaveSlotJob vtable: +0x10 this delta, +0x14 run (0x01940574) */
#define RING_HEAD       (*(volatile int32_t *)0x01BA33E4)   /* BgFileWorker ring: next to run */
#define RING_TAIL       (*(volatile int32_t *)0x01BA33E8)   /* .. next free (equal: empty) */
#define RING_CUR        (*(void *volatile *)0x01BA29D8)     /* the job running (0 between jobs; popped before it runs) */
#define RING_IDLE()     (RING_HEAD == RING_TAIL && !RING_CUR)
#define DELAY_MS        ((void (*)(int))0x0193E5C0)         /* sleeps (VDK_Sleep): the worker (priority 22) runs */
#define BG_STOP         ((void (*)(void))0x019412D4)        /* BgFileWorker stop: flag, wake, wait until it ended */
/* the job's run method as BgFileWorker calls it: through the vtable, so a core wrapper patched into the slot
 * (make_firmware.py SLOTS, e.g. the live-patch busy count kn_ds_slot_save_job) runs here too */
static void RUN(int kind, int slot)
{
    struct { uint32_t vt; uint8_t drum, pad[3]; int32_t slot; } job = { JOB_VT, (uint8_t)kind, { 0, 0, 0 }, slot };
    void (*run)(void *) = *(void (*const volatile *)(void *))(JOB_VT + 0x14);
    run((uint8_t *)&job + *(volatile int16_t *)(JOB_VT + 0x10));
}
static uint32_t irq_off(void) { uint32_t m; __asm__ volatile ("cli %0;" : "=d"(m)); return m; }
static void irq_on(uint32_t m) { __asm__ volatile ("sti %0;" : : "d"(m)); }
#endif

void bgs_queue_slot(int is_drum, int slot)                  /* replaces 0x019411B8 (hook.S) */
{
    if (slot < 0 || slot > 7) { QUEUE(is_drum & 0xFF, slot); return; }   /* unknown: stock behaviour */
    uint32_t m = irq_off();
    bgs_pending |= (uint16_t)(1u << ((is_drum & 1) * 8 + slot));
    bgs_last = NOW();
    irq_on(m);
}

static uint16_t bgs_take(void)
{
    uint32_t m = irq_off();
    uint16_t p = bgs_pending;
    bgs_pending = 0;
    irq_on(m);
    return p;
}

void bgs_flush(void)                                        /* queue the waiting saves now */
{
    if (!bgs_pending) return;
    uint16_t p = bgs_take();
    for (int b = 0; b < 16; b++) if (p >> b & 1) QUEUE(b >> 3, b & 7);
}

void bgs_service(void)                                      /* main loop: after a quiet period */
{
    if (bgs_pending && NOW() - bgs_last >= BGS_QUIET) bgs_flush();
}

/* queue the waiting saves and wait until BgFileWorker has run every queued job: 0 done, -1 not within ms. Sleeps:
 * call it from a thread above the worker (KeyHandler, main loop), never from the worker or the audio path. */
int bgs_drain(int ms)
{
    bgs_flush();
    for (int t = 0; t < ms; t += 10) {
        if (RING_IDLE()) return 0;
        DELAY_MS(10);
    }
    return RING_IDLE() ? 0 : -1;
}

/* shutdown (power-off, restart): replaces R246's CALL 0x019412D4 at 0x019F0BDC (hook.S .hook_bgsstop) */
void kn_bgs_stop(void)
{
    bgs_drain(BGS_WAIT);
    BG_STOP();
}

/* after the worker stopped (kennel/kn_boot.c kn_boot_unmount, before the yaffs unmount): save what is still noted,
 * synchronously, in the caller's thread */
void bgs_flush_sync(void)
{
    if (!bgs_pending) return;
    uint16_t p = bgs_take();
    for (int b = 0; b < 16; b++) if (p >> b & 1) RUN(b >> 3, b & 7);
}
