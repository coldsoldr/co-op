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
volatile uint32_t g_master_blocks;      /* liveness counter (region-A .bss), like m1's g_blocks */

extern "C" void coop_master_apply(void)
{
    if (!g_inited) {
        coop_master_init(&g_master);    /* transparent until params are set */
        g_inited = 1;
    }
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) { L[i] = MASTER_L[i]; R[i] = MASTER_R[i]; }
    coop_master_process(&g_master, L, R, BLOCK);
    for (int i = 0; i < BLOCK; i++) { MASTER_L[i] = L[i]; MASTER_R[i] = R[i]; }
    g_master_blocks++;
}
