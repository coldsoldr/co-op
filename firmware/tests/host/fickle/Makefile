# fickle's host test (fickle_test.cc): the plugin as it ships, on the host and in the Blackfin GNU simulator.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/fickle'        stress + holds + e8 check
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/fickle wav'    WAVs in build/fickle_wav
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/fickle sim'    cycles in bfin-elf-run
HOST_CXX ?= g++
COOP := ../../../plugins/fickle
BUILD ?= ../../../build/host/fickle
WAVDIR ?= ../../../build/fickle_wav
BLOCKS ?= 60000
SEED ?= 1
SIM_BLOCKS ?= 600
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
CXXFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wextra -Wno-unused-function
SRC := fickle_test.cc $(COOP)/fickle.cc $(COOP)/fickle_screen.cc ../fx_scratch_host.cc ../screen_host.cc
DEPS := $(SRC) $(wildcard $(COOP)/*.h) ../../../sdk/coop_plugin.h ../../../sdk/coop_fx_screen.h ../../../sdk/coop_screen.h
INC := -I$(COOP) -I../../../sdk

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti \
            -fno-tree-loop-distribute-patterns -Wall

.PHONY: all wav sim prof screens clean
all: $(BUILD)/fickle_test
	$(BUILD)/fickle_test stress $(BLOCKS) $(SEED)

wav: $(BUILD)/fickle_test
	@mkdir -p $(WAVDIR)
	$(BUILD)/fickle_test wav $(WAVDIR)

$(BUILD)/fickle_test: $(DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) $(INC) $(SRC) -o $@

# The simulator: the plugin as the packer compiles it (-O2, bf524), the CYCLES register around each render.
$(BUILD)/fickle_test.elf: $(DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(INC) $(SRC) -o $@

sim: $(BUILD)/fickle_test.elf
	$(BFIN)/bfin-elf-run $(BUILD)/fickle_test.elf sim $(SIM_BLOCKS)

clean:
	rm -rf $(BUILD)

# The same with each pass's cycles (FICKLE_PROFILE: CYCLES reads between the passes, a few dozen cycles more a block).
$(BUILD)/fickle_prof.elf: $(DEPS)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DFICKLE_PROFILE $(INC) $(SRC) -o $@

prof: $(BUILD)/fickle_prof.elf
	$(BFIN)/bfin-elf-run $(BUILD)/fickle_prof.elf sim $(SIM_BLOCKS)

# Screen frames for the owner (no op1emu needed): the plugin driven into four scenes, its screen run with the recording
# stand-in for the core's screen calls (screen_rec.cc), drawn with fickle.svg to PNG by render_frames.py.
FRAMEDIR ?= ../../../build/fickle_screens
$(BUILD)/fickle_frames: $(DEPS) screen_rec.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DFICKLE_FRAMES $(INC) fickle_test.cc $(COOP)/fickle.cc $(COOP)/fickle_screen.cc \
	  ../fx_scratch_host.cc screen_rec.cc -o $@

screens: $(BUILD)/fickle_frames
	@mkdir -p $(FRAMEDIR)
	$(BUILD)/fickle_frames frames $(FRAMEDIR)/frames.txt
	python3 -I render_frames.py $(COOP)/fickle.svg $(FRAMEDIR)/frames.txt $(FRAMEDIR)
