/*
 * coop-fw master bus suite — firmware payload (Phase 2, item 11).
 *
 * coop_master_apply() runs a coop FX slot (a stereo delay, core/include/coop_fx.h) then the host-tested
 * master suite (comp -> drive -> EQ, core/include/coop_master.h) on the mixed master bus
 * d34 (0xff800d34, L) / db4 (0xff800db4, R), 64 x int16, planar. We widen int16 -> Q31, process, and
 * narrow back with saturation.
 *
 * Where it sits (op1emu write-traced 2026-11-01, docs/audio-graph.md): the L1 master stage calls a
 * trivial SDRAM getter at 0xffa017ac (`CALL (P1)`, P1 = 0x019d90e8: `R0 = 0xff80187c; RTS`) and then
 * the stock master level/comp stage `CALL 0xffa04280` at 0xffa017ae, which reads d34/db4 and WRITES the
 * int32 output buses e34 (0xff800e34, L) / f34 (0xff800f34, R) that the SPORT TX conversion loop
 * (0xffa0171c..0xffa0178e) and the USB stream (0x01915608) consume. Milestone B replaced 0xffa04280
 * with this function, so e34/f34 were never written and the codec output was silence (the earlier
 * "e34/f34 = USB bus, always zero" reading was a probe at block start, before the stage ran). So this
 * function now takes the GETTER's place instead: tools/l1_insert.py --imm-pair-site 0xffa017a4 points
 * P1 at it, it processes d34/db4 before the stock stage runs, and it must return the getter's value
 * (R0 = 0xff80187c, the first argument 0xffa04280 receives). 0xffa04280 keeps its original SP (it
 * uses a caller stack slot, which is why calling it through a trampoline hung). The stock stage
 * overwrites R0-R2 and P0-P2 before using them, and the code after it (0xffa01372) sets its own
 * pointers, so a C function's clobbers are safe here.
 *
 * Validated in op1emu: the delay line stays zero at idle and fills during a chord (the FX reads live
 * audio); a mute build (write 0 to d34/db4) confirms the coop insert owns the master-bus content; the
 * --audio-dump WAV of the SPORT output carries the chord (the test the replace-mode image failed).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_master.h"
#include "coop_fx.h"
#include "coop_plugin_host.h"

/* The mixed master bus d34/db4 (int16, 64 samples, planar): what the stock master FX render is handed
 * (L1 0xffa017bc/c6) and what the stock level stage 0xffa04280 reads to produce e34/f34. We widen
 * int16 -> Q31 for the coop DSP and narrow back with saturation. */
#define MBUS_L ((volatile int16_t *)0xff800d34u)
#define MBUS_R ((volatile int16_t *)0xff800db4u)
#define BLOCK 64
/* What the stock getter at 0x019d90e8 returns; 0xffa04280 receives it in R0 (see the header comment). */
#define STOCK_GETTER_RESULT 0xff80187cu
extern "C" void coop_slot2_render(int pos, int16_t *l, int16_t *r);   /* plugin_host.cc, S2 */
extern "C" void coop_runner_click(int16_t *l, int16_t *r);            /* coop_runner.cc, p5b */

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

/* Image-time controls (inject_app --set-word): bit 0 = run the demo FX delay, bit 1 = run the master
 * suite. Default 0: the stage is a transparent pass-through, which the stereo-bus regression gate
 * (coop image == stock image, sample for sample) relies on. */
__attribute__((section(".data"))) volatile uint32_t g_coop_master_flags = 0;

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

#include "coop_cost.h"
extern "C" uint32_t coop_master_apply(void)
{
    if (!g_inited) {
        coop_master_init(&g_master);    /* transparent until params are set */
        coop_delay_init(&g_fx, FXB_L, FXB_R, FXB_LEN);
        coop_delay_set(&g_fx, FX_DELAY_SAMPLES, FX_FB_Q31, FX_MIX_Q31);
        g_inited = 1;
    }
    uint32_t m0 = coop_cycles();
    coop_slot2_render(1, (int16_t *)MBUS_L, (int16_t *)MBUS_R);   /* S2: the master's second slot, after the stock master
                                                                     effect (0xffa017e6), before the level stage */
    coop_runner_click((int16_t *)MBUS_L, (int16_t *)MBUS_R);      /* p5b: the test runner's alignment click, if due */
    uint32_t flags = g_coop_master_flags;
    if (!(flags & 3u)) { coop_cost_add(COOP_COST_MASTER, coop_cycles() - m0); return STOCK_GETTER_RESULT; }   /* transparent */
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) { L[i] = (int32_t)MBUS_L[i] << 16; R[i] = (int32_t)MBUS_R[i] << 16; }

    /* FX slot 2 (coop stereo FX), in series before the master suite. Timed separately. */
    uint32_t f0 = coop_cycles();
    if (flags & 1u) coop_delay_process(&g_fx, L, R, BLOCK);
    uint32_t fdt = coop_cycles() - f0;
    if (fdt > g_fx_cycles) g_fx_cycles = fdt;
    g_fx_blocks++;

    /* master suite: comp -> drive -> EQ */
    uint32_t t0 = coop_cycles();
    if (flags & 2u) coop_master_process(&g_master, L, R, BLOCK);
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_master_cycles) g_master_cycles = dt;
    g_master_blocks++;

    for (int i = 0; i < BLOCK; i++) { MBUS_L[i] = coop_sat16(L[i] >> 16); MBUS_R[i] = coop_sat16(R[i] >> 16); }
    return STOCK_GETTER_RESULT;   /* we stand in for the getter the stock level stage's caller invokes */
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
