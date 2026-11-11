// The host / GNU simulator stand-in for the core's tempo export (e10: core/sdram/plugin_host.cc coop_tempo_bpm_q16,
// sdk/coop_plugin.h), linked into the host test of every effect port that syncs to the tempo (tests/host/tape_echo).
// It runs the core's own conversion (core/include/coop_tempo.h) on a stand-in for the stock's float BPM word
// (FW_TEMPO_BPM_F32), which the test sets: coop_tempo_host_set(bits of the IEEE single).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include "coop_tempo.h"

static volatile uint32_t g_tempo_f32 = 0x42F00000u;   // 120.0f: the stock image's value at boot

extern "C" void coop_tempo_host_set(uint32_t f32_bits) { g_tempo_f32 = f32_bits; }
extern "C" uint32_t coop_tempo_bpm_q16(void) { return coop_tempo_q16_from_f32(g_tempo_f32); }
