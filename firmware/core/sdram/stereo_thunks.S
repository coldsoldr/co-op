/* coop-fw stereo bus: SDRAM thunks between the rewritten L1 sites and the C hooks (stereo_bus.cc).
 *
 * The L1 trampolines (tools/l1_stereo_insert.py, free L1 0xffa0bfb4..) reach these with the stock
 * registers intact. Blackfin ABI: R0-R2 carry arguments, the caller reserves 12 bytes of outgoing
 * argument space below its pushes (a callee may spill R0-R2 there), R4-R7/P3-P5/FP are callee-saved.
 * The render thunks are as safe as the stock `CALL (Px)` they replace: everything the stock code keeps
 * live across the render is in callee-saved registers or memory, and our C hooks honour the ABI.
 * The mix thunk replaces INLINE loop code, so it also saves the DAG/accumulator state compiled code may
 * clobber (I0-I3, B0, L0, M0, A0, A1, LC1/LT1/LB1); the stock code after loop B reloads everything
 * else it uses (docs/stereo-bus.md "C1 takeover mechanism").
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text

/* Entered by JUMP from the synth trampoline: RETS = 0xffa00d2a (the stock return), P2 = the stock
 * render (*(vtable+0x14)), R0/R1/R2 = render args, R7 = voice index. */
    .global _coop_synth_render_thunk
    .type   _coop_synth_render_thunk, STT_FUNC
_coop_synth_render_thunk:
    [--SP] = RETS;
    SP += -12;
    CALL (P2);
    R0 = R7;
    CALL _coop_synth_voice_hook;
    SP += 12;
    RETS = [SP++];
    RTS;
    .size _coop_synth_render_thunk, .-_coop_synth_render_thunk

/* Entered by JUMP from the drum trampoline: RETS = 0xffa00906, P0 = the stock render
 * (*(vtable+0x24)), R7 = 12 - voice. The stock caller tests the render's R0 (0xffa00906), so the
 * return value is preserved across the hook. */
    .global _coop_drum_render_thunk
    .type   _coop_drum_render_thunk, STT_FUNC
_coop_drum_render_thunk:
    [--SP] = RETS;
    SP += -12;
    CALL (P0);
    SP += 12;
    [--SP] = R0;
    SP += -12;
    R0 = R7;
    CALL _coop_drum_voice_hook;
    SP += 12;
    R0 = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_drum_render_thunk, .-_coop_drum_render_thunk

/* CALLed from the mix trampoline (which saved the site's RETS): R0.L = gain, R1.L = ramp, R2.L = step,
 * P1 = the ramp state (W[P1]). R3 is dead here (loop B clobbered it). */
    .global _coop_mix_thunk
    .type   _coop_mix_thunk, STT_FUNC
_coop_mix_thunk:
    [--SP] = RETS;
    [--SP] = I0;
    [--SP] = I1;
    [--SP] = I2;
    [--SP] = I3;
    [--SP] = B0;
    [--SP] = L0;
    [--SP] = M0;
    [--SP] = LC1;
    [--SP] = LT1;
    [--SP] = LB1;
    R3 = A0.W;
    [--SP] = R3;
    R3 = A0.X;
    [--SP] = R3;
    R3 = A1.W;
    [--SP] = R3;
    R3 = A1.X;
    [--SP] = R3;
    P0.H = _g_mix_ramp_ptr;
    P0.L = _g_mix_ramp_ptr;
    [P0] = P1;
    SP += -12;
    CALL _coop_mix_entry;
    SP += 12;
    R3 = [SP++];
    A1.X = R3;
    R3 = [SP++];
    A1.W = R3;
    R3 = [SP++];
    A0.X = R3;
    R3 = [SP++];
    A0.W = R3;
    LB1 = [SP++];
    LT1 = [SP++];
    LC1 = [SP++];
    M0 = [SP++];
    L0 = [SP++];
    B0 = [SP++];
    I3 = [SP++];
    I2 = [SP++];
    I1 = [SP++];
    I0 = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_mix_thunk, .-_coop_mix_thunk
