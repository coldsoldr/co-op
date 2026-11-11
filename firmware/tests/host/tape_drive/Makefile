# The tape_drive port's host test (tape_drive_compare.cc): plugins/tape_drive against te-op1-cfw's ports/fx/src/tape
# as the kennel build drove it, bit for bit; then the plugin as it ships (assembly kernels) in the Blackfin simulator,
# bit for bit again, with its cycles per render.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/tape_drive'
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports/fx
COOP := ../../../plugins/tape_drive
BUILD ?= ../../../build/host/tape_drive
BLOCKS ?= 20000
# e8: the position buffers (../fx_scratch_host.cc), sized from plugin.json "scratch"
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
CXXFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-unused-variable
COOP_SRC := $(COOP)/tape_drive.cc $(COOP)/src/tape.cc ../fx_scratch_host.cc

BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns

.PHONY: all clean
all: $(BUILD)/tape_ref $(BUILD)/tape_coop $(BUILD)/tape_coop.elf
	$(BUILD)/tape_ref $(BUILD)/ref.raw $(BLOCKS)
	$(BUILD)/tape_coop $(BUILD)/coop.raw $(BLOCKS)
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "tape_drive (host, C kernels): coop == te-op1-cfw, bit-identical"
	$(BFIN)/bfin-elf-run $(BUILD)/tape_coop.elf $(BUILD)/sim.raw $(BLOCKS)
	cmp $(BUILD)/ref.raw $(BUILD)/sim.raw && echo "tape_drive (simulator, assembly kernels): coop == te-op1-cfw, bit-identical"

$(BUILD)/tape_ref: tape_drive_compare.cc $(wildcard $(TE)/src/tape/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DTAPE_REF -I$(TE) tape_drive_compare.cc $(TE)/src/tape/tape.cc -o $@

$(BUILD)/tape_coop: tape_drive_compare.cc $(COOP_SRC) $(wildcard $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk tape_drive_compare.cc $(COOP_SRC) -o $@

$(BUILD)/tape_coop.elf: tape_drive_compare.cc $(COOP_SRC) $(wildcard $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP) -I../../../sdk tape_drive_compare.cc $(COOP_SRC) $(COOP)/src/tape_bfin.S -o $@
	@if $(BFIN)/bfin-elf-nm $@ | grep -q '_GLOBAL__sub_I.*tape'; then echo "a static initialiser"; exit 1; fi

clean:
	rm -rf $(BUILD)
