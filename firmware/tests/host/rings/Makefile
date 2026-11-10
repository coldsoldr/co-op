# The rings port's host test (rings_compare.cc): plugins/rings against te-op1-cfw's ports/rings, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/rings'
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports/rings
COOP := ../../../plugins/rings
BUILD ?= ../../../build/host/rings
RATE := -DOP1RINGS_SAMPLE_RATE=22050 -DOP1RINGS_CONTROL_BLOCK_SIZE=32 -DOP1RINGS_UPSAMPLE=4
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-variable -Wno-unused-function -Wno-class-memaccess $(RATE)
DSP := src/op1_voice.cc src/string_voice.cc src/string_fixed.cc src/reverb_fixed.cc src/fm_voice_fixed.cc \
       src/modal_resonator.cc src/fixed_tables.cc

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns \
            -Wno-class-memaccess $(RATE)
PROFILE_BLOCKS ?= 62010
PROFILE_MODEL ?=

.PHONY: all clean profile halfband-check halved batched
all: $(BUILD)/rings_ref $(BUILD)/rings_coop
	$(BUILD)/rings_ref $(BUILD)/ref.raw
	$(BUILD)/rings_coop $(BUILD)/coop.raw
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "rings: coop == te-op1-cfw, bit-identical"

# te-op1-cfw's own engine (the firmware build's engine.cc in its simulator mode: its screen, input and patch table
# only have to link).
$(BUILD)/rings_ref: rings_compare.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DRINGS_REF -DOP1_SIM -DOP1_HOST_SIM -I$(TE) rings_compare.cc $(TE)/op1/engine.cc \
	  $(TE)/op1/screen.cc $(TE)/op1/input.cc $(TE)/op1/patches.cc $(addprefix $(TE)/,$(DSP)) -o $@

$(BUILD)/rings_coop: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc \
	  $(addprefix $(COOP)/,$(DSP)) -o $@

# Compute cycles per call in the GNU simulator (no cache or SDRAM model): make profile [PROFILE_BLOCKS=n]
$(BUILD)/rings_profile.elf: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DOP1_COOP -DRINGS_PROFILE $(PROFILE_FLAGS) -I$(COOP) -I../../../sdk rings_compare.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) $(COOP)/src/mode_bank_bfin.S $(COOP)/src/string_kernel_bfin.S \
	  $(COOP)/src/limiter_bfin.S $(COOP)/src/reverb_bfin.S $(COOP)/src/halfband_bfin.S -o $@

profile: $(BUILD)/rings_profile.elf
	$(BFIN)/bfin-elf-run $< $(BUILD)/profile.raw $(PROFILE_BLOCKS) $(PROFILE_MODEL)

clean:
	rm -rf $(BUILD)

# The upsampler's half-band kernel against its C loop, in the simulator.
$(BUILD)/halfband_check.elf: halfband_check.cc $(COOP)/src/halfband_bfin.S $(COOP)/src/halfband_taps.h
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP) halfband_check.cc $(COOP)/src/halfband_bfin.S -o $@

halfband-check: $(BUILD)/halfband_check.elf
	$(BFIN)/bfin-elf-run $<

# The coop build as it ships (OP1RINGS_CONTROL_HALVED=1, plugin.json): close to te-op1-cfw, not bit-identical.
$(BUILD)/rings_halved: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -I$(COOP) -I../../../sdk rings_compare.cc \
	  $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

halved: $(BUILD)/rings_halved $(BUILD)/rings_ref
	$(BUILD)/rings_ref $(BUILD)/ref.raw
	$(BUILD)/rings_halved $(BUILD)/halved.raw
	python3 -I compare_raw.py $(BUILD)/ref.raw $(BUILD)/halved.raw
	python3 -I compare_spectra.py $(BUILD)/ref.raw.mix $(BUILD)/halved.raw.mix

# The batched control work (OP1RINGS_CONTROL_BATCHED) against the halved schedule alone, and the build as it ships
# (plugin.json: halved, batched, POLY4). `make batched` compares the first two in a steady state (no knob jumps or
# bends: the batch uses each voice's last block, so only a moving control lands differently).
$(BUILD)/rings_batched: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -I$(COOP) \
	  -I../../../sdk rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

$(BUILD)/rings_shipped: rings_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/* $(COOP)/op1/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_COOP -DOP1RINGS_CONTROL_HALVED=1 -DOP1RINGS_CONTROL_BATCHED=1 -DOP1RINGS_COOP_POLY4=1 \
	  -I$(COOP) -I../../../sdk rings_compare.cc $(COOP)/rings.cc $(addprefix $(COOP)/,$(DSP)) -o $@

batched: $(BUILD)/rings_halved $(BUILD)/rings_batched $(BUILD)/rings_shipped
	@for m in 0 1 2 3 4 5; do \
	  RINGS_NO_JUMP=1 RINGS_NO_BEND=1 $(BUILD)/rings_halved $(BUILD)/h$$m.raw 8000 $$m 3 > /dev/null; \
	  RINGS_NO_JUMP=1 RINGS_NO_BEND=1 $(BUILD)/rings_batched $(BUILD)/b$$m.raw 8000 $$m 3 > /dev/null; \
	  printf "model $$m, polyphony 4: "; python3 -I block_levels.py $(BUILD)/h$$m.raw $(BUILD)/b$$m.raw; done
	$(BUILD)/rings_shipped $(BUILD)/shipped.raw
