# petri's host test (petri_test.cc): the stress run, the decay checks, the e8 scratch check and the listening takes on
# the host; the cycles a block per effect in the Blackfin simulator (the plugin's own .S kernels).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/petri'        host (+ WAVs)
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/petri sim'    simulator cycles
#   ... make -C tests/host/petri screens     screen previews: build/petri_screens/*.png
#   ... make -C tests/host/petri tables      regenerate plugins/petri/src/petri_tables.h
HOST_CXX ?= g++
COOP := ../../../plugins/petri
BUILD ?= ../../../build/host/petri
WAVDIR ?= ../../../build/petri_wav
BLOCKS ?= 60000
SIM_BLOCKS ?= 1500
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
# 0.3: the idle path is off here (COOP_FX_NO_IDLE), so the digest is comparable with the versions before it (after an
# idle time the filter and the space are an LSB or so apart); ../idle tests that path against this build
CXXFLAGS := -DCOOP_FX_NO_IDLE -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter
COOP_SRC := $(COOP)/petri.cc $(COOP)/petri_screen.cc $(COOP)/src/petri_engine.cc $(COOP)/src/halfband.cc \
            ../fx_scratch_host.cc ../screen_host.cc
COOP_DEPS := $(wildcard $(COOP)/*.cc $(COOP)/*.h $(COOP)/src/*) ../../../sdk/coop_fx_screen.h ../../../sdk/coop_screen.h \
             ../../../sdk/coop_plugin.h

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_NO_IDLE -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns

.PHONY: all sim screens tables clean
all: $(BUILD)/petri_test
	@mkdir -p $(WAVDIR)/wet
	$(BUILD)/petri_test $(WAVDIR) $(BLOCKS)

$(BUILD)/petri_test: petri_test.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk -I../../../core/include petri_test.cc $(COOP_SRC) -o $@

# The simulator: the plugin as it ships (its .S kernels; the clock stepped by the test).
$(BUILD)/petri_test.elf: petri_test.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DCOOP_PETRI_HOST_CLOCK -DCOOP_PETRI_REF -I$(COOP) -I../../../sdk -I../../../core/include petri_test.cc \
	  $(COOP_SRC) $(COOP)/src/halfband_bfin.S $(COOP)/src/voice_bfin.S $(COOP)/src/space_bfin.S -o $@

sim: $(BUILD)/petri_test.elf
	$(BFIN)/bfin-elf-run $(BUILD)/petri_test.elf - $(SIM_BLOCKS)

# The screen previews (petri_screens.cc): the screen driven over the running effect, frames rasterised to PNG.
SCREENDIR ?= ../../../build/petri_screens
$(BUILD)/petri_screens: petri_screens.cc $(COOP_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk -I../../../core/include petri_screens.cc $(COOP)/petri.cc \
	  $(COOP)/petri_screen.cc $(COOP)/src/petri_engine.cc $(COOP)/src/halfband.cc ../fx_scratch_host.cc -o $@

screens: $(BUILD)/petri_screens
	@mkdir -p $(SCREENDIR)
	$(BUILD)/petri_screens $(COOP)/petri.svg $(SCREENDIR)

tables:
	python3 $(COOP)/gen_petri_tables.py $(COOP)/src/petri_tables.h

clean:
	rm -rf $(BUILD)
