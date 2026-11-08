/* Phase 2 milestone 1: the coop audio-callback wrapper, linked into free SDRAM region A.
 * inject_app.py adds the load segments as LDR blocks and patches the audio-callback imm pair.
 *
 * p6c (docs/fx-data-placement-plan.md lever 6): the text is split. The per-block code (every .S file: the kernels,
 * the stage, the thunks; and the listed C++ functions: the callback, the stages, the effect renders, slot 2, the
 * voice hooks, LFO 2's process, the tape tap, the per-block stats) stays alone in region A's 4 MB instruction CPLB
 * page 0x02c00000, which the settings menu's "code prio" row marks LRUPRIO (coop_codeprio.cc). Everything else (the
 * UI, settings, SysEx, the loaders, the stats writer, libgcc) goes to region B 0x02810000, a page of its own
 * (inject_app.py COLD_*). Data stays in region A. A function missing from the hot list only misses as before; a
 * cold function wrongly listed hot is the harm (a sticky high line each time it runs), so keep the list per-block. */
OUTPUT_FORMAT("elf32-bfin")
ENTRY(_op1_header)
MEMORY { injected (rwx) : ORIGIN = 0x02c00000, LENGTH = 0x00400000
         cold (rx)      : ORIGIN = 0x02810000, LENGTH = 0x003f0000     /* region B: the cold text (inject_app.py COLD_*) */
         near (rwx)     : ORIGIN = 0x01e00400, LENGTH = 0x00000c00 }   /* region C window in pcrel24 reach of the stock (inject_app.py NEAR_*) */
SECTIONS {
  .header : { KEEP(*(.op1header)) } > injected
  .text.libgcc : { *libgcc.a:(.text .text.*) } > cold                  /* first match wins: libgcc's plain .text is cold */
  .text.hot : {
    *(.text)                                                            /* the .S files: kernels, stage, thunks */
    *(.text.*CoopAudioCb) *(.text.*coop_instr_stage) *(.text.*coop_mix_entry)
    *(.text.*voice_hook*) *(.text.*voice_sum_stock*) *(.text.*coop_synth_note_on_hook)
    *(.text.*coop_seam_fill) *(.text.*coop_seam_prologue) *(.text.*coop_drum_bus) *(.text.*drumbus_side*)
    *(.text.*coop_stereo_block_start) *(.text.*coop_stereo_side_rw) *(.text.*coop_stereo_side_bufs) *(.text.*coop_dbg_pre)
    *(.text.*coop_tank_grid_render) *(.text.*coop_tank_spring_render)
    *(.text.*coop_fx_render_mono) *(.text.*coop_fx_render_st) *(.text.*st_render*) *(.text.*fx_sanitize*)
    *(.text.*type_of_mono*) *(.text.*fx_index*) *(.text.*knob_map*)
    *(.text.*slot2_render*) *(.text.*coop_slot2_instr) *(.text.*coop_slot2_bypassed)
    *(.text.*coop_master_apply)
    *(.text.*coop_tape_record) *(.text.*coop_tape_gains) *(.text.*coop_tape_clamp_track)
    *(.text.*coop_lfo2_pre_render_*) *(.text.*coop_lfo2_trigger_*) *(.text.*ZL7process*) *(.text.*coop_sound_lfo2)
    *(.text.*coop_eng_cost) *(.text.*coop_eng_block_end) *(.text.*coop_stats_tick) *(.text.*coop_slow_keep)
    *(.text.*coop_gap_block) *(.text.*zero_costs*)
    *(.text.*coop_runner_block) *(.text.*coop_runner_click) *(.text.*coop_bend_block) *(.text.*coop_drumpan_q15)
    ___hot_end = .;
  } > injected
  .text   : { ___cold_start = .; *(.text.*) ___cold_end = .; } > cold
  .near   : { KEEP(*(.near)) } > near
  .rodata : { *(.rodata .rodata.*) } > injected
  .data   : { *(.data .data.*) } > injected
  .bss (NOLOAD) : ALIGN(32) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > injected
  ___image_end = .;
  /DISCARD/ : { *(.init_array*) *(.ctors*) *(.comment) *(.eh_frame*) *(.fini_array*) *(.dtors*) }
}
