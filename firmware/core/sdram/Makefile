# Phase 2 milestone 1: the coop audio-callback wrapper (identity wrap + measure).
# Build (WSL): make -C core/sdram ; output build/coop_cb.elf, injected into region A.
BFIN_BIN ?= $(HOME)/op1-toolchain/bfin/bin
BFIN_CXX := $(BFIN_BIN)/bfin-elf-g++
BUILD    ?= ../../build/core
EXTRA_CXXFLAGS ?=
CXXFLAGS := $(EXTRA_CXXFLAGS) -O2 -std=c++17 -mcpu=bf524 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS  := -nostdlib -nostartfiles -T coop_cb.ld -Wl,--gc-sections -Wl,-z,max-page-size=0x400 -Wl,--no-undefined

.PHONY: all clean combined
all: $(BUILD)/coop_cb.elf
$(BUILD)/coop_cb.elf: coop_cb.cc coop_cb.ld | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) coop_cb.cc $(LDFLAGS) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@

# Combined baseline+suite image: m1's whole-callback timer (CoopAudioCb, imm-seam patched)
# plus the master suite (coop_master_apply, wired by tools/l1_insert.py), one header, region A.
combined: $(BUILD)/coop_combined.elf
STEREO_KEEP := -Wl,--undefined=_coop_synth_render_thunk -Wl,--undefined=_coop_drum_render_thunk -Wl,--undefined=_coop_mix_thunk -Wl,--undefined=_coop_synth_noteon_thunk -Wl,--undefined=_coop_core_noteon_thunk -Wl,--undefined=_coop_tape_rec_thunk -Wl,--undefined=_coop_tape_gains_thunk -Wl,--undefined=_coop_seam_prologue_thunk -Wl,--undefined=_coop_seam_fill -Wl,--undefined=_coop_lift -Wl,--undefined=_coop_drop -Wl,--undefined=_coop_erase -Wl,--undefined=_coop_tape_io_step -Wl,--undefined=_coop_tape_mount_and_index -Wl,--undefined=_coop_sysex_rx_poll -Wl,--undefined=_coop_key_event -Wl,--undefined=_coop_fx_name_eq -Wl,--undefined=_coop_fx_render_mono -Wl,--undefined=_coop_fx_init_mono -Wl,--undefined=_coop_fx_render_st -Wl,--undefined=_coop_fx_init_st -Wl,--undefined=_coop_tank_grid_render -Wl,--undefined=_coop_tank_grid_init -Wl,--undefined=_coop_tank_spring_render -Wl,--undefined=_coop_tank_spring_init -Wl,--undefined=_g_coop_voice_sum_own -Wl,--undefined=_coop_ui_frame -Wl,--undefined=_coop_ui_knob -Wl,--undefined=_coop_dsynth_voice_loop -Wl,--undefined=_coop_cwo_loop_near -Wl,--undefined=_coop_cwo_vec_ctor -Wl,--undefined=_coop_phone_loop_near -Wl,--undefined=_coop_phone_capture_near -Wl,--defsym=_stock_grid_render_back=0x019af84e -Wl,--defsym=_stock_grid_init_back=0x019af294 -Wl,--defsym=_stock_spring_render_back=0x01a3300a -Wl,--defsym=_stock_spring_init_back=0x01a336b8 -Wl,--defsym=_stock_dsynth_loop_back=0x0197ea6c -Wl,--defsym=_stock_dsynth_loop_entry=0x0197e95a -Wl,--defsym=_stock_cwo_loop_entry=0x019457d6 -Wl,--defsym=_stock_cwo_loop_back=0x01945ac8 -Wl,--defsym=_stock_vec_ctor=0x01aa3862 -Wl,--defsym=_stock_phone_loop_top=0x01a1bcb0 -Wl,--defsym=_stock_phone_loop_bottom=0x01a1bf34 -Wl,--defsym=_stock_phone_loop_back=0x01a1bf36 -Wl,--defsym=_stock_phone_capture_top=0x01a1bc00 -Wl,--defsym=_stock_phone_capture_bottom=0x01a1bc58 -Wl,--defsym=_stock_phone_capture_back=0x01a1bc5a -Wl,--undefined=_coop_punch_render_st -Wl,--defsym=_stock_punch_render=0x019e662c -Wl,--undefined=_coop_nitro_loop_near -Wl,--defsym=_stock_nitro_loop_top=0x019a64a8 -Wl,--defsym=_stock_nitro_loop_bottom=0x019a6682 -Wl,--defsym=_stock_nitro_loop_back=0x019a67f8 -Wl,--undefined=_coop_grid_loop_near -Wl,--defsym=_stock_grid_loop_top=0x019af932 -Wl,--defsym=_stock_grid_loop_bottom=0x019afcc8 -Wl,--defsym=_stock_grid_loop_back=0x019afcca -Wl,--undefined=_coop_delay_loop_near -Wl,--defsym=_stock_delay_loop_top=0x0197dd4a -Wl,--defsym=_stock_delay_loop_bottom=0x0197df7a -Wl,--defsym=_stock_delay_loop_back=0x0197df7c
$(BUILD)/coop_combined.elf: coop_cb.cc master_suite.cc stereo_bus.cc stereo_thunks.S cwo_fast.S delay_fast.S grid_fast.S nitro_fast.S phone_fast.S punch_fast.S coop_stage.S lfo2.cc tape_stereo.cc tape_io.cc tape_edit.cc coop_stats.cc l1_pool.cc coop_sysex.cc lander_off.cc plugin_load.cc plugin_host.cc coop_ui.cc ../../sdk/coop_exports.cc coop_cb.ld | $(BUILD)
	$(BFIN_CXX) $(CXXFLAGS) -DCOOP_COMBINED -I../include -I../../sdk coop_cb.cc master_suite.cc stereo_bus.cc stereo_thunks.S cwo_fast.S delay_fast.S grid_fast.S nitro_fast.S phone_fast.S punch_fast.S coop_stage.S lfo2.cc tape_stereo.cc tape_io.cc tape_edit.cc coop_stats.cc l1_pool.cc coop_sysex.cc lander_off.cc plugin_load.cc plugin_host.cc coop_ui.cc ../../sdk/coop_exports.cc $(LDFLAGS) -Wl,--undefined=_coop_master_apply $(STEREO_KEEP) -o $@ -lgcc
	$(BFIN_BIN)/bfin-elf-size $@
	$(BFIN_BIN)/bfin-elf-nm $@ | grep -iE "CoopAudioCb|coop_master_apply|coop_.*thunk|coop_.*hook|coop_mix_entry|g_coop_|g_max|g_blocks|g_master|g_fx|g_stereo|lfo2|tape" | sort
$(BUILD):
	mkdir -p $(BUILD)
clean:
	rm -f $(BUILD)/coop_cb.elf
