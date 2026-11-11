# The wavefolder port's host test (fold_compare.cc): plugins/wavefolder against te-op1-cfw's ports/fx/src/fold as
# its kennel plugin drove it, bit for bit, natively (the C kernels) and in the Blackfin simulator (the assembly).
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/wavefolder all sim'
# From a worktree, point TE at the te-op1-cfw checkout: make TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports ...
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/wavefolder
BUILD ?= ../../../build/host/wavefolder
BLOCKS ?= 20000
SIM_BLOCKS ?= 6000
# e8: the position buffers (../fx_scratch_host.cc), sized from plugin.json "scratch"
SCRATCH := $(shell python3 -c "import json; print(json.load(open('$(COOP)/plugin.json'))['scratch'])")
CXXFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -Wall -Wextra
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIMFLAGS := -DCOOP_FX_SCRATCH_BYTES=$(SCRATCH) -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -DFOLD_CYCLES
COOP_SRC := $(COOP)/wavefolder.cc $(COOP)/src/fold.cc ../fx_scratch_host.cc
REF_SRC := $(TE)/fx/src/fold/fold.cc

.PHONY: all sim clean
all: $(BUILD)/fold_ref $(BUILD)/fold_coop
	$(BUILD)/fold_ref $(BUILD)/ref.raw $(BLOCKS)
	$(BUILD)/fold_coop $(BUILD)/coop.raw $(BLOCKS)
	cmp $(BUILD)/ref.raw $(BUILD)/coop.raw && echo "wavefolder (host, C kernels): coop == te-op1-cfw, bit-identical"

$(BUILD)/fold_ref: fold_compare.cc
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DFOLD_REF -I$(TE)/fx fold_compare.cc $(REF_SRC) -o $@

$(BUILD)/fold_coop: fold_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -I$(COOP) -I../../../sdk fold_compare.cc $(COOP_SRC) -o $@

# The Blackfin builds (the assembly kernels, as on the device) in the GNU simulator, with the cycle counts; their
# output must equal each other and the native run of the same length.
$(BUILD)/fold_ref.elf: fold_compare.cc
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -DFOLD_REF -I$(TE)/fx fold_compare.cc $(REF_SRC) $(TE)/fx/src/fold/fold_bfin.S -o $@

$(BUILD)/fold_coop.elf: fold_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) -I$(COOP) -I../../../sdk fold_compare.cc $(COOP_SRC) $(COOP)/src/fold_bfin.S $(COOP)/src/bus_bfin.S -o $@

sim: $(BUILD)/fold_ref.elf $(BUILD)/fold_coop.elf $(BUILD)/fold_ref
	$(BFIN)/bfin-elf-run $(BUILD)/fold_ref.elf $(BUILD)/ref_sim.raw $(SIM_BLOCKS)
	$(BFIN)/bfin-elf-run $(BUILD)/fold_coop.elf $(BUILD)/coop_sim.raw $(SIM_BLOCKS)
	$(BUILD)/fold_ref $(BUILD)/ref_short.raw $(SIM_BLOCKS) > /dev/null
	cmp $(BUILD)/ref_sim.raw $(BUILD)/coop_sim.raw && echo "wavefolder (simulator, assembly): coop == te-op1-cfw, bit-identical"
	cmp $(BUILD)/ref_short.raw $(BUILD)/coop_sim.raw && echo "wavefolder: simulator (assembly) == host (C), bit-identical"

clean:
	rm -rf $(BUILD)
