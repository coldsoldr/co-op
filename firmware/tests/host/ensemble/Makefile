# The ensemble port's test (ensemble_compare.cc): plugins/ensemble against te-op1-cfw's kennel ensemble, bit for bit.
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/ensemble'       (host, C kernels)
#   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/ensemble sim'   (bfin-elf-run,
#                                                       coop's assembly against the upstream C, cycles a render)
# TE: te-op1-cfw's ports folder (from a worktree: make TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports).
HOST_CXX ?= g++
TE ?= ../../../../../te-op1-cfw/ports
COOP := ../../../plugins/ensemble
BUILD ?= ../../../build/host/ensemble
BFIN ?= $(HOME)/op1-toolchain/bfin/bin
SIM_BLOCKS ?= 4000

TE_INC := -I$(TE)/knl -I$(TE) -I$(TE)/fx -I$(TE)/rings -I$(TE)/clouds
TE_SRC := $(TE)/fx/src/ensemble/ensemble.cc $(TE)/clouds/src/core/halfband.cc $(TE)/clouds/src/core/clouds_tables.cc \
          $(TE)/rings/src/fixed_tables.cc
COOP_SRC := $(COOP)/ensemble.cc $(COOP)/src/ensemble_core.cc $(COOP)/src/ensemble_tables.cc $(COOP)/src/halfband.cc
COOP_ASM := $(COOP)/src/ensemble_bfin.S $(COOP)/src/halfband_bfin.S
INC := -I$(COOP) -I../../../sdk $(TE_INC)
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-class-memaccess -DOP1CLOUDS_SAMPLE_RATE=22050
SIMFLAGS := -O2 -std=c++17 -mcpu=bf524 -msim -fno-exceptions -fno-rtti -fno-tree-loop-distribute-patterns \
            -Wno-class-memaccess -DOP1CLOUDS_SAMPLE_RATE=22050 -DOP1FX_NO_ASM -DOP1CLOUDS_NO_ASM

.PHONY: all sim clean
all: $(BUILD)/ensemble_compare
	$(BUILD)/ensemble_compare

$(BUILD)/ensemble_compare: ensemble_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(CXXFLAGS) -DOP1_HOST_SIM $(INC) ensemble_compare.cc $(COOP_SRC) $(TE_SRC) -o $@

# The upstream's own assembly is linked only because its Side() names it under __bfin__; OP1FX_NO_ASM runs its C.
$(BUILD)/ensemble_compare.elf: ensemble_compare.cc $(wildcard $(COOP)/*.cc $(COOP)/src/*)
	@mkdir -p $(BUILD)
	$(BFIN)/bfin-elf-g++ $(SIMFLAGS) $(INC) ensemble_compare.cc $(COOP_SRC) $(COOP_ASM) $(TE_SRC) \
	  $(TE)/fx/src/ensemble/ensemble_bfin.S -o $@

sim: $(BUILD)/ensemble_compare.elf
	$(BFIN)/bfin-elf-run $< $(SIM_BLOCKS)

clean:
	rm -rf $(BUILD)
