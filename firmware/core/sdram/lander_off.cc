/*
 * coop-fw: the Lander easter-egg game removed (milestone T4d). Its 40 sound effects sit in L1 data B, which the coop L1
 * pool now uses for the stereo stages and the stock sine table (l1_pool.cc), so the game must never run. The KeyHandler
 * dispatch (CALL 0x019B6C74 at 0x019C2D3E) goes through coop_key_event, which swallows the fourth mode key of the
 * combo that enters the game (synth + drum + tape + mixer all held; kennel dispatcher.c key_event_hook) and passes
 * everything else to the stock dispatch. The game's screen object is still created at boot as stock does (kennel's
 * stub screen hung the boot in op1emu without kennel's other screen hooks); it is simply unreachable, and its sound
 * table points at size-0 dummies.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define KEY_DISPATCH   ((void (*)(int, int))0x019B6C74u)                          /* the stock key dispatch (code, pressed) */

volatile uint32_t g_lander_swallowed;
static uint8_t mode_keys;                                                         /* synth drum tape mixer held (bits 0-3) */

extern "C" void coop_key_event(int code, int pressed)
{
    if (code >= 0x02 && code <= 0x05) {                                           /* the four mode keys */
        uint8_t bit = (uint8_t)(1u << (code - 2));
        if (!pressed) mode_keys &= (uint8_t)~bit;
        else if ((mode_keys | bit) == 0x0F && !(mode_keys & bit)) { mode_keys |= bit; g_lander_swallowed++; return; }
        else mode_keys |= bit;
    }
    KEY_DISPATCH(code, pressed);
}
