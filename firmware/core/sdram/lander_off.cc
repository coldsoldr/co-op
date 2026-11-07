/*
 * coop-fw: the Lander easter-egg game removed (milestone T4d). Its 40 sound effects sit in L1 data B, which the coop L1
 * pool now uses for the stereo stages and the stock sine table (l1_pool.cc), so the game must never run. The KeyHandler
 * dispatch (CALL 0x019B6C74 at 0x019C2D3E) goes through coop_key_event, which swallows the fourth mode key of the
 * combo that enters the game (synth + drum + tape + mixer all held; kennel dispatcher.c key_event_hook) and passes
 * everything else to the stock dispatch. The game's screen object is still created at boot as stock does (kennel's
 * stub screen hung the boot in op1emu without kennel's other screen hooks); it is simply unreachable, and its sound
 * table points at size-0 dummies. u1: the combo opens the settings menu instead (coop_settings.cc).
 *
 * u4: the synth and drum keys as modifiers (randomise, coop_random.cc). Stock resets the page to T1 when the mode key
 * is pressed on its own mode, so that press is deferred: coop keeps it (the combo's tracking still sees it held) and
 * on the release sends the stock a press + release only if nothing used the key while it was down. A tap behaves
 * as stock (back to T1), a hold is a modifier. From another mode the press goes straight through (a mode switch).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define KEY_DISPATCH   ((void (*)(int, int))0x019B6C74u)                          /* the stock key dispatch (code, pressed) */
#define CUR_MODE       (*(volatile int *)0xFF907D58u)                             /* 0 synth, 1 drum, 2 tape, 3 mixer */
#define SHIFT_HELD     (*(volatile uint8_t *)0x01DD7F78u)

volatile uint32_t g_lander_swallowed, g_mode_key_taps, g_mode_key_holds;
extern "C" int coop_ui_key(int code, int pressed);   /* coop_ui.cc: the slot 2 page and the settings menu */
extern "C" int coop_settings_open(void);             /* coop_settings.cc (u1): the combo opens the settings menu */
static uint8_t mode_keys;                                                         /* synth drum tape mixer held (bits 0-3) */
static uint8_t deferred, used;                                                    /* u4: synth / drum pressed on its own mode */
extern "C" uint8_t coop_mode_keys_held(void) { return mode_keys; }              /* u4: synth / drum held as a modifier */
extern "C" void coop_mode_key_used(void) { used = deferred; }

extern "C" void coop_key_event(int code, int pressed)
{
    if (coop_ui_key(code, pressed)) return;
    if (code >= 0x02 && code <= 0x05) {                                           /* the four mode keys */
        uint8_t bit = (uint8_t)(1u << (code - 2));
        if (!pressed) {
            mode_keys &= (uint8_t)~bit;
            if (deferred & bit) {                                                 /* the deferred press: a tap goes to stock now */
                deferred &= (uint8_t)~bit;
                if (used & bit) { used &= (uint8_t)~bit; g_mode_key_holds++; return; }
                g_mode_key_taps++;
                KEY_DISPATCH(code, 1);
                KEY_DISPATCH(code, 0);
                return;
            }
        }
        else if ((mode_keys | bit) == 0x0F && !(mode_keys & bit)) { mode_keys |= bit; g_lander_swallowed++; coop_settings_open(); return; }
        else {
            mode_keys |= bit;
            if (!SHIFT_HELD && ((code == 0x02 && CUR_MODE == 0) || (code == 0x03 && CUR_MODE == 1))) { deferred |= bit; return; }   /* on its own mode, no Shift: wait (Shift + synth is a stock combo on the press) */
        }
    }
    KEY_DISPATCH(code, pressed);
}
