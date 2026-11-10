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

.PHONY: all clean
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

clean:
	rm -rf $(BUILD)
