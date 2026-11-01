/*
 * coop-fw master bus suite — firmware payload (Phase 2, item 11).
 *
 * coop_master_apply() runs a coop FX slot (a stereo delay, core/include/coop_fx.h) then the host-tested
 * master suite (comp -> drive -> EQ, core/include/coop_master.h) on the real codec/speaker master bus
 * d34 (0xff800d34, L) / db4 (0xff800db4, R), 64 x int16, planar. (op1emu probing proved the earlier
 * "master pair 0xff800e34/f34 Q31" was wrong: e34/f34 are the USB-audio bus and are zero on the
 * speaker path; see docs/audio-graph.md.) We widen int16 -> Q31, process, and narrow back with
 * saturation. It is called once per audio block from an insert in the L1 master stage 0xffa00e50
 * (tools/l1_insert.py retargets the comp/level CALL at 0xffa017ae), before the mix loop reads d34/db4.
 *
 * Validated in op1emu: the delay line stays zero at idle and fills during a chord (the FX reads live
 * audio); a mute build (write 0 to d34/db4) confirms the coop insert owns the master-bus content.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_master.h"
#include "coop_fx.h"

/* The live master bus is d34/db4 (int16, 64 samples, planar), confirmed by op1emu probe: e34/f34
 * (0xff800e34/f34) are always zero in this firmware config, while d34 (0xff800d34) / db4
 * (0xff800db4) carry the mixed master audio and are the buffers the stock master FX render is handed
 * (L1 0xffa017bc/c6). We widen int16 -> Q31 for the coop DSP and narrow back with saturation. */
#define MBUS_L ((volatile int16_t *)0xff800d34u)
#define MBUS_R ((volatile int16_t *)0xff800db4u)
#define BLOCK 64

/* Milestone B, FX slot 2: a coop stereo feedback delay in series before the master suite. Its delay
 * line lives high in free SDRAM region A (0x02d00000), above our ~5 KB payload at the region base.
 * Region A is the region we have proven free by running our own code there under stock firmware;
 * region B (0x02800000) is the documented scratch but is unverified under stock, so the line stays in
 * A for now. FXB_LEN samples per channel (64 KiB each). Demo params: ~0.15 s slap echo. */
#define FXB_LEN 16384
#define FXB_L   ((int32_t *)0x02d00000u)
#define FXB_R   ((int32_t *)0x02d10000u)
#define FX_DELAY_SAMPLES 6615            /* 0.15 s at 44.1 kHz */
#define FX_FB_Q31   858993459            /* 0.40 */
#define FX_MIX_Q31  751619277            /* 0.35 */

static coop_master g_master;
static coop_delay g_fx;
static int g_inited;
volatile uint32_t g_master_blocks;      /* liveness counter (region-A .bss) */
volatile uint32_t g_master_cycles;      /* peak per-block cost of the suite, for the perf gate */
volatile uint32_t g_fx_blocks;          /* FX slot 2 liveness counter */
volatile uint32_t g_fx_cycles;          /* peak per-block cost of the FX slot, for the perf gate */

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
        coop_delay_init(&g_fx, FXB_L, FXB_R, FXB_LEN);
        coop_delay_set(&g_fx, FX_DELAY_SAMPLES, FX_FB_Q31, FX_MIX_Q31);
        g_inited = 1;
    }
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) { L[i] = (int32_t)MBUS_L[i] << 16; R[i] = (int32_t)MBUS_R[i] << 16; }

    /* FX slot 2 (coop stereo FX), in series before the master suite. Timed separately. */
    uint32_t f0 = coop_cycles();
    coop_delay_process(&g_fx, L, R, BLOCK);
    uint32_t fdt = coop_cycles() - f0;
    if (fdt > g_fx_cycles) g_fx_cycles = fdt;
    g_fx_blocks++;

    /* master suite: comp -> drive -> EQ */
    uint32_t t0 = coop_cycles();
    coop_master_process(&g_master, L, R, BLOCK);
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_master_cycles) g_master_cycles = dt;
    g_master_blocks++;

    for (int i = 0; i < BLOCK; i++) { MBUS_L[i] = coop_sat16(L[i] >> 16); MBUS_R[i] = coop_sat16(R[i] >> 16); }
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
