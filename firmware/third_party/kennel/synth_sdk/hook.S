/* 4-byte JUMP.L placed at 0x019B975C (instr_types name compare) -> instr_name_eq */
	.section .hook_namecmp,"ax"
	.global __hook_namecmp
__hook_namecmp:
	jump.l _instr_name_eq;

/* --dev only: CALL access("/yaffs2/te-boot.op1") at boot (0x019F01B8) -> du_boot_access (devupd/du_fw.c) */
	.section .hook_devrx,"ax"
	.global __hook_devrx
__hook_devrx:
	call _du_rx_poll;
	.section .hook_devboot,"ax"
	call _du_boot_access;

/* CALL at 0x01A01844 (patch::sample_path "sampler" check) -> sample_type_eq */
	.section .hook_samplecheck,"ax"
	.global __hook_samplecheck
__hook_samplecheck:
	call _sample_type_eq;

/* JUMP.L at 0x019AAD8E (fx_types name compare) -> fx_name_eq */
	.section .hook_fxnamecmp,"ax"
	.global __hook_fxnamecmp
__hook_fxnamecmp:
	jump.l _fx_name_eq;

/* ScreenId::for_fx jump-table case 5 target. Entered with the for_fx frame (LINK 0xc) still active,
 * like the original case code: compute R0, then  P0=[FP+4]; UNLINK; JUMP (P0). */
	.text
	.global _fx_screen_case5
	.align 2
_fx_screen_case5:
	call _fx_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

	.section .hook_iterread,"ax"
	call _iter_read_hook;
	.section .hook_screenctor,"ax"
	call _screens_ctor_hook;
	.section .hook_screenload,"ax"
	call _screens_load_hook;

/* ScreenId::for_synth jump-table case 14 (iter) target, same frame convention as fx_screen_case5 */
	.text
	.global _synth_screen_case14
	.align 2
_synth_screen_case14:
	call _synth_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

/* ---- optional OP-Z controller mode (make_firmware.py --opz), docs/12 ----
 * Flag 0x018B6016 = OP-Z host mode. Stock fw clears it on COM->CTRL entry (midi_send_identity_request)
 * and on every TE host-mode SysEx F0 00 20 76 00 01 <m> F7, but never sets or reads it.
 * midi_opz_mode_enabled (0x019E0CFC, stub "return 0") -> return the flag. */
	.section .hook_opzget,"ax"
	jump.l _opz_mode_enabled;
	.text
	.align 2
	.global _opz_mode_enabled
_opz_mode_enabled:
	P1.L = 0x6016;
	P1.H = 0x018B;
	R0 = B[P1] (Z);
	rts;

/* midi_rx_parse, host-mode SysEx branch for m != 1,2 (0x019E3540, was B[P4+9] = R5).
 * Entry: P4 = 0x018B600C (+8 ableton, +9 reason, +A opz), R0 = m. m == 3 -> OP-Z mode.
 * (stock code stores R5 here, assumed 0 - but a preceding 00 04 SysEx can leave R5 = 1) */
	.section .hook_opzset,"ax"
	jump.l _opz_hostmode_other;
	.text
	.align 2
_opz_hostmode_other:
	CC = R0 == 3;
	R0 = CC;
	B[P4 + 10] = R0;
	R0 = 0;
	B[P4 + 9] = R0;
	jump.l _midi_rx_parse_next;

/* every CALL read_fx_param (0x01A28DA0) in the firmware -> read_fx_param_safe (dispatcher.c):
 * clamps custom-FX knob words into the filter ranges first, so the range assert can never fire. */
	.section .hook_rfp019427b8,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01984f60,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019a74d4,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019b05f8,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019e6a48,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp019e832c,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01a1cfd2,"ax"
	call _read_fx_param_safe;
	.section .hook_rfp01a33fb2,"ax"
	call _read_fx_param_safe;

/* KeyHandler::process_queue dispatch -> encoder press / turn handling for custom FX pages (dispatcher.c) */
	.section .hook_keyev,"ax"
	call _key_event_hook;
	.section .hook_knobev,"ax"
	call _knob_event_hook;
/* VoiceCtl key_down / key_up: CALL voicectl_synth_note_on / _note_off -> the features' note pipeline (kennel/kn_features.c) */
	.section .hook_chordon,"ax"
	call _note_pipe_on;
	.section .hook_chordoff,"ax"
	call _note_pipe_off;
/* midi_rx_parse FA (MIDI START) branch: CALL 0x01A5E7D0 (reset beat counters) -> midi_start_hook (dispatcher.c) */
	.section .hook_midistart,"ax"
	call _midi_start_hook;
/* VoiceCtl::on_midi_rx 0xB0 branch: CALL midi_message (0x01A84F8C) -> midi_cc_hook (dispatcher.c) */
	.section .hook_midicc,"ax"
	call _midi_cc_hook;

/* ---- quad LFO (lfo/quadlfo.c) ---- */
/* lfo_types name compares (from_name 0x019C9C3A, is_valid 0x019C9BE2): "midi" also matches "quad" */
	.section .hook_lfoname1,"ax"
	call _lfo_name_eq;
	.section .hook_lfoname2,"ax"
	call _lfo_name_eq;
/* every CALL read_lfo_param (0x01A293BC) -> read_lfo_param_safe (quad words would trip the range assert) */
	.section .hook_rlp0193de2a,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01968c34,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01981eda,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp019e4a18,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a2d388,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a30c3c,"ax"
	call _read_lfo_param_safe;
	.section .hook_rlp01a48fe8,"ax"
	call _read_lfo_param_safe;

/* ScreenId::for_lfo case 4 (midi) target, same frame convention as fx_screen_case5 */
	.text
	.global _lfo_screen_case4
	.align 2
_lfo_screen_case4:
	call _lfo_screen_select;
	P0 = [FP + 4];
	unlink;
	jump (P0);

/* LfoMidi vtable apply entries (synth 0x01910840, drum 0x01910860, FX 0x01910880) -> here.
 * Entry R0 = this, R1 = LFO param words, R2 = pass. Words of a feature LFO type (quad; kennel/kn_lfo.c) -> its
 * code, anything else -> tail jump to the original LfoMidi method with registers and stack untouched. */
.macro QL_STUB name, orig, ours
	.global \name
	.align 2
\name:
	[--SP] = RETS;
	[--SP] = R0;
	[--SP] = R1;
	[--SP] = R2;
	SP += -12;
	R0 = R1;
	call _kn_lfo_claims;
	SP += 12;
	CC = R0 == 0;
	R2 = [SP++];
	R1 = [SP++];
	R0 = [SP++];
	RETS = [SP++];
	IF !CC JUMP 1f;
	jump.l \orig;
1:
	jump.l \ours;
.endm
	QL_STUB _ql_vt_synth, _midi_apply_synth, _kn_lfo_apply_synth
	QL_STUB _ql_vt_fx, _midi_apply_fx, _kn_lfo_apply_fx
	QL_STUB _ql_vt_drum, _midi_apply_drum, _kn_lfo_apply_drum
	.global _ql_apply_none
_ql_apply_none:
	R0 = 0;
	rts;

/* boot: CALL Clips::load (0x019F02EE) -> + kennel plugin loader; CALL MasterController::load (0x019F054A) -> + master
 * FX by name (kennel/kn_boot.c) */
	.section .hook_knboot,"ax"
	call _kn_boot_hook;
	.section .hook_knmaster,"ax"
	call _kn_master_load;
/* power-off / restart: R246's shutdown write-back loop CALL tape_c::service(1) 0x01A54710 (0x019F0BE2) -> one bounded
 * flush of every modified tape chunk, held ones included, then R0 = 0 ends the loop (kennel/kn_tape.c kn_tape_final) */
	.section .hook_tapefinal,"ax"
	call _kn_tape_final;
/* power-off / restart: R246's shutdown CALL yaffs unmount 0x019964B4 (0x019F0BEA) -> + crash marker (kennel/kn_boot.c) */
	.section .hook_knunmount,"ax"
	call _kn_boot_unmount;
/* main loop: CALL yaffs background GC 0x01A9AC48 (0x019F0C46) -> at most once per 0.1 s, as op1r 49 (kennel/kn_boot.c) */
	.section .hook_gcrate,"ax"
	call _kn_gc_rate;
/* delayed patch saves (bgsave.c): the SaveSlotJob queue entry 0x019411B8 (LINK 0) -> bgs_queue_slot, which only notes
 * (kind, slot); bgs_queue_stock is the original (its LINK here, then on at 0x019411BC). Shutdown: CALL BgFileWorker
 * stop 0x019412D4 (0x019F0BDC) -> kn_bgs_stop (queue what is noted, wait for the ring, then stop). */
	.section .hook_bgsqueue,"ax"
	jump.l _bgs_queue_slot;
	.section .hook_bgsstop,"ax"
	call _kn_bgs_stop;
	.text
	.global _bgs_queue_stock
	.align 2
_bgs_queue_stock:
	LINK 0;
	jump.l _bgs_queue_cont;
/* every frame: gfx_render_frame's CALL screens_update (0x019B2690) -> + toast (kennel/kn_core.c) */
	.section .hook_knscreens,"ax"
	call _kn_screens_update;

/* parse_patch_json (0x01A037A8) type lookups -> engine / FX picked by the saved type name (dispatcher.c,
 * docs/preset-identity.md). The parser keeps its source JSON at [FP - 0x78]; the callee has not linked yet, so FP is
 * still the parser's. The stubs pass it as the second argument and jump on: the C function returns to the parser. */
	.section .hook_patchtype,"ax"
	call _patch_type_stub;
	.section .hook_fxpatchtype,"ax"
	call _fx_patch_type_stub;
	.text
	.global _patch_type_stub
_patch_type_stub:
	R1 = [FP - 120];
	jump.l _patch_type_hook;
	.global _fx_patch_type_stub
_fx_patch_type_stub:
	R1 = [FP - 120];
	jump.l _fx_patch_type_hook;

/* parse_patch_json LFO word range check (0x01A042A2) -> lfo_range_ok (accepts quad words, dispatcher.c) */
	.section .hook_lforange,"ax"
	call _lfo_range_ok;

/* PlayerManager::create_player case 3 (CALL 0x019ED460 finger singleton @0x01A0D150) -> finger, euclid or dots */
	.section .hook_euplayer,"ax"
	call _seq_player_get;

/* SeqIds::from_name CALLs -> eu_seq_from_name ("euclid" = type 3 + flag) */
	.section .hook_seqname1,"ax"
	call _eu_seq_from_name;
	.section .hook_seqname2,"ax"
	call _eu_seq_from_name;
	.section .hook_seqname3,"ax"
	call _eu_seq_from_name;
/* SeqIds::name(type) entry -> eu_seq_name (returns "euclid" for type 3 while euclid is active) */
	.section .hook_seqtoname,"ax"
	jump.l _eu_seq_name;

/* input key "sampler?" checks -> rec_type_ok (dispatcher.c). Each replaces "CC = Rx == R(6); IF !CC JUMP no"
 * (4 bytes) with a CALL; the stub keeps all registers, returns to the next instruction (yes) or to `no`. */
	.section .hook_recA,"ax"
	call _rec_check_a;
	.section .hook_recB,"ax"
	call _rec_check_b;
	.section .hook_recend,"ax"
	call _rec_end_patch;
	.text
	.align 2
_rec_check_a:                       /* on_key @0x019AC8B4: type in R0, no -> 0x019AC8CC */
	[--SP] = RETS;
	[--SP] = (R7:0, P5:0);
	SP += -12;
	call _rec_type_ok;
	SP += 12;
	CC = R0 == 0;
	(R7:0, P5:0) = [SP++];
	RETS = [SP++];
	if CC jump 1f;
	rts;
1:	[--SP] = P0;
	P0.L = 0xC8CC;
	P0.H = 0x019A;
	RETS = P0;
	P0 = [SP++];
	rts;
_rec_check_b:                       /* on_key_shift @0x019ACC7C: type in R1, no -> 0x019ACC8C */
	[--SP] = RETS;
	[--SP] = (R7:0, P5:0);
	SP += -12;
	R0 = R1;
	call _rec_type_ok;
	SP += 12;
	CC = R0 == 0;
	(R7:0, P5:0) = [SP++];
	RETS = [SP++];
	if CC jump 1f;
	rts;
1:	[--SP] = P0;
	P0.L = 0xCC8C;
	P0.H = 0x019A;
	RETS = P0;
	P0 = [SP++];
	rts;

/* L1 tape resampler 0xFFA06194 prologue call-out (make_firmware.py L1_CALLS): replaces P1 = 0xFF8029DC;
 * P2 = 0xFF802154. Entered after LINK 0 + push (R7:6, P5:3); R0 = position (1/64 samples), R1 = increment,
 * R2 = gain increment pointer: all kept. ASTAT kept: the tape code sets CC (lp_index < 0) before the call and tests it
 * after (0xFFA0724E / 0xFFA0725C, assert tape_c.cpp:1039); the stock resampler never touches CC. The resampler sets every other register it uses afterwards, so the C call
 * (track outs, seq/... usbtrack.c) may clobber the C caller-saved registers. Caller frame: [FP] = tape FP,
 * [FP + 0x40] = tape [SP + 0x38] = samples read per block. */
	.text
	.global _l1_rs_probe
	.align 2
_l1_rs_probe:
	[--SP] = ASTAT;                           /* CC is live across the resampler call (tape code tests it after) */
	[--SP] = RETS;
	[--SP] = R0;
	[--SP] = R1;
	[--SP] = R2;
	SP += -12;
	call _tr_resample;
	SP += 12;
	R2 = [SP++];
	R1 = [SP++];
	R0 = [SP++];
	RETS = [SP++];
	ASTAT = [SP++];
	P1.L = 0x29dc; P1.H = 0xff80;
	P2.L = 0x2154; P2.H = 0xff80;
	rts;

/* Tape mix loops (L1): the per-track peak-meter MAX instruction is replaced by CALL to an L1 trampoline, which calls
 * these. They do the MAX and log {src, dest, track, N} for the track outs. Must preserve every register but R0.
 * Stack on entry: trampoline pushed RETS + P1 (8 bytes) -> tape SP = SP + 8. */
	.global _mix_hook_fwd
	.align 2
_mix_hook_fwd:                                /* loop 1 @0xFFA0709C: src P4, dest P3, track R1 */
	R0 = MAX (R2, R0) (V);
	[--SP] = P0; [--SP] = P5; [--SP] = R7; [--SP] = R6;
	R6 = P4; R7 = P3;
	[--SP] = R1;
	jump.s _mix_rec_common;
	.global _mix_hook_rev
	.align 2
_mix_hook_rev:                                /* loop 2 @0xFFA0712C: src P1 (+2), dest I1, track R3 */
	R0 = MAX (R0, R1) (V);
	[--SP] = P0; [--SP] = P5; [--SP] = R7; [--SP] = R6;   /* 16 bytes: the trampoline's saved P1 is now at SP + 16 */
	R6 = [SP + 16];                           /* the loop's P1: pushed by the trampoline (P1 = hook address here) */
	R6 += 2; R7 = I1;
	[--SP] = R3;
_mix_rec_common:                              /* stack: track, R6, R7, P5, P0 (20 bytes) + 8 -> tape SP = SP + 28 */
	[--SP] = ASTAT;                           /* CC is live in the mix loop (IF CC JUMP right after) */
	[--SP] = R5;
	[--SP] = R4;                              /* + 12 -> tape SP = SP + 40 */
	P5.L = _tr_rec; P5.H = _tr_rec;
	R5 = [P5];
	R4 = 16;
	CC = R5 < R4;
	if !CC jump _mix_rec_full;
	P0 = R5;
	R5 += 1;
	[P5] = R5;
	P0 = P0 << 2;
	P0 = P0 << 2;
	P5 = P5 + P0;
	[P5 + 4] = R6;                            /* src */
	[P5 + 8] = R7;                            /* dest */
	R5 = [SP + 12];                           /* track */
	[P5 + 12] = R5;
	R5 = [SP + 0x60];                         /* tape [SP + 0x38] = N: SP + 40 + 0x38 */
	[P5 + 16] = R5;
_mix_rec_full:
	R4 = [SP++]; R5 = [SP++]; ASTAT = [SP++];
	SP += 4;
	R6 = [SP++]; R7 = [SP++]; P5 = [SP++]; P0 = [SP++];
	rts;

/* L1 trampolines (plain LDR block at 0xFFA0BFB4, the free end of L1 instruction SRAM): pc-relative CALL range */
	.section .l1tramp,"ax"
	.global _l1_tramp_fwd
	.align 2
_l1_tramp_fwd:
	[--SP] = RETS; [--SP] = P1;
	P1.L = _mix_hook_fwd; P1.H = _mix_hook_fwd;
	call (P1);
	P1 = [SP++]; RETS = [SP++];
	rts;
	.global _l1_tramp_rev
	.align 2
_l1_tramp_rev:
	[--SP] = RETS; [--SP] = P1;
	P1.L = _mix_hook_rev; P1.H = _mix_hook_rev;
	call (P1);
	P1 = [SP++]; RETS = [SP++];
	rts;

/* USB 6 channels (screens/usbtrack.c) */
	.section .hook_usbdesc,"ax"
	.global __hook_usbdesc
__hook_usbdesc:
	call _usb6_desc_init;
	.section .hook_usbiso1,"ax"
	.global __hook_usbiso1
__hook_usbiso1:
	call _usb6_iso_in_silent;
	.section .hook_ciservice,"ax"
	.global __hook_ciservice
__hook_ciservice:
	call _ci_service;
	.section .hook_taperase,"ax"
	.global __hook_taperase
__hook_taperase:
	call _fast_tape_erase;
/* tape edit events (kennel/kn_tape.c): CALL sites in the clipboard service, its main-loop call, split and join */
	.section .hook_clipsvc,"ax"
	.global __hook_clipsvc
__hook_clipsvc:
	call _kn_clip_service;
	.section .hook_clipdrop,"ax"
	.global __hook_clipdrop
__hook_clipdrop:
	call _kn_clip_drop_add;
	.section .hook_clipdropall,"ax"
	.global __hook_clipdropall
__hook_clipdropall:
	call _kn_clip_dropall_add;
	.section .hook_cliplift,"ax"
	.global __hook_cliplift
__hook_cliplift:
	call _kn_clip_lift;
	.section .hook_cliplftall,"ax"
	.global __hook_cliplftall
__hook_cliplftall:
	call _kn_clip_lift_all;
	.section .hook_split1,"ax"
	.global __hook_split1
__hook_split1:
	call _kn_tape_split;
	.section .hook_split2,"ax"
	.global __hook_split2
__hook_split2:
	call _kn_tape_split;
	.section .hook_split3,"ax"
	.global __hook_split3
__hook_split3:
	call _kn_tape_split;
	.section .hook_join,"ax"
	.global __hook_join
__hook_join:
	call _kn_tape_join;
	.section .hook_recadd1,"ax"
	.global __hook_recadd1
__hook_recadd1:
	call _kn_rec_clip_add;
	.section .hook_recadd2,"ax"
	.global __hook_recadd2
__hook_recadd2:
	call _kn_rec_clip_add;
	.section .hook_recadd3,"ax"
	.global __hook_recadd3
__hook_recadd3:
	call _kn_rec_clip_add;
	.section .hook_recadd4,"ax"
	.global __hook_recadd4
__hook_recadd4:
	call _kn_rec_clip_add;
	.section .hook_recadd5,"ax"
	.global __hook_recadd5
__hook_recadd5:
	call _kn_rec_clip_add;
	.section .hook_recstart,"ax"
	.global __hook_recstart
__hook_recstart:
	call _kn_rec_start;
/* tape writeback and disk mode flush (kennel/kn_tape.c) */
	.section .hook_tapewrite,"ax"
	.global __hook_tapewrite
__hook_tapewrite:
	call _kn_tape_write;
	.section .hook_disk1,"ax"
	.global __hook_disk1
__hook_disk1:
	call _kn_disk_toggle;
	.section .hook_disk2,"ax"
	.global __hook_disk2
__hook_disk2:
	call _kn_disk_toggle;
	.section .hook_disk3,"ax"
	.global __hook_disk3
__hook_disk3:
	call _kn_disk_toggle;
	.section .hook_disk4,"ax"
	.global __hook_disk4
__hook_disk4:
	call _kn_disk_toggle;
/* snapshot save hook (kennel/kn_save.c): the global controller's hold-to-save pass, and its two date-stamp
 * snprintf calls (synth / drum) -> _kn_stamp_stub: stock snprintf unless a feature's name is armed */
	.section .hook_savesvc,"ax"
	.global __hook_savesvc
__hook_savesvc:
	call _kn_save_service;
	.section .hook_stampsyn,"ax"
	.global __hook_stampsyn
__hook_stampsyn:
	call _kn_stamp_stub;
	.section .hook_stampdrm,"ax"
	.global __hook_stampdrm
__hook_stampdrm:
	call _kn_stamp_stub;
	.text
	.align 2
	.global _kn_stamp_stub
_kn_stamp_stub:
	P0.L = _kn_snap_armed; P0.H = _kn_snap_armed;
	P0 = [P0];
	CC = P0 == 0;
	IF !CC JUMP _kn_stamp_mine;
	P0.L = 0xce04; P0.H = 0x01a9;		/* snprintf: arguments untouched (5 on the stack) */
	JUMP (P0);
_kn_stamp_mine:
	JUMP.L _kn_snap_name;			/* (dst, size): the name */
/* tape hook point (kennel/kn_tape.c): note input observers at VoiceCtl key_down / key_up's entries (LINK 0x10 /
 * LINK 0x20), the tape draw's recording-state CALL, tape_c's chunk lookup entry (LINK 0, called from the L1 tape
 * tick). Each entry stub first reads its core pointer: 0 = the original first instruction, then on (stock but for
 * P0 and a few cycles; checked in R246: all three bodies write P0 before reading it, key_down takes R0 / R1 only,
 * key_up R0). Otherwise the C side runs with RETS, ASTAT, the arguments and both hardware loops kept (the callers are
 * VisualDSP code). */
	.section .hook_vcdown,"ax"
	jump.l _kn_vcdown_stub;
	.section .hook_vcup,"ax"
	jump.l _kn_vcup_stub;
	.section .hook_tapepre,"ax"
	call _kn_tape_pre;
	.section .hook_tlookup,"ax"
	jump.l _kn_lookup_stub;
	.text
	.align 2
_kn_vcdown_stub:
	[--SP] = ASTAT;
	P0.L = _kn_note_obs; P0.H = _kn_note_obs;
	P0 = [P0];
	CC = P0 == 0;
	IF CC JUMP _kn_vcdown_stock;
	[--SP] = RETS; [--SP] = R0; [--SP] = R1;
	[--SP] = LC0; [--SP] = LT0; [--SP] = LB0; [--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
	R2 = RETS;			/* caller: the note's source (kn_midi.c) */
	SP += -12;
	call _kn_note_input_down;
	SP += 12;
	LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++]; LB0 = [SP++]; LT0 = [SP++]; LC0 = [SP++];
	R1 = [SP++]; R0 = [SP++]; RETS = [SP++];
_kn_vcdown_stock:
	ASTAT = [SP++];
	LINK 0x10;
	P0.L = 0x5754; P0.H = 0x01a8;
	JUMP (P0);
	.align 2
_kn_vcup_stub:
	[--SP] = ASTAT;
	P0.L = _kn_note_obs; P0.H = _kn_note_obs;
	P0 = [P0];
	CC = P0 == 0;
	IF CC JUMP _kn_vcup_stock;
	[--SP] = RETS; [--SP] = R0;
	[--SP] = LC0; [--SP] = LT0; [--SP] = LB0; [--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
	R1 = RETS;			/* caller (key_up takes R0 only) */
	SP += -12;
	call _kn_note_input_up;
	SP += 12;
	LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++]; LB0 = [SP++]; LT0 = [SP++]; LC0 = [SP++];
	R0 = [SP++]; RETS = [SP++];
_kn_vcup_stock:
	ASTAT = [SP++];
	LINK 0x20;
	P0.L = 0x5844; P0.H = 0x01a8;
	JUMP (P0);
	.align 2
_kn_lookup_stub:
	[--SP] = ASTAT;
	P0.L = _kn_lookup_owner; P0.H = _kn_lookup_owner;
	P0 = [P0];
	CC = P0 == 0;
	IF CC JUMP _kn_lookup_stock;
	[--SP] = RETS;
	[--SP] = LC0; [--SP] = LT0; [--SP] = LB0; [--SP] = LC1; [--SP] = LT1; [--SP] = LB1;
	SP += -12;
	call _kn_tape_lookup;
	SP += 12;
	LB1 = [SP++]; LT1 = [SP++]; LC1 = [SP++]; LB0 = [SP++]; LT0 = [SP++]; LC0 = [SP++];
	RETS = [SP++];
	ASTAT = [SP++];
	rts;
_kn_lookup_stock:
	ASTAT = [SP++];
	.global _kn_lookup_orig
_kn_lookup_orig:                              /* the stock lookup, callable: its first instruction, then the rest */
	LINK 0;
	P0.L = 0x4d60; P0.H = 0x01a5;
	JUMP (P0);
	.section .hook_dpkeep,"ax"
	.global __hook_dpkeep
__hook_dpkeep:
	call _dp_keep_op1r;
	.section .hook_dpbrowse,"ax"
	.global __hook_dpbrowse
__hook_dpbrowse:
	call _dp_browser_id;
	.section .hook_dpload,"ax"
	.global __hook_dpload
__hook_dpload:
	call _dp_load_parse;
/* FX2 routing (screens/fxroute.c, op1r 44): the LFO / seq tick 0x019C9D04 (P1 = 0x01DD591E; B[P1] = 0; RTS; its caller
 * is L1 core_block_process right after the tape playback) -> fr_snap, which also does the tick's stock body */
	.section .hook_frsnap,"ax"
	jump.l _fr_snap;
/* CALL parse_patch_json in the synth slot loader (0x01A051F8) -> synth_load_parse (kennel/kn_patch_safe.c) */
	.section .hook_synthload,"ax"
	.global __hook_synthload
__hook_synthload:
	call _synth_load_parse;
/* sampler preset guard (kennel/kn_sampler_guard.c): patch_table_loader's user-row branch for "sampler count over the
 * limit" (0x01A05984, R0.L = the patch.cpp:629 text; then CALL panic) -> count back to the limit, row skipped. R1 still
 * holds the limit (the compare's imm7); R0 / P0 are dead there (the stock branch calls panic). 0x01A0596A is the
 * loop's next row (it reloads R4 / R7), past the all-patches count, so the skipped entry is not kept. */
	.section .hook_smpover,"ax"
	.global __hook_smpover
__hook_smpover:
	jump.l _kn_sampler_over_stub;
	.text
	.align 2
	.global _kn_sampler_over_stub
_kn_sampler_over_stub:
	R0 = R1;
	call _kn_sampler_over;
	P0.L = 0x596a; P0.H = 0x01a0;
	jump (P0);
/* disk-mode export (0x0199DA6C), sampler row: CALL yaffs_open at 0x0199DD7A, whose handle R246 never closes ->
 * kn_export_probe_open: the same open, closed again (kennel/kn_sampler_guard.c) */
	.section .hook_diskprobe,"ax"
	.global __hook_diskprobe
__hook_diskprobe:
	call _kn_export_probe_open;
	.section .hook_sqlcache,"ax"
	.global __hook_sqlcache
__hook_sqlcache:
	call _sqlite_cache_hook;
	.section .hook_usbiso2,"ax"
	.global __hook_usbiso2
__hook_usbiso2:
	call _usb6_iso_in;
	.section .hook_usbiso3,"ax"
	.global __hook_usbiso3
__hook_usbiso3:
	call _usb6_iso_in_silent;
	.section .hook_usbbuf1,"ax"
	.global __hook_usbbuf1
__hook_usbbuf1:
	R2 = 0x33c (X);
	.section .hook_usbbuf2,"ax"
	.global __hook_usbbuf2
__hook_usbbuf2:
	R2 = 0x33c (X);

/* ---- L1 vector: fixed entry points for the calls patched into the encrypted L1 code ----
 * Placed first in .text (link.ld), so its addresses (CODE_BASE + 4 * i) never move: the encrypted L1 patches are
 * then constant and ship as precomputed bytes (l1_patches.json); plugins can change without regenerating them.
 * JUMP.L keeps all registers, RETS and ASTAT: the target returns straight to the L1 caller. Append only. */
	.section .l1vec, "ax"
	.global _l1v_metro, _l1v_push_lr, _l1v_push_interleaved, _l1v_rs_probe
_l1v_metro:            JUMP.L _ci_metro_render
_l1v_push_lr:          JUMP.L _usb6_push_lr
_l1v_push_interleaved: JUMP.L _usb6_push_interleaved
_l1v_rs_probe:         JUMP.L _l1_rs_probe

/* Instrument fade: original envelope fast path when idle. Adapted from op1r-src hook_spadsr. */
.section .hook_knfadeadsr,"ax"
jump.l _kn_sound_adsr_entry;
.text
.global _kn_sound_adsr_entry
.align 2
_kn_sound_adsr_entry:
 P1.L = _kn_fade_live; P1.H = _kn_fade_live;
 R3 = B[P1] (Z);
 CC = R3 == 0;
 IF !CC JUMP 1f;
 P1.L = _kn_sampler_adsr_live; P1.H = _kn_sampler_adsr_live;
 R3 = B[P1] (Z);
 CC = R3 == 0;
 IF !CC JUMP 1f;
 [--SP] = (R7:4, P5:3);
 P5 = R2;
 jump.l _kn_sound_adsr_cont;
1: jump.l _kn_sound_adsr;
.global _kn_sound_adsr_stock
.align 2
_kn_sound_adsr_stock:
 [--SP] = (R7:4, P5:3);
 P5 = R2;
 jump.l _kn_sound_adsr_cont;

.section .hook_lander1,"ax"
call _lander_stub;
.section .hook_lander2,"ax"
call _lander_stub;
.section .hook_lander3,"ax"
call _lander_stub;

/* Physical-key note construction and octave release/replay. */
.section .hook_keydown,"ax"
call _kn_keys_down;
.section .hook_keyup,"ax"
call _kn_keys_up;
.section .hook_keyoct0,"ax"
call _kn_keys_octave;
.section .hook_keyoct1,"ax"
call _kn_keys_octave;
.section .hook_keyoct2,"ax"
call _kn_keys_octave;
.section .hook_keyoct3,"ax"
call _kn_keys_octave;
.section .hook_keyoct4,"ax"
call _kn_keys_octave;
.section .hook_keyoct5,"ax"
call _kn_keys_octave;

/* VoiceCtl reset: replay its original LINK 0 before resuming. */
.section .hook_notesreset,"ax"
jump.l _kn_notes_reset;
.text
.global _kn_notes_reset_stock
.align 2
_kn_notes_reset_stock:
 LINK 0;
 jump.l _kn_notes_reset_cont;

/* Stock slot selection transposes held voices just like the octave path. */
.section .hook_keyslot,"ax"
jump.l _kn_keys_slot;
.text
.global _kn_keys_slot_stock
.align 2
_kn_keys_slot_stock:
 LINK 0x10;
 jump.l _kn_keys_slot_cont;

/* All preset import paths share native sound repair before returning to the caller. */
.section .hook_pparse1,"ax"
call _patch_parse_safe;
.section .hook_pparse2,"ax"
call _patch_parse_safe;
.section .hook_pparse3,"ax"
call _patch_parse_safe;

/* Immutable per-build option, inside the inventory core CRC range. The workshop
 * patches this word in a checked full-firmware variant, never at runtime. */
.section .rodata
.align 4
.global _kn_lander_enabled
_kn_lander_enabled:
.long 0
