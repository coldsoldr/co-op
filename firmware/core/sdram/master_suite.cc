/*
 * coop-fw master bus suite — firmware payload (Phase 2, item 11).
 *
 * coop_master_apply() runs the host-tested master suite (comp -> drive -> EQ, core/include/coop_master.h)
 * on the stereo master pair 0xff800e34 (L) / 0xff800f34 (R), 64 x int32 Q31. It is called once per
 * audio block from an insertion point inside the L1 master stage 0xffa00e50, just before the master
 * pair is read by the codec downmix and the USB push, so both outputs get the processed signal. The
 * exact insertion point is being pinned by tracing e34/f34 in op1emu (docs/phase2.md); this payload is
 * independent of that and ready to wire.
 *
 * The suite starts transparent (default init), as milestone 1 did, so the first integration confirms
 * the insert is harmless before any processing is enabled.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_master.h"

#define MASTER_L ((volatile int32_t *)0xff800e34u)
#define MASTER_R ((volatile int32_t *)0xff800f34u)
#define BLOCK 64

static coop_master g_master;
static int g_inited;
volatile uint32_t g_master_blocks;      /* liveness counter (region-A .bss) */
volatile uint32_t g_master_cycles;      /* peak per-block cost of the suite, for the perf gate */

static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

extern "C" void coop_master_apply(void)
{
    if (!g_inited) {
        coop_master_init(&g_master);    /* transparent until params are set */
        g_inited = 1;
    }
    uint32_t t0 = coop_cycles();
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) { L[i] = MASTER_L[i]; R[i] = MASTER_R[i]; }
    coop_master_process(&g_master, L, R, BLOCK);
    for (int i = 0; i < BLOCK; i++) { MASTER_L[i] = L[i]; MASTER_R[i] = R[i]; }
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_master_cycles) g_master_cycles = dt;
    g_master_blocks++;
}

/* OP1X header so inject_app.py can add this payload as a region-A block. No patches here: the L1
 * call-site retarget + trampoline are applied post-decrypt by tools/l1_insert.py. */
struct patch { uint32_t address, kind, stock; const void *value; };
struct header {
    uint32_t magic, version;
    void *render, *note_on, *note_off;
    const struct patch *patches;
    uint32_t num_patches, reserved;
};
#ifndef COOP_COMBINED
extern "C" __attribute__((section(".op1header"), used))
const struct header op1_header = { 0x5831504fu, 1u, 0, 0, 0, 0, 0u, 0u };
#endif
