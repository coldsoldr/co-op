/* Custom code in SDRAM 0x01E00000.. (free: image+bss end 0x01DFFF34, heap0 0x100..0x1000000,
   heap1 0x03040000.., ICPLB maps 0..64MB). .hook_* sections are patched into the original image. */
ENTRY(_synth_render)
SECTIONS {
  . = 0x01E00000;
  .text   : { KEEP(*(.l1vec)) *(.text*) }       /* .l1vec first: fixed L1 entry points (hook.S) */
  .rodata : { *(.rodata*) __core_ro_end = .; }   /* code + constants end: the core's identity CRC (kn_inv.c) */
  .data   : { *(.data*) }
  __image_end = .;
  /* zero-initialised data lives apart from the code block, as an LDR FILL block (no bytes in the image): the loaded
     image stays below 0x01E40000, reserved for other firmware modules. 0x01EC0000.. is free
     SDRAM (stock image ends 0x01DFFF34; heaps at 0x100.. and 0x03040000..); make_firmware asserts the end. */
  /* .farbss: large zero-initialised core tables (kn_mem.h kn_farbss), a second LDR FILL block in the free range
     0x01E40000..0x01EC0000 (kn_mem.h KN_FARBSS_LO..HI). It follows the image when a test core's image runs past
     0x01E40000 (harness/matrix.py "builtin"); make_firmware asserts the release layout. Before .bss: the first
     matching rule takes .bss.kn_far. .bss starts with .bss.kn_hot (kn_mem.h kn_hot), line-aligned at both ends. */
  .farbss MAX(0x01E40000, ALIGN(__image_end, 32)) (NOLOAD) : { __farbss_start = .; *(.bss.kn_far) . = ALIGN(4); __farbss_end = .; }
  .bss 0x01EC0000 (NOLOAD) : { __bss_start = .; . = ALIGN(32); __kn_hot_start = .; *(.bss.kn_hot) . = ALIGN(32); __kn_hot_end = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
  .l1tramp 0xFFA0BFB4 : { KEEP(*(.l1tramp)) }    /* free end of L1 instruction SRAM (image ends 0xFFA0BFB4) */
  .hook_keyslot 0x01A86420 : { KEEP(*(.hook_keyslot)) }
  _kn_keys_slot_cont = 0x01A86424;
  .hook_notesreset 0x01A862D4 : { KEEP(*(.hook_notesreset)) }
  _kn_notes_reset_cont = 0x01A862D8;
  .hook_keydown 0x019AC288 : { KEEP(*(.hook_keydown)) }
  .hook_keyup 0x019AC3A0 : { KEEP(*(.hook_keyup)) }
  .hook_keyoct0 0x01A17046 : { KEEP(*(.hook_keyoct0)) }
  .hook_keyoct1 0x01A170E0 : { KEEP(*(.hook_keyoct1)) }
  .hook_keyoct2 0x01A42744 : { KEEP(*(.hook_keyoct2)) }
  .hook_keyoct3 0x01A427C8 : { KEEP(*(.hook_keyoct3)) }
  .hook_keyoct4 0x01A510AC : { KEEP(*(.hook_keyoct4)) }
  .hook_keyoct5 0x01A510F0 : { KEEP(*(.hook_keyoct5)) }
  .hook_lander1 0x01A3BDB0 : { KEEP(*(.hook_lander1)) }
  .hook_lander2 0x019C35F0 : { KEEP(*(.hook_lander2)) }
  .hook_lander3 0x019C369C : { KEEP(*(.hook_lander3)) }
  .hook_knfadeadsr 0x01912128 : { KEEP(*(.hook_knfadeadsr)) }
  _kn_sound_adsr_cont = 0x0191212C;
  .hook_namecmp 0x019B975C : { KEEP(*(.hook_namecmp)) }
  .hook_samplecheck 0x01A01844 : { KEEP(*(.hook_samplecheck)) }
  .hook_fxnamecmp 0x019AAD8E : { KEEP(*(.hook_fxnamecmp)) }
  .hook_iterread 0x019B9EF8 : { KEEP(*(.hook_iterread)) }
  .hook_screenctor 0x01A3BD58 : { KEEP(*(.hook_screenctor)) }
  .hook_screenload 0x019F07D2 : { KEEP(*(.hook_screenload)) }
  .hook_rfp019427b8 0x019427B8 : { KEEP(*(.hook_rfp019427b8)) }
  .hook_rfp01984f60 0x01984F60 : { KEEP(*(.hook_rfp01984f60)) }
  .hook_rfp019a74d4 0x019A74D4 : { KEEP(*(.hook_rfp019a74d4)) }
  .hook_rfp019b05f8 0x019B05F8 : { KEEP(*(.hook_rfp019b05f8)) }
  .hook_rfp019e6a48 0x019E6A48 : { KEEP(*(.hook_rfp019e6a48)) }
  .hook_rfp019e832c 0x019E832C : { KEEP(*(.hook_rfp019e832c)) }
  .hook_rfp01a1cfd2 0x01A1CFD2 : { KEEP(*(.hook_rfp01a1cfd2)) }
  .hook_rfp01a33fb2 0x01A33FB2 : { KEEP(*(.hook_rfp01a33fb2)) }
  .hook_keyev 0x019C2D3E : { KEEP(*(.hook_keyev)) }
  .hook_midicc 0x01A85C8A : { KEEP(*(.hook_midicc)) }
  .hook_midistart 0x019E39D6 : { KEEP(*(.hook_midistart)) }
  .hook_knobev 0x019C2E94 : { KEEP(*(.hook_knobev)) }
  .hook_chordon 0x01A85828 : { KEEP(*(.hook_chordon)) }
  .hook_chordoff 0x01A858D0 : { KEEP(*(.hook_chordoff)) }
  .hook_lforange 0x01A042A2 : { KEEP(*(.hook_lforange)) }
  .hook_knboot 0x019F02EE : { KEEP(*(.hook_knboot)) }
  .hook_knmaster 0x019F054A : { KEEP(*(.hook_knmaster)) }
  .hook_tapefinal 0x019F0BE2 : { KEEP(*(.hook_tapefinal)) }
  .hook_knunmount 0x019F0BEA : { KEEP(*(.hook_knunmount)) }
  .hook_gcrate 0x019F0C46 : { KEEP(*(.hook_gcrate)) }
  .hook_bgsqueue 0x019411B8 : { KEEP(*(.hook_bgsqueue)) }
  _bgs_queue_cont = 0x019411BC;    /* SaveSlotJob queue after its LINK 0 (bgsave.c) */
  .hook_bgsstop 0x019F0BDC : { KEEP(*(.hook_bgsstop)) }
  .hook_knscreens 0x019B2690 : { KEEP(*(.hook_knscreens)) }
  .hook_patchtype 0x01A03884 : { KEEP(*(.hook_patchtype)) }
  .hook_fxpatchtype 0x01A04030 : { KEEP(*(.hook_fxpatchtype)) }
  .hook_euplayer 0x01A0D150 : { KEEP(*(.hook_euplayer)) }
  .hook_seqname1 0x01A42A2E : { KEEP(*(.hook_seqname1)) }
  .hook_seqname2 0x01A42BDC : { KEEP(*(.hook_seqname2)) }
  .hook_seqname3 0x01A42CE0 : { KEEP(*(.hook_seqname3)) }
  .hook_seqtoname 0x01A422EC : { KEEP(*(.hook_seqtoname)) }
  .hook_lfoname1 0x019C9C3A : { KEEP(*(.hook_lfoname1)) }
  .hook_lfoname2 0x019C9BE2 : { KEEP(*(.hook_lfoname2)) }
  .hook_rlp0193de2a 0x0193DE2A : { KEEP(*(.hook_rlp0193de2a)) }
  .hook_rlp01968c34 0x01968C34 : { KEEP(*(.hook_rlp01968c34)) }
  .hook_rlp01981eda 0x01981EDA : { KEEP(*(.hook_rlp01981eda)) }
  .hook_rlp019e4a18 0x019E4A18 : { KEEP(*(.hook_rlp019e4a18)) }
  .hook_rlp01a2d388 0x01A2D388 : { KEEP(*(.hook_rlp01a2d388)) }
  .hook_rlp01a30c3c 0x01A30C3C : { KEEP(*(.hook_rlp01a30c3c)) }
  .hook_rlp01a48fe8 0x01A48FE8 : { KEEP(*(.hook_rlp01a48fe8)) }
  _midi_apply_synth = 0x019CCE68;   /* LfoMidi::apply_synth / apply_fx / apply_drum (quad LFO stubs) */
  _midi_apply_fx = 0x019CD104;     /* FX object (vtable 0x0191086C) */
  _midi_apply_drum = 0x019CD048;   /* drum object (vtable 0x0191084C) */
  .hook_usbdesc 0x01A6ED96 : { KEEP(*(.hook_usbdesc)) }
  .hook_usbiso1 0x019E151C : { KEEP(*(.hook_usbiso1)) }
  .hook_usbiso2 0x019E161C : { KEEP(*(.hook_usbiso2)) }
  .hook_usbiso3 0x019E1812 : { KEEP(*(.hook_usbiso3)) }
  .hook_ciservice 0x019F0A24 : { KEEP(*(.hook_ciservice)) }
  .hook_sqlcache 0x0196F942 : { KEEP(*(.hook_sqlcache)) }
  .hook_taperase 0x0191B9F0 : { KEEP(*(.hook_taperase)) }
  .hook_clipsvc 0x019F0A2A : { KEEP(*(.hook_clipsvc)) }
  .hook_clipdrop 0x0191B65A : { KEEP(*(.hook_clipdrop)) }
  .hook_clipdropall 0x0191B730 : { KEEP(*(.hook_clipdropall)) }
  .hook_cliplift 0x0191B89C : { KEEP(*(.hook_cliplift)) }
  .hook_cliplftall 0x0191B974 : { KEEP(*(.hook_cliplftall)) }
  .hook_split1 0x01A56F24 : { KEEP(*(.hook_split1)) }
  .hook_split2 0x01A57690 : { KEEP(*(.hook_split2)) }
  .hook_split3 0x01A5769C : { KEEP(*(.hook_split3)) }
  .hook_join 0x01A577C6 : { KEEP(*(.hook_join)) }
  .hook_recadd1 0x01A5828A : { KEEP(*(.hook_recadd1)) }
  .hook_recadd2 0x01A5830C : { KEEP(*(.hook_recadd2)) }
  .hook_recadd3 0x01A5838E : { KEEP(*(.hook_recadd3)) }
  .hook_recadd4 0x01A58432 : { KEEP(*(.hook_recadd4)) }
  .hook_recadd5 0x01A5857E : { KEEP(*(.hook_recadd5)) }
  .hook_recstart 0x01A584D0 : { KEEP(*(.hook_recstart)) }
  .hook_tapewrite 0x01A54842 : { KEEP(*(.hook_tapewrite)) }
  .hook_disk1 0x01964336 : { KEEP(*(.hook_disk1)) }
  .hook_disk2 0x0196443E : { KEEP(*(.hook_disk2)) }
  .hook_disk3 0x01964616 : { KEEP(*(.hook_disk3)) }
  .hook_disk4 0x01964CFE : { KEEP(*(.hook_disk4)) }
  .hook_savesvc 0x019F0A3C : { KEEP(*(.hook_savesvc)) }
  .hook_stampsyn 0x019AD220 : { KEEP(*(.hook_stampsyn)) }
  .hook_stampdrm 0x019AD5A6 : { KEEP(*(.hook_stampdrm)) }
  .hook_vcdown 0x01A85750 : { KEEP(*(.hook_vcdown)) }
  .hook_vcup 0x01A85840 : { KEEP(*(.hook_vcup)) }
  .hook_tapepre 0x01A5CFA8 : { KEEP(*(.hook_tapepre)) }
  .hook_tlookup 0x01A54D5C : { KEEP(*(.hook_tlookup)) }
  .hook_dpkeep 0x01A044A8 : { KEEP(*(.hook_dpkeep)) }
  .hook_dpload 0x01A04BC0 : { KEEP(*(.hook_dpload)) }
  .hook_frsnap 0x019C9D04 : { KEEP(*(.hook_frsnap)) }
  .hook_pparse1 0x01995680 : { KEEP(*(.hook_pparse1)) }
  .hook_pparse2 0x0199F210 : { KEEP(*(.hook_pparse2)) }
  .hook_pparse3 0x0199FA4E : { KEEP(*(.hook_pparse3)) }
  .hook_synthload 0x01A051F8 : { KEEP(*(.hook_synthload)) }
  .hook_smpover 0x01A05984 : { KEEP(*(.hook_smpover)) }
  .hook_diskprobe 0x0199DD7A : { KEEP(*(.hook_diskprobe)) }
  .hook_dpbrowse 0x01977900 : { KEEP(*(.hook_dpbrowse)) }
  .hook_usbbuf1 0x01A69730 : { KEEP(*(.hook_usbbuf1)) }
  .hook_usbbuf2 0x01A69740 : { KEEP(*(.hook_usbbuf2)) }
  .hook_opzget 0x019E0CFC : { KEEP(*(.hook_opzget)) }
  .hook_recA 0x019AC8B4 : { KEEP(*(.hook_recA)) }
  .hook_recB 0x019ACC7C : { KEEP(*(.hook_recB)) }
  .hook_recend 0x019DEEF2 : { KEEP(*(.hook_recend)) }
  .hook_opzset 0x019E3540 : { KEEP(*(.hook_opzset)) }
  _midi_rx_parse_next = 0x019E348E;   /* end of the SysEx dispatch in midi_rx_parse */
  .hook_devboot 0x019F01B8 : { KEEP(*(.hook_devboot)) }
  .hook_devrx 0x019E3CC8 : { KEEP(*(.hook_devrx)) }
  .hook_inputbus 0x019674F8 : { KEEP(*(.hook_inputbus)) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
  .hook_dsfactory 0x1967984 : { KEEP(*(.hook_dsfactory)) }
  .hook_dstype 0x19b9810 : { KEEP(*(.hook_dstype)) }
  .hook_dsnormalize 0x1a037a8 : { KEEP(*(.hook_dsnormalize)) }
  .hook_dsscreen 0x1a3bfee : { KEEP(*(.hook_dsscreen)) }
  .hook_dsjsonsave 0x19fffba : { KEEP(*(.hook_dsjsonsave)) }
  .hook_dsjsonload 0x1a00630 : { KEEP(*(.hook_dsjsonload)) }
  .hook_dspresetquery 0x1a0502a : { KEEP(*(.hook_dspresetquery)) }
  .hook_dspresetcopy 0x193f5c2 : { KEEP(*(.hook_dspresetcopy)) }
  .hook_dssnapread 0x19ad554 : { KEEP(*(.hook_dssnapread)) }
  .hook_dssnapsave 0x19ad776 : { KEEP(*(.hook_dssnapsave)) }
  _kn_ds_stock_trigger = 0x1970de6;
  _kn_ds_stock_render = 0x1970ee8;
  _kn_ds_stock_release = 0x1970e7e;
  _kn_ds_stock_status = 0x1970e5c;
}
