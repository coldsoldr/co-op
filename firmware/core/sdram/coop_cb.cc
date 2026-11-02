/*
 * coop-fw Phase 2, milestone 1: identity wrap + measure of the audio callback.
 *
 * The stock firmware stores the audio DMA callback pointer (0xFFA04804) at 0xff80036c, built by the
 * imm pair at 0x0191AB52 (R1.L=0x4804 at +2, R1.H=0xFFA0 at +6; confirmed in the stock disassembly).
 * This payload, linked into free SDRAM region A, repoints that imm pair to CoopAudioCb, which calls
 * the stock callback unchanged and times it with the CYCLES counter. No audible change: it is the
 * seam for Phase 2, proven transparent before any stage is replaced.
 *
 * Measurements (CoopAudioCb's cycle cost per block) are kept in region-A .bss (g_last/g_max/g_blocks)
 * for a later on-screen readout. Verification for this milestone is that the device still boots and
 * runs normally with our callback in the audio path.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE, NOTICE).
 */
#include <stdint.h>

typedef int (*audio_cb_fn)(int r0, int r1);
#define FW_AUDIO_CB 0xFFA04804u

struct patch { uint32_t address, kind, stock; const void *value; };
struct header {
    uint32_t magic, version;
    void *render, *note_on, *note_off;
    const struct patch *patches;
    uint32_t num_patches, reserved;
};
enum { HEADER_MAGIC = 0x5831504fu };
enum { PATCH_IMM16_LO = 1, PATCH_IMM16_HI = 2 };

/* measurements, in region-A .bss */
volatile uint32_t g_last, g_max, g_blocks;

static inline uint32_t cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

#ifdef COOP_COMBINED
extern "C" void coop_stereo_block_start(void);   /* stereo bus: per-block reset (core/sdram/stereo_bus.cc) */
#endif

/* The replacement audio callback: time the stock callback and forward its result. */
extern "C" int CoopAudioCb(int r0, int r1)
{
    uint32_t t0 = cycles();
#ifdef COOP_COMBINED
    coop_stereo_block_start();
#endif
    int ret = ((audio_cb_fn)(uintptr_t)FW_AUDIO_CB)(r0, r1);
    uint32_t dt = cycles() - t0;
    g_last = dt;
    if (dt > g_max) g_max = dt;
    g_blocks++;
    return ret;
}

/* Repoint the audio-callback imm pair (0x0191AB52) to CoopAudioCb. inject_app checks the stock
 * imm halves (0x4804 / 0xFFA0) before writing. */
extern "C" const struct patch g_patches[] = {
    { 0x0191AB54u, PATCH_IMM16_LO, 0x4804u, (const void *)&CoopAudioCb },
    { 0x0191AB58u, PATCH_IMM16_HI, 0xFFA0u, (const void *)&CoopAudioCb },
};

extern "C" __attribute__((section(".op1header"), used))
const struct header op1_header = {
    HEADER_MAGIC, 1u, 0, 0, 0, g_patches, 2u, 0u,
};
