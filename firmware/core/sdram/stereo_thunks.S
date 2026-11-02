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
    [--SP] = P2;                     /* the stock render; a C call clobbers P2 */
    [--SP] = R0;                     /* its arguments */
    [--SP] = R1;
    [--SP] = R2;
    SP += -12;
    R0 = R7;
    CALL _coop_lfo2_pre_render_synth; /* second LFO slot, stage 0: knobs / pitch / volume before the render */
    SP += 12;
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    P2 = [SP++];
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
    [--SP] = P0;                     /* the stock render; a C call clobbers P0 */
    [--SP] = R0;
    [--SP] = R1;
    [--SP] = R2;
    SP += -12;
    R0 = 12;
    R0 = R0 - R7;                    /* voice = 12 - R7 */
    CALL _coop_lfo2_pre_render_drum; /* second LFO slot, stage 3, before the render */
    SP += 12;
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    P0 = [SP++];
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

/* Entered by JUMP from the note-on trampoline (C2): RETS = 0xffa00e36 (the stock return), P0 = the
 * engine's note_on (*(vtable+0x1c)), R0/R1/R2 = its args, R7 = voice index. The stock code after the
 * call jumps straight back into the voice loop and reloads its registers from the frame. */
    .global _coop_synth_noteon_thunk
    .type   _coop_synth_noteon_thunk, STT_FUNC
_coop_synth_noteon_thunk:
    [--SP] = RETS;
    SP += -12;
    CALL (P0);
    R0 = R7;
    R1 = 1 (X);
    CALL _coop_synth_note_on_hook;
    SP += 12;
    RETS = [SP++];
    RTS;
    .size _coop_synth_noteon_thunk, .-_coop_synth_noteon_thunk

/* Stands in for Core::NoteOn (SDRAM 0x01967854, R0 = voice 0..5, returns 1 when it dispatched the
 * engine's note_on) at its three callers in VoiceCtl (0x01a8442c, 0x01a848e4, 0x01a8564e; CALL24
 * patches in the OP1X header, coop_cb.cc). Runs the stock function, then the hook with
 * (voice, return value), and returns the stock result. */
    .global _coop_core_noteon_thunk
    .type   _coop_core_noteon_thunk, STT_FUNC
_coop_core_noteon_thunk:
    [--SP] = RETS;
    [--SP] = R0;
    SP += -12;
    P1.H = 0x0196;
    P1.L = 0x7854;
    CALL (P1);
    SP += 12;
    R1 = R0;
    R0 = [SP++];
    [--SP] = R1;
    SP += -12;
    CALL _coop_synth_note_on_hook;
    SP += 12;
    R0 = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_core_noteon_thunk, .-_coop_core_noteon_thunk

/* CALLed from the mix trampoline (which saved the site's RETS): R0.L = gain, R1.L = ramp, R2.L = step,
 * P1 = the ramp state (W[P1]). R3 is dead here (loop B clobbered it). */
    .global _coop_mix_thunk
    .type   _coop_mix_thunk, STT_FUNC
_coop_mix_thunk:
    [--SP] = RETS;
    /* the stock loops leave R0 and R2 as they were (loop A: gains/step; loop B: gain); the code after loop A
     * uses R2.L as the loop B step when its branch skips */
    [--SP] = R0;
    [--SP] = R2;
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
    R3 = [SP + 68];                  /* the RETS the trampoline pushed = the site + 4: tells loop A from loop B */
    P0.H = _g_mix_site;
    P0.L = _g_mix_site;
    [P0] = R3;
    R3 = [SP + 132];                 /* loop A keeps its ramp/meter record pointer at [SP + 0x3c] of the site (72 B above) */
    P0.H = _g_mix_a_rec;
    P0.L = _g_mix_a_rec;
    [P0] = R3;
    SP += -12;
    CALL _coop_mix_entry;
    SP += 12;
    R1 = R0;                         /* R1 as the stock loop leaves it (loop B: the ramp; loop A: .H = ramp, .L kept) */
    /* P0 as the stock loop leaves it: loop B keeps its count 0x40 (the code after it relies on that; a C
     * clobber of P0 shifted the tape by a block), loop A 0x01b4ef6e */
    P0.H = _g_mix_p0_out;
    P0.L = _g_mix_p0_out;
    P0 = [P0];
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
    R2 = [SP++];
    R0 = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_mix_thunk, .-_coop_mix_thunk

/* ---- stereo tape (tape_stereo.cc) ---- */

/* Stands in for the two `CALL 0xffa06334` (l1_tape_record) sites: entered by JUMP from the tape_rec
 * trampoline with RETS = the stock return and R0 = the mono input pointer. The stock function is a normal
 * callee (R7:4, P5:3 saved); coop_tape_record calls it twice and honours the ABI, so only the DAG and
 * accumulator state that compiled code may keep live across a CALL is saved here as in the mix thunk. */
    .global _coop_tape_rec_thunk
    .type   _coop_tape_rec_thunk, STT_FUNC
_coop_tape_rec_thunk:
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
    SP += -12;
    CALL _coop_tape_record;
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

/* Entered by JUMP from the tape_gains trampoline, which the tick reaches through a CALL that replaces the
 * 8-byte `P1.L = 0x17f0; P1.H = 0x18c` at 0xffa06de0, right after the stock pan law. Inline code: every
 * register but P1 is preserved, including ASTAT. coop_tape_gains gets the SP the tick had (its frame
 * holds the gain and meter pointers) and leaves P1 = 0x018c17f0 as the displaced instructions would. */
    .global _coop_tape_gains_thunk
    .type   _coop_tape_gains_thunk, STT_FUNC
_coop_tape_gains_thunk:
    [--SP] = RETS;
    [--SP] = ASTAT;
    [--SP] = (R7:0, P5:0);
    [--SP] = I0;
    [--SP] = I1;
    [--SP] = I2;
    [--SP] = I3;
    [--SP] = B0;
    [--SP] = L0;
    [--SP] = M0;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
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
    R0 = SP;
    R1 = 132 (X);                    /* back to the SP at entry: RETS 4 + ASTAT 4 + (R7:0,P5:0) 56 + I0-I3 16 + B0/L0/M0 12 + LC/LT/LB x2 24 + A0/A1 16 */
    R0 = R0 + R1;
    SP += -12;
    CALL _coop_tape_gains;
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
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    M0 = [SP++];
    L0 = [SP++];
    B0 = [SP++];
    I3 = [SP++];
    I2 = [SP++];
    I1 = [SP++];
    I0 = [SP++];
    (R7:0, P5:0) = [SP++];
    ASTAT = [SP++];
    RETS = [SP++];
    P1.L = 0x17f0;
    P1.H = 0x18c;
    RTS;

/* Hook B of the stereo tape: replaces the 8-byte `P2.L = 0xef7c; P2.H = 0x1b4` at 0xffa0706e, right before the
 * seam mix loop. Inline code: everything but P2 is preserved; P2 = the 8-entry staging table coop_seam_prologue
 * returns. */
    .global _coop_seam_prologue_thunk
    .type   _coop_seam_prologue_thunk, STT_FUNC
_coop_seam_prologue_thunk:
    [--SP] = RETS;
    [--SP] = ASTAT;
    [--SP] = (R7:0, P5:0);
    [--SP] = I0;
    [--SP] = I1;
    [--SP] = I2;
    [--SP] = I3;
    [--SP] = B0;
    [--SP] = L0;
    [--SP] = M0;
    [--SP] = LC0;
    [--SP] = LT0;
    [--SP] = LB0;
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
    SP += -12;
    CALL _coop_seam_prologue;
    SP += 12;
    P2 = R0;
    [SP + 76] = P2;                  /* the saved P2 slot: 68 B of DAG/loop/acc pushes above SP, then P0, P1, P2 of (R7:0, P5:0) */
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
    LB0 = [SP++];
    LT0 = [SP++];
    LC0 = [SP++];
    M0 = [SP++];
    L0 = [SP++];
    B0 = [SP++];
    I3 = [SP++];
    I2 = [SP++];
    I1 = [SP++];
    I0 = [SP++];
    (R7:0, P5:0) = [SP++];
    ASTAT = [SP++];
    RETS = [SP++];
    RTS;
