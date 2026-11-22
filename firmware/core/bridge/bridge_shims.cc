/*
 * coop's bridge: the stand-ins a kennel import resolves to (bridge_imports.def). Each one is an answer kennel's own
 * SDK documents (op1_l1.h: "0: no pool ... the output is the same either way"; op1_plugin.h: op1_fx_fast "0 = no
 * lease", op1_sc_source "0 and zeros: not available", op1_dbg "an empty core call"), so a file that gets it behaves as
 * on a kennel core with that service off. Audio thread and GUI thread safe: no state but the three page arrays.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include "bridge.h"
extern "C" {

/* op1_fx_page[pos][k] / op1_eng_page[k]: 1 while knob k shows page 2 (or its latch is on). Nothing sets them until B5,
 * so every file stays on page 1. op1_fx_wrap[pos][k]: the file writes how far a stepped knob wraps; read from B5. */
volatile uint8_t br_op1_fx_page[3][4], br_op1_fx_wrap[3][4], br_op1_eng_page[4];

void *br_op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh)
{
    (void)part; (void)owner; (void)bytes;
    if (fresh) *fresh = 0;
    return 0;
}

void *br_op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh)
{
    (void)part; (void)home; (void)state_bytes; (void)bytes;
    if (fresh) *fresh = 0;
    return 0;
}

const int16_t *br_op1_l1_sine_quarter(void) { return 0; }

void br_op1_dbg(const char *text, int a, int b) { (void)text; (void)a; (void)b; }

void *br_op1_fx_fast(int pos, uint32_t *size)
{
    (void)pos;
    if (size) *size = 0;
    return 0;
}

int br_op1_sc_source(int src, int16_t *out)
{
    (void)src;
    if (out) for (int i = 0; i < BR_FX_BLOCK; i++) out[i] = 0;
    return 0;
}

/* a screen call of a file whose screen the bridge does not show yet: never reached (the screen object is not
 * registered), and 0 if it ever is. Blackfin's caller owns the arguments, so any signature may land here. */
int br_stub0(void) { return 0; }
}   /* extern "C" */
