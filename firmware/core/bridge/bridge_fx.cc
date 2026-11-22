/*
 * coop's bridge: a kennel effect behind coop's effect call (bridge.h, docs/bridge.md "Effects"). kennel's render is
 * mono int16, 64 samples in place, state per position; coop's is stereo int32 (the int16 bus widened by 16 bits).
 * A mono core is hosted dual-mono (docs/fx-abi.md, the owner's decision of 2026-11-01): two relocations of the file,
 * one a channel, so the channels never share state. A file with kennel's stereo flag gets both channels in one call
 * at the master position, as on kennel; at the synth and drum positions kennel calls its mono render, and so do we,
 * once a channel.
 *
 * The position buffer: a kennel effect reaches R246's own buffer through the table at 0x01AB8BA0 (an inline, not an
 * import). The left instance uses the stock's buffer of the position, as on kennel; around every call into the right
 * instance the table's entry is swapped to that instance's buffer and put back. Audio thread only (init at positions
 * 0-2 runs there too), and the stock effect that owns the buffer is not the one playing while a plugin is.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include "bridge.h"
extern "C" {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

static inline int16_t narrow(int32_t x)           /* Q31 -> int16, rounded: exact for the widened int16 bus */
{
    if (x >= 0x7FFF8000) return 32767;
    return (int16_t)((x + 0x8000) >> 16);
}

static void right_call(br_fx_inst *x, int pos, int16_t *buf, const int16_t *params)   /* buf 0: init */
{
    void *keep = 0;
    const int swap = x->scratch_tab && x->right_scratch[pos];
    if (swap) { keep = x->scratch_tab[pos]; x->scratch_tab[pos] = x->right_scratch[pos]; }
    if (buf) x->right->render(pos, buf, params);
    else x->right->init(pos);
    if (swap) x->scratch_tab[pos] = keep;
}

int br_fx_init(br_fx_inst *x, int slot)
{
    if (slot < 0 || slot > 2) return -1;          /* kennel knows three positions: the second slots are refused */
    x->left->init(slot);
    right_call(x, slot, 0, 0);
    return 0;
}

void br_fx_render(br_fx_inst *x, int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    if (slot < 0 || slot > 2 || n != BR_FX_BLOCK) { x->dry_blocks++; return; }
    int16_t a[BR_FX_BLOCK], b[BR_FX_BLOCK];
    for (int i = 0; i < BR_FX_BLOCK; i++) { a[i] = narrow(l[i]); b[i] = narrow(r[i]); }
    if (x->stereo && slot == 2) x->left->render_stereo(slot, a, b, params);
    else {
        x->left->render(slot, a, params);
        right_call(x, slot, b, params);
    }
    for (int i = 0; i < BR_FX_BLOCK; i++) { l[i] = (int32_t)((uint32_t)(int32_t)a[i] << 16); r[i] = (int32_t)((uint32_t)(int32_t)b[i] << 16); }
}
}   /* extern "C" */
