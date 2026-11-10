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
    .section .text.coop_voice,"ax",@progbits   /* v1: the per-voice path, contiguous (coop_cost.h COOP_HOT_VOICE) */
    .global _coop_synth_render_thunk
    .type   _coop_synth_render_thunk, STT_FUNC
_coop_synth_render_thunk:
    [--SP] = RETS;
    [--SP] = P2;                     /* the stock render; a C call clobbers P2 */
    [--SP] = R0;                     /* its arguments */
    [--SP] = R1;
    [--SP] = R2;
    SP += -12;
    CALL _coop_m1_thunk_in;          /* m1: the voice's entry mark, only while the gap profile runs (stereo_bus.cc) */
    R0 = R7;
    CALL _coop_lfo2_pre_render_synth; /* second LFO slot, stage 0: knobs / pitch / volume before the render */
    CALL _coop_eng_pf_start;         /* p6e: the performance counters at the render's start (coop_stats.cc) */
    SP += 12;
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    P2 = [SP++];
    [--SP] = P2;                     /* the render names the engine for the cost line */
    R3 = CYCLES;                     /* R3 is no argument and was already clobbered by the C call above */
    [--SP] = R3;
    SP += -12;
#ifdef COOP_VOLTAGE_CHECK
    P1.L = 0x9bf4; P1.H = 0x01a4;    /* test builds: the Voltage render runs through the lockstep check */
    CC = P2 == P1;
    P1.L = _coop_voltage_check; P1.H = _coop_voltage_check;
    IF CC P2 = P1;
#endif
#ifdef COOP_PHASE_CHECK
    P1.L = 0xbc5c; P1.H = 0x01a0;    /* test builds: the Phase render runs through the lockstep check */
    CC = P2 == P1;
    P1.L = _coop_phase_check; P1.H = _coop_phase_check;
    IF CC P2 = P1;
#endif
    CALL (P2);
    R1 = CYCLES;
    SP += 12;
    R3 = [SP++];
    R1 = R1 - R3;                    /* the render's cycles, interrupts included */
    R0 = [SP++];
    R2 = 0;
    SP += -12;
    CALL _coop_eng_cost;             /* (render, cycles, 0 = synth) */
    R0 = R7;
    CALL _coop_synth_voice_hook;
    SP += 12;
    RETS = [SP++];
    RTS;
    .size _coop_synth_render_thunk, .-_coop_synth_render_thunk
    .text

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
    CALL _coop_eng_pf_start;         /* p6e: the counters at the render's start */
    SP += 12;
    R2 = [SP++];
    R1 = [SP++];
    R0 = [SP++];
    P0 = [SP++];
    [--SP] = P0;                     /* the render names the engine for the cost line */
    R3 = CYCLES;
    [--SP] = R3;
    SP += -12;
    CALL (P0);
    R1 = CYCLES;
    SP += 12;
    R3 = [SP++];
    R1 = R1 - R3;
    P0 = [SP++];
    [--SP] = R0;                     /* the render's result, which the stock caller tests */
    SP += -12;
    R0 = P0;
    R2 = 1;
    CALL _coop_eng_cost;             /* (render, cycles, 1 = drum) */
    R1 = [SP+12];                    /* active: the stock sums the voice only when the render returned nonzero */
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

/* S3 (docs/stereo-bus-v2.md): CALLed from the inline far call that replaces `CALL 0xffa005d4` (the stock decimator)
 * + `P1 = 0x019674f8; CALL (P1)` (the stock DC block) at 0xffa01eb4, 0xffa02456, 0xffa02888:
 *     [--SP] = RETS; P0 = coop_instr_thunk; CALL (P0); RETS = [SP++]
 * The stock pair clobbers R0-R2, P0-P2 and the DAG / loop / accumulator registers, and the site at 0xffa02888 keeps
 * M1 across it, so everything compiled code may touch besides R0-R3 / P0-P2 is saved; L0-L3 are zeroed for the C
 * code (the ABI assumes linear addressing). g_coop_stereo_enable = 0 runs the stock pair instead. */
    .global _coop_instr_thunk
    .type   _coop_instr_thunk, STT_FUNC
_coop_instr_thunk:
    [--SP] = RETS;
    [--SP] = ASTAT;
    [--SP] = I0; [--SP] = I1; [--SP] = I2; [--SP] = I3;
    [--SP] = B0; [--SP] = B1; [--SP] = B2; [--SP] = B3;
    [--SP] = L0; [--SP] = L1; [--SP] = L2; [--SP] = L3;
    [--SP] = M0; [--SP] = M1; [--SP] = M2; [--SP] = M3;
    [--SP] = LC0; [--SP] = LT0; [--SP] = LB0;
    [--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
    R3 = A0.W; [--SP] = R3; R3 = A0.X; [--SP] = R3;
    R3 = A1.W; [--SP] = R3; R3 = A1.X; [--SP] = R3;
    L0 = 0 (X); L1 = 0 (X); L2 = 0 (X); L3 = 0 (X);
    P1.H = _g_coop_stereo_enable;
    P1.L = _g_coop_stereo_enable;
    R0 = [P1];
    CC = R0 == 0;
    IF CC JUMP 1f;
    SP += -12;
    P1.H = _g_coop_dbg_cmp;          /* debug (op1emu): also run the stock pair, coop_instr_stage compares */
    P1.L = _g_coop_dbg_cmp;
    R0 = [P1];
    CC = R0 == 0;
    IF CC JUMP 3f;
    CALL _coop_dbg_pre;
    P1.H = 0xffa0;
    P1.L = 0x05d4;
    CALL (P1);
    P1.H = 0x0196;
    P1.L = 0x74f8;
    CALL (P1);
3:  CALL _coop_instr_stage;
    SP += 12;
    JUMP 2f;
1:  SP += -12;
    P1.H = 0xffa0;                   /* the stock pair, as the site had it */
    P1.L = 0x05d4;
    CALL (P1);
    P1.H = 0x0196;
    P1.L = 0x74f8;
    CALL (P1);
    SP += 12;
2:  R3 = [SP++]; A1.X = R3; R3 = [SP++]; A1.W = R3;
    R3 = [SP++]; A0.X = R3; R3 = [SP++]; A0.W = R3;
    LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++];
    LB0 = [SP++]; LT0 = [SP++]; LC0 = [SP++];
    M3 = [SP++]; M2 = [SP++]; M1 = [SP++]; M0 = [SP++];
    L3 = [SP++]; L2 = [SP++]; L1 = [SP++]; L0 = [SP++];
    B3 = [SP++]; B2 = [SP++]; B1 = [SP++]; B0 = [SP++];
    I3 = [SP++]; I2 = [SP++]; I1 = [SP++]; I0 = [SP++];
    ASTAT = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_instr_thunk, .-_coop_instr_thunk

/* S3: the drum path's bus stage call at 0xffa02364 (`P1 = 0x01a66922; CALL (P1)`, R0 = state, R1 = bus, R2 = params)
 * is retargeted here (P1's immediates). The stock callee is compiled C; the L1 code after the call reloads what it
 * uses, but the DAG / loop / accumulator state is saved as for the instrument stage, to be safe. */
    .global _coop_drumbus_thunk
    .type   _coop_drumbus_thunk, STT_FUNC
_coop_drumbus_thunk:
    [--SP] = RETS;
    [--SP] = ASTAT;
    [--SP] = I0; [--SP] = I1; [--SP] = I2; [--SP] = I3;
    [--SP] = B0; [--SP] = B1; [--SP] = B2; [--SP] = B3;
    [--SP] = L0; [--SP] = L1; [--SP] = L2; [--SP] = L3;
    [--SP] = M0; [--SP] = M1; [--SP] = M2; [--SP] = M3;
    [--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
    R3 = A0.W; [--SP] = R3; R3 = A0.X; [--SP] = R3;
    R3 = A1.W; [--SP] = R3; R3 = A1.X; [--SP] = R3;
    L0 = 0 (X); L1 = 0 (X); L2 = 0 (X); L3 = 0 (X);
    SP += -12;
    CALL _coop_drum_bus;
    SP += 12;
    R3 = [SP++]; A1.X = R3; R3 = [SP++]; A1.W = R3;
    R3 = [SP++]; A0.X = R3; R3 = [SP++]; A0.W = R3;
    LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++];
    M3 = [SP++]; M2 = [SP++]; M1 = [SP++]; M0 = [SP++];
    L3 = [SP++]; L2 = [SP++]; L1 = [SP++]; L0 = [SP++];
    B3 = [SP++]; B2 = [SP++]; B1 = [SP++]; B0 = [SP++];
    I3 = [SP++]; I2 = [SP++]; I1 = [SP++]; I0 = [SP++];
    ASTAT = [SP++];
    RETS = [SP++];
    RTS;
    .size _coop_drumbus_thunk, .-_coop_drumbus_thunk

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
    [SP + 80] = P2;                  /* the saved P2 slot: 68 B of DAG/loop/acc pushes above SP, then P5, P4, P3, P2 of (R7:0, P5:0)
                                      * (push multiple stores R0 first and P5 last: P5 at [SP], P2 at [SP + 12], R0 at [SP + 52];
                                      * checked in the GNU simulator, docs/tape-stereo.md section 11) */
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

/* ---- own reverb memory per stereo grid / spring object (clean-up after S2, docs/stereo-bus-v2.md) ----
 * The stereo grid and spring classes load their tank address as an immediate pair in their render and init. Each of
 * those four 8-byte pairs becomes a JUMP.L here (tools/l1_stereo_insert.py, TANK_SITES); the thunk looks the object
 * up in a map kept by plugin_host.cc, puts that object's tank (or the stock tank for any other object, the stock
 * master's included) into the register the stock code expects, and jumps back after the pair. Map: 8 words,
 * { obj0, tank0, obj1, tank1, obj2, tank2, 0, stock tank }. Only ASTAT and the scratch registers are touched, and
 * each is restored except the destination. No CALL: the grid init does not LINK, so RETS must survive. */
/* The return addresses come from the link (core/sdram/Makefile, --defsym): the assembler cannot reach an absolute
 * address from a section-relative JUMP.L. _stock_grid_render_back 0x019af84e, _stock_grid_init_back 0x019af294,
 * _stock_spring_render_back 0x01a3300a, _stock_spring_init_back 0x01a336b8. */

/* In region C (.near, coop_cb.ld): region A, where the rest of coop lives, is beyond JUMP.L reach of the stock. */
    .section .near, "ax"
/* R0 = this -> R0 = its tank; clobbers R1, P0, CC */
.macro TANK_LOOKUP map
    P0.L = \map; P0.H = \map;
    R1 = [P0++]; CC = R0 == R1; IF CC JUMP 1f; P0 += 4;
    R1 = [P0++]; CC = R0 == R1; IF CC JUMP 1f; P0 += 4;
    R1 = [P0++]; CC = R0 == R1; IF CC JUMP 1f; P0 += 4;
    P0 += 4;
1:  R0 = [P0];
.endm

/* grid stereo render 0x019af83c: at 0x019af846 `R7.L = 0xc70c; R7.H = 0x1d8`, R0 = this */
    .global _coop_tank_grid_render
    .type   _coop_tank_grid_render, STT_FUNC
_coop_tank_grid_render:
    [--SP] = ASTAT; [--SP] = R0; [--SP] = R1; [--SP] = P0;
    TANK_LOOKUP _g_coop_tank_grid
    R7 = R0;
    P0 = [SP++]; R1 = [SP++]; R0 = [SP++]; ASTAT = [SP++];
    JUMP.L _stock_grid_render_back;
    .size _coop_tank_grid_render, .-_coop_tank_grid_render

/* grid stereo init 0x019af27e: at 0x019af28c `I0.L = 0xc70c; I0.H = 0x1d8`, P2 = this, R0 = 0 (kept) */
    .global _coop_tank_grid_init
    .type   _coop_tank_grid_init, STT_FUNC
_coop_tank_grid_init:
    [--SP] = ASTAT; [--SP] = R0; [--SP] = R1; [--SP] = P0;
    R0 = P2;
    TANK_LOOKUP _g_coop_tank_grid
    I0 = R0;
    P0 = [SP++]; R1 = [SP++]; R0 = [SP++]; ASTAT = [SP++];
    JUMP.L _stock_grid_init_back;
    .size _coop_tank_grid_init, .-_coop_tank_grid_init

/* spring stereo render 0x01a32ffc: at 0x01a33002 `P1.L = 0x6252; P1.H = 0x1de`, R0 = this */
    .global _coop_tank_spring_render
    .type   _coop_tank_spring_render, STT_FUNC
_coop_tank_spring_render:
    [--SP] = ASTAT; [--SP] = R0; [--SP] = R1; [--SP] = P0;
    TANK_LOOKUP _g_coop_tank_spring
    P1 = R0;
    P0 = [SP++]; R1 = [SP++]; R0 = [SP++]; ASTAT = [SP++];
    JUMP.L _stock_spring_render_back;
    .size _coop_tank_spring_render, .-_coop_tank_spring_render

/* spring stereo init 0x01a336ae: at 0x01a336b0 `P0.L = 0x6252; P0.H = 0x1de`, R0 = P1 = this */
    .global _coop_tank_spring_init
    .type   _coop_tank_spring_init, STT_FUNC
_coop_tank_spring_init:
    [--SP] = ASTAT; [--SP] = R0; [--SP] = R1;
    TANK_LOOKUP _g_coop_tank_spring
    P0 = R0;
    R1 = [SP++]; R0 = [SP++]; ASTAT = [SP++];
    JUMP.L _stock_spring_init_back;
    .size _coop_tank_spring_init, .-_coop_tank_spring_init
    .text

/* ---- the stock DSynth voice loop, rewritten (performance, 2026-11-04) ----
 * DSynth's render (0x0197E1D8) finishes each voice with a 128-sample loop (0x0197E974..0x0197EA6A): out = buf * gain *
 * env, << 1 (S); a one-pole lowpass with 32-bit state (s += c * (v - s), as two 32 x 32 products); x 8 (S); a DC
 * blocker (y = c2 * (y + w - w_prev)). The compiled loop is a software loop with four CALLs per sample to one-line
 * accessors (0x0197EF08 / EF1C / EF2C / EF40) and every intermediate value through the stack. This is the same
 * arithmetic, instruction for instruction (the 32 x 32 multiply sequences are the stock's), in a hardware loop with
 * the state in registers. Entry: JUMP.L from 0x0197E956, which every path into the loop goes through (0x0197E882,
 * 0x0197EB60 ... 0x0197EE00); R7 = the lowpass coefficient, [FP-0x2C] the gain (low half), [FP-0x10] the DC
 * coefficient, P5 = the voice object, [SP+0x30] the buffer, the envelope at L1 0xFF801D50. Exit: JUMP.L 0x0197EA6C,
 * which reads only P0 / P1 (= the object) and the stack words [FP-0x20] / [FP-0x34] set here; R4-R7 are restored by
 * the function's epilogue. Uses LC0 (the function itself sets up LC0 loops) and M0 (call-clobbered). */
    .section .near, "ax"
    .global _coop_dsynth_voice_loop
    .type   _coop_dsynth_voice_loop, STT_FUNC
_coop_dsynth_voice_loop:
    P1.L = _g_coop_fast_dsynth; P1.H = _g_coop_fast_dsynth;   /* 0: the stock loop (A/B, kill switch). R0 and CC */
    R0 = [P1];                                                 /* are free here: the code at 0x0197E95A sets both */
    CC = R0 == 0;
    IF CC JUMP 3f;
    P1 = FP;
    P1 += -10;
    [FP + -0x20] = P5;
    [FP + -0x34] = P1;
    P5 += 8;
    P3 = [SP + 0x30];
    P4.L = 0x1d50; P4.H = 0xff80;
    R0 = [FP + -0x2c];
    M0 = R0;                                       /* the gain (low half) */
    R6 = [FP + -0x10];                             /* the DC coefficient */
    R4 = [P5 + 0x2c];                              /* lowpass state s */
    R5 = [P5 + 0x38];                              /* DC state y */
    R3 = [P5 + 0x3c];                              /* DC input w_prev << 16 */
    P0 = 128;
    LSETUP (1f, 2f) LC0 = P0;
1:  R1 = W[P3] (X);
    R2 = M0;
    R1.L = R1.L * R2.L || R0 = W[P4++] (X) || NOP;
    R1.L = R1.L * R0.L;
    R1.H = R1.L << 1 (S);
    R1 = R1 >>> 16;
    R2 = R1 << 16;                                 /* v << 16 */
    A1 = R2.L * R7.L (FU);                         /* v * c */
    A1 = A1 >> 16;
    A1 += R2.H * R7.L (M), A0 = R2.H * R7.H;
    A1 += R7.H * R2.L (M);
    A1 = A1 >>> 15;
    R2 = (A0 += A1);
    R0 = -R4 (S);                                  /* -s * c */
    A1 = R0.L * R7.L (FU);
    A1 = A1 >> 16;
    A1 += R0.H * R7.L (M), A0 = R0.H * R7.H;
    A1 += R7.H * R0.L (M);
    A1 = A1 >>> 15;
    R0 = (A0 += A1);
    R2 = R4 + R2 (S);
    R4 = R2 + R0 (S);                              /* s' */
    R0 = R4 >>> 16;
    R2.L = R0.L << 3 (S);                          /* w */
    R2 = R2 << 16;
    R1 = R2 + R5 (S);
    R1 = R1 - R3 (S);
    R3 = R2;
    A1 = R6.L * R1.L (FU);                         /* y' = c2 * (y + w - w_prev) */
    A1 = A1 >> 16;
    A1 += R6.H * R1.L (M), A0 = R6.H * R1.H;
    A1 += R1.H * R6.L (M);
    A1 = A1 >>> 15;
    R5 = (A0 += A1);
    W[P3] = R5.H;
2:  P3 += 2;
    [P5 + 0x2c] = R4;
    [P5 + 0x38] = R5;
    [P5 + 0x3c] = R3;
    P1 = [FP + -0x20];
    P0 = P1;
    JUMP.L _stock_dsynth_loop_back;
3:  P1 = FP;                                       /* the two replaced instructions, then the stock loop */
    P1 += -10;
    JUMP.L _stock_dsynth_loop_entry;
    .size _coop_dsynth_voice_loop, .-_coop_dsynth_voice_loop
    .text
