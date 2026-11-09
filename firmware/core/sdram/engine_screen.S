/*
 * coop-fw E2 (docs/engines.md "Screens"): ScreenId::for_synth's case 14 (iter). The jump-table word 0x018F8074
 * (stock 0x01A3BFD0) points here. Entered inside for_synth's frame (LINK still active), like the stock case code:
 * compute R0 (the screen id), then P0 = [FP + 4]; UNLINK; JUMP (P0). kennel 3.1 hook.S synth_screen_case14.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text
    .align 2
    .global _coop_synth_screen_case14
    .type   _coop_synth_screen_case14, STT_FUNC
_coop_synth_screen_case14:
    CALL _coop_synth_screen_select;
    P0 = [FP + 4];
    UNLINK;
    JUMP (P0);
    .size _coop_synth_screen_case14, .-_coop_synth_screen_case14
